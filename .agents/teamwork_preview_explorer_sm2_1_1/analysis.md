# Architectural Analysis and Recommendations for Chronos Edge Tracker (SM 2.1)

## Executive Summary
This report provides architectural recommendations and implementation strategies for edge-daemon enhancements in Sub-Milestone 2.1. The analysis covers local SQLite3 buffering, network state detection via POSIX APIs, domain obfuscation, a Laplace DP calibration utility, and CMake build configuration, alongside a critical security advisory regarding shell injection.

---

## 1. Local SQLite3 Database Integration

### 1.1 Schema Design
To reliably buffer telemetry events when the device is offline, a lightweight table is required. The telemetry events represent aggregated window/domain durations. We recommend the following schema:

```sql
CREATE TABLE IF NOT EXISTS telemetry_buffer (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    metric_name TEXT NOT NULL,
    value REAL NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

*   `id`: Primary key with autoincrement to ensure chronological (FIFO) ordering.
*   `metric_name`: Stores the domain or app identifier (already anonymized or obfuscated).
*   `value`: The numeric duration/count (e.g. 10.0 seconds).
*   `created_at`: Metadata for auditing or time-window aggregation.

### 1.2 Connection Management (C++)
We recommend wrapping SQLite3 resources in C++ RAII classes to prevent database connection and statement leaks.

```cpp
#include <sqlite3.h>
#include <memory>
#include <string>
#include <stdexcept>

// Custom Deleters for smart pointers
struct Sqlite3Deleter {
    void operator()(sqlite3* db) const { sqlite3_close(db); }
};

struct Sqlite3StmtDeleter {
    void operator()(sqlite3_stmt* stmt) const { sqlite3_finalize(stmt); }
};

using Sqlite3DbPtr = std::unique_ptr<sqlite3, Sqlite3Deleter>;
using Sqlite3StmtPtr = std::unique_ptr<sqlite3_stmt, Sqlite3StmtDeleter>;

class TelemetryDatabase {
private:
    Sqlite3DbPtr db_;
    
public:
    explicit TelemetryDatabase(const std::string& db_path) {
        sqlite3* temp_db = nullptr;
        if (sqlite3_open(db_path.c_str(), &temp_db) != SQLITE_OK) {
            if (temp_db) sqlite3_close(temp_db);
            throw std::runtime_error("Failed to open SQLite database: " + db_path);
        }
        db_.reset(temp_db);
        initializeSchema();
    }

    void initializeSchema() {
        const char* sql = "CREATE TABLE IF NOT EXISTS telemetry_buffer ("
                          "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                          "metric_name TEXT NOT NULL, "
                          "value REAL NOT NULL, "
                          "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP);";
        char* err_msg = nullptr;
        int rc = sqlite3_exec(db_.get(), sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string err(err_msg);
            sqlite3_free(err_msg);
            throw std::runtime_error("Failed to initialize database schema: " + err);
        }
    }
};
```

### 1.3 Buffering Telemetry Flow
To prevent network operations from blocking the extension ingestion thread (since `main.cpp` listens on port 8888 and handles pings synchronously), a **Producer-Consumer / Queue-drain** architecture is recommended:
1.  **Ingestion (Main Thread):**
    *   Parses incoming telemetry ping.
    *   Applies domain obfuscation and differential privacy noise.
    *   Inserts the anonymized event into `telemetry_buffer` database.
    *   Immediately returns HTTP `200 OK` response to extension (non-blocking).
2.  **Transmission (Background Worker Thread):**
    *   Wakes up periodically (e.g. every 5–10 seconds) or when signaled.
    *   Checks if the network is online.
    *   If **Online**:
        *   Queries the oldest records: `SELECT id, metric_name, value FROM telemetry_buffer ORDER BY id ASC LIMIT 10;`.
        *   Iterates through records and transmits each via HTTP.
        *   If transmission succeeds, deletes the record: `DELETE FROM telemetry_buffer WHERE id = ?;`.
        *   If transmission fails (network dropped), halts processing, sleeps, and retries later.
    *   If **Offline**:
        *   Enters sleep mode waiting for network recovery.

---

## 2. Crostini Network State Checker

### 2.1 API Selection: `getifaddrs` and Sockets
In the Crostini virtualized environment, checking connectivity can be divided into a dual-tiered check:
1.  **Interface State (L1 Check):** Use `getifaddrs()` to ensure that at least one network interface is up, running, and assigned a non-loopback IP address.
2.  **Internet Routing (L2 Check):** Perform a non-blocking `connect()` on a UDP socket to a public IP (e.g. `8.8.8.8` DNS server on port 53). A UDP connect is purely local (routing table look-up) and fails instantly if no route to the internet is configured, avoiding long blocking delays.

### 2.2 C++ Implementation Sketch
```cpp
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

class NetworkChecker {
public:
    static bool isInterfaceUp() {
        struct ifaddrs* ifaddr = nullptr;
        if (getifaddrs(&ifaddr) == -1) return false;

        bool is_up = false;
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            
            int family = ifa->ifa_addr->sa_family;
            if (family == AF_INET || family == AF_INET6) {
                bool up_flags = (ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING);
                bool loopback = (ifa->ifa_flags & IFF_LOOPBACK);
                
                if (up_flags && !loopback) {
                    is_up = true;
                    break;
                }
            }
        }
        freeifaddrs(ifaddr);
        return is_up;
    }

    static bool hasInternetRoute() {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return false;

        // Set socket to non-blocking
        fcntl(sock, F_SETFL, O_NONBLOCK);

        struct sockaddr_in serv_addr;
        std::memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &serv_addr.sin_addr);

        // Connect UDP socket - instantly checks routing table
        int res = connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        close(sock);

        return (res == 0);
    }

    static bool isOnline() {
        return isInterfaceUp() && hasInternetRoute();
    }
};
```

---

## 3. Domain Obfuscation Mapping

To prevent leaking detailed URLs or private window titles (which violates privacy policies), the daemon must map these inputs to clean base domains or generic application identifiers before applying Laplace noise.

### 3.1 URL Parsing
For inputs that are raw URLs:
1.  **Strip Protocol:** Remove `https://` or `http://`.
2.  **Truncate Query & Path:** Locate the first occurrence of `/`, `:`, `?`, or `#` and strip everything after.
3.  **Heuristic Base Domain Extraction:**
    *   Split the host by `.`.
    *   If segments $\le 2$, return the host.
    *   If the second-to-last segment is a common ccSLD (e.g. `co`, `com`, `org`, `net` in combination with a country code TLD like `uk`, `jp`, `br`), return the last 3 segments (e.g. `google.co.uk`).
    *   Otherwise, return the last 2 segments (e.g. `github.com`).

### 3.2 Tab Title Parsing
For general tab titles:
1.  **Delimiters:** Split the title using common separators like ` - ` or ` | `. Typically, hostnames or app names appear in the final segment.
2.  **Keyword Matching:** Compare the rightmost segments against a list of common apps:
    *   `"Gmail"` -> `mail.google.com`
    *   `"Slack"` -> `slack.com`
    *   `"YouTube"` -> `youtube.com`
3.  **Fallback to Hashing:** If no domain can be cleanly extracted, hash the cleaned string using `HMAC-SHA256(title, secret)` to produce a unique, consistent, but non-reversible token. This ensures unique sites can be tracked/aggregated without exposing details.

---

## 4. Command Line Laplace Calibration Utility

A dedicated utility `laplace-calibrate` is proposed to compute parameters and verify Laplace distribution characteristics.

### 4.1 CLI Requirements
*   `--epsilon`: Privacy parameter $\epsilon > 0$.
*   `--sensitivity`: Target function sensitivity $\Delta f > 0$.
*   `--budget`: Total available privacy budget $B$.
*   `--secret`: Salt used for secure PRNG seeding and hashing.

### 4.2 C++ Design Structure
```cpp
#include <iostream>
#include <string>
#include <cmath>
#include <random>
#include <functional>

struct CalibratorArgs {
    double epsilon = 0.0;
    double sensitivity = 1.0;
    double budget = 0.0;
    std::string secret = "";
};

void printUsage() {
    std::cout << "Usage: laplace-calibrate --epsilon <val> --sensitivity <val> --budget <val> --secret <str>\n";
}

int main(int argc, char* argv[]) {
    CalibratorArgs args;
    // Simple CLI parser (recommend getopt_long in full implementation)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--epsilon" && i + 1 < argc) args.epsilon = std::stod(argv[++i]);
        else if (arg == "--sensitivity" && i + 1 < argc) args.sensitivity = std::stod(argv[++i]);
        else if (arg == "--budget" && i + 1 < argc) args.budget = std::stod(argv[++i]);
        else if (arg == "--secret" && i + 1 < argc) args.secret = argv[++i];
    }

    if (args.epsilon <= 0 || args.sensitivity <= 0 || args.budget <= 0) {
        printUsage();
        return 1;
    }

    // Calculations
    double scale = args.sensitivity / args.epsilon;
    double variance = 2.0 * std::pow(scale, 2.0);
    int max_queries = static_cast<int>(std::floor(args.budget / args.epsilon));

    std::cout << "--- Laplace Calibration Results ---" << std::endl;
    std::cout << "Epsilon (e):       " << args.epsilon << std::endl;
    std::cout << "Sensitivity (df):  " << args.sensitivity << std::endl;
    std::cout << "Scale parameter(b):" << scale << std::endl;
    std::cout << "Variance:          " << variance << std::endl;
    std::cout << "Total Budget:      " << args.budget << std::endl;
    std::cout << "Max Queries:       " << max_queries << " (before budget exhaustion)" << std::endl;

    // Secure Seeding using hashed secret
    std::hash<std::string> hasher;
    unsigned int seed = hasher(args.secret);
    std::mt19937 generator(seed);
    std::uniform_real_distribution<double> dist(-0.5, 0.5);

    std::cout << "\nSample Noise values seeded by secret:" << std::endl;
    for (int i = 0; i < 5; ++i) {
        double u = dist(generator);
        double noise = -scale * (u > 0 ? 1 : -1) * std::log(1.0 - 2.0 * std::abs(u));
        std::cout << "  Sample #" << i+1 << ": " << noise << std::endl;
    }

    return 0;
}
```

---

## 5. Linker Dependencies Evaluation

### 5.1 SQLite3 Linking Analysis
Our investigation of the target system shows that while `libsqlite3.so.0` is present in `/usr/lib/aarch64-linux-gnu/`, the development headers (`sqlite3.h`) and standard configuration scripts are **not installed**. Therefore, a simple `find_package(SQLite3 REQUIRED)` will fail at build time.

We propose two integration alternatives:

#### Option A: Amalgamation (Highly Recommended)
Copy `sqlite3.c` and `sqlite3.h` directly into a project subdirectory (e.g. `third_party/sqlite3/`).
*   **Pros:** 100% self-contained; guarantees build compilation in any environment; no external dependencies to manage.
*   **Cons:** Binary size increases slightly; security patching of SQLite3 must be managed manually by updating vendored sources.

**CMake Configuration (Option A):**
```cmake
add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_compile_options(sqlite3 PRIVATE -w) # Disable compiler warnings
target_include_directories(sqlite3 PUBLIC third_party/sqlite3)

# Link to core_lib
target_link_libraries(core_lib PUBLIC sqlite3)
```

#### Option B: Dynamic Linking via system package
Require the host machine to install `libsqlite3-dev` first.

**CMake Configuration (Option B):**
```cmake
find_package(SQLite3 REQUIRED)
target_include_directories(core_lib PRIVATE ${SQLite3_INCLUDE_DIRS})
target_link_libraries(core_lib PRIVATE ${SQLite3_LIBRARIES})
```

---

## 🔒 Security Advisory: Shell Injection Vulnerability

In `edge-daemon/src/main.cpp`, we observed the following transmission logic:

```cpp
std::string curl_cmd = "curl -s -X POST -H \"Content-Type: application/json\" -d '{\"metric\":\"" + safe_event.metric_name + "\",\"val\":" + std::to_string(safe_event.value) + "}' https://europe-west3-project-chronos-2026.cloudfunctions.net/ingestTelemetry > /dev/null";
exec(curl_cmd.c_str());
```

This constructs a raw shell command string and runs it via `popen()`, which spawns `/bin/sh`.
*   **Vulnerability:** If the window title (`safe_event.metric_name`) contains shell metacharacters (e.g., quotes, semicolons, backticks), it leads to **arbitrary code execution (ACE)** under the daemon's user privileges.
*   **Proof of Concept:** A tab titled `test'; rm -rf / #` will execute the payload in the system command interpreter.
*   **Resolution:**
    1.  **Do not execute shell commands.** Replace `popen(curl)` with native `libcurl` linking in C++.
    2.  If shell calls are absolutely required, use `fork()` and `execve()` directly with argument arrays to avoid `/bin/sh` shell parsing.

**CMake Integration for Libcurl:**
```cmake
find_package(CURL REQUIRED)
target_link_libraries(core_lib PRIVATE CURL::libcurl)
```

---

## 6. Observations & Evidence Chain

*   **Observation 1:** Host SQLite3 runtime library exists at `/usr/lib/aarch64-linux-gnu/libsqlite3.so.0.8.6`, but header files and development symlinks are missing. Verified via filesystem searches.
*   **Observation 2:** Ingestion uses synchronous execution blocks. `edge-daemon/src/main.cpp:85` executes `popen` for curl, blocking request handling and exposing the system to injection.
*   **Observation 3:** Property-based tests for Differential Privacy noise are available at `edge-daemon/tests/differential_privacy_test.cpp`.

---

## 7. Verification Method

To verify these architectural changes:
1.  Verify SQLite3 linking compilation:
    ```bash
    cmake -S . -B build
    cmake --build build
    ```
2.  Run property based verification tests:
    ```bash
    ./build/edge-daemon-tests
    ```
3.  Inject mock shell titles (e.g. `test' ; exit 1;`) to verify that the domain obfuscator sanitizes or hashes input and that `libcurl` safely encapsulates request payloads.
