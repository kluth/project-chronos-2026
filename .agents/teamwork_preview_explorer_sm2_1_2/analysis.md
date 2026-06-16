# Chronos Edge-Daemon Architectural Recommendations
**Sub-Milestone 2.1: Local Buffering, Network Check, Obfuscation, and Calibration**

---

## Executive Summary
This report provides architectural designs and recommendations for enhancing the `edge-daemon` component of Project Chronos. By incorporating local SQLite3 offline buffering, dynamic network state checking, local domain obfuscation, and command-line privacy calibration, we transition the edge-daemon into a secure, robust, and offline-resilient telemetry tracker.

---

## 1. SQLite3 Database Integration
Offline resilience requires buffering telemetry events locally inside the Crostini container when internet connectivity is lost, followed by bulk upload when the connection is restored.

### Schema Design
To balance memory usage, disk durability, and data privacy, we propose the following `telemetry_buffer` table:

```sql
CREATE TABLE IF NOT EXISTS telemetry_buffer (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    metric_name TEXT NOT NULL,
    value REAL NOT NULL,
    timestamp INTEGER NOT NULL,  -- Unix Epoch time (seconds)
    is_anonymized INTEGER NOT NULL DEFAULT 0 -- 1 if DP noise already applied, 0 otherwise
);
```

#### Privacy Design Decision: *Raw vs. Anonymized Buffering*
1. **Anonymized Buffering (Recommended for Privacy)**: Generate Laplace noise *immediately* and store the anonymized values in the database.
   - *Pros*: Complies with "differential privacy at rest" principles. If the local system is compromised, only noisy data is present.
   - *Cons*: Cannot recalculate values if the privacy budget or epsilon is modified dynamically after the event occurs.
2. **Raw Buffering**: Store the actual duration/window, and apply noise only during transmission.
   - *Pros*: Essential if the user wants to adjust epsilon values retroactively or calculate a local daily leakage budget based on true events.
   - *Cons*: Raw data is stored locally.

### Connection & Lifecycle Management
We recommend wrapping SQLite3 interactions in a thread-safe connection wrapper class `TelemetryDB`:

```cpp
#include <sqlite3.h>
#include <string>
#include <vector>
#include <mutex>

struct TelemetryEventRecord {
    int id;
    std::string metric_name;
    double value;
    int64_t timestamp;
    bool is_anonymized;
};

class TelemetryDB {
private:
    sqlite3* db_ = nullptr;
    std::mutex db_mutex_;
    std::string db_path_;

public:
    TelemetryDB(const std::string& path) : db_path_(path) {}
    ~TelemetryDB() { close(); }

    bool open() {
        std::lock_guard<std::mutex> lock(db_mutex_);
        int rc = sqlite3_open_v2(db_path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            return false;
        }
        // Initialize Schema
        char* err_msg = nullptr;
        const char* sql = "CREATE TABLE IF NOT EXISTS telemetry_buffer ("
                          "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                          "metric_name TEXT NOT NULL,"
                          "value REAL NOT NULL,"
                          "timestamp INTEGER NOT NULL,"
                          "is_anonymized INTEGER NOT NULL DEFAULT 0);";
        rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            sqlite3_free(err_msg);
            return false;
        }
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(db_mutex_);
        if (db_) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
    }

    bool insertEvent(const std::string& name, double val, int64_t ts, bool anon) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        sqlite3_stmt* stmt;
        const char* sql = "INSERT INTO telemetry_buffer (metric_name, value, timestamp, is_anonymized) VALUES (?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        
        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, val);
        sqlite3_bind_int64(stmt, 3, ts);
        sqlite3_bind_int(stmt, 4, anon ? 1 : 0);

        bool success = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return success;
    }

    std::vector<TelemetryEventRecord> fetchBufferedEvents(int limit = 100) {
        std::lock_guard<std::mutex> lock(db_mutex_);
        std::vector<TelemetryEventRecord> records;
        sqlite3_stmt* stmt;
        const char* sql = "SELECT id, metric_name, value, timestamp, is_anonymized FROM telemetry_buffer LIMIT ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return records;

        sqlite3_bind_int(stmt, 1, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            records.push_back({
                sqlite3_column_int(stmt, 0),
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                sqlite3_column_double(stmt, 2),
                sqlite3_column_int64(stmt, 3),
                sqlite3_column_int(stmt, 4) != 0
            });
        }
        sqlite3_finalize(stmt);
        return records;
    }

    bool deleteEvents(const std::vector<int>& ids) {
        if (ids.empty()) return true;
        std::lock_guard<std::mutex> lock(db_mutex_);
        std::string query = "DELETE FROM telemetry_buffer WHERE id IN (";
        for (size_t i = 0; i < ids.size(); ++i) {
            query += (i == 0 ? "?" : ", ?");
        }
        query += ");";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;

        for (size_t i = 0; i < ids.size(); ++i) {
            sqlite3_bind_int(stmt, i + 1, ids[i]);
        }

        bool success = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        return success;
    }
};
```

---

## 2. Crostini Network State Checker
In Crostini, the daemon currently shells out to `curl` to transmit data. Spawning a new shell process when offline causes `curl` to hang on DNS resolution, wasting system resources. We propose a two-tiered check.

### Design Architecture
1. **Level 1 (Interface Level)**: Use `getifaddrs` to verify that a non-loopback network interface is UP, RUNNING, and has an IPv4/IPv6 address.
2. **Level 2 (Connection Level)**: Attempt to open a non-blocking TCP socket to a reliable public host (e.g., Google DNS `8.8.8.8` on port `53`) with a 1-second timeout.

```cpp
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>

class NetworkChecker {
public:
    // L1: Fast check of local interfaces
    static bool isInterfaceUp() {
        struct ifaddrs* ifaddr = nullptr;
        if (getifaddrs(&ifaddr) == -1) {
            return false;
        }
        bool has_connection = false;
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr) continue;
            
            // Skip loopback interfaces
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            
            // Verify if interface is active and has IP
            if ((ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING)) {
                int family = ifa->ifa_addr->sa_family;
                if (family == AF_INET || family == AF_INET6) {
                    has_connection = true;
                    break;
                }
            }
        }
        freeifaddrs(ifaddr);
        return has_connection;
    }

    // L2: Active routing verification (non-blocking TCP connect check)
    static bool canReachInternet() {
        if (!isInterfaceUp()) return false;

        int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (sock < 0) return false;

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(53); // DNS Port
        inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr); // Google DNS

        int res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        if (res < 0) {
            if (errno == EINPROGRESS) {
                struct pollfd pfd;
                pfd.fd = sock;
                pfd.events = POLLOUT;
                // Wait for up to 1000 milliseconds
                int poll_res = poll(&pfd, 1, 1000);
                if (poll_res > 0) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                        close(sock);
                        return true;
                    }
                }
            }
        } else {
            // Instant connect
            close(sock);
            return true;
        }

        close(sock);
        return false;
    }
};
```

### Integration Workflow
In `main.cpp`:
- If `NetworkChecker::canReachInternet()` returns `true`, upload immediately.
- If it returns `false`, bypass `curl` execution entirely and insert the event into `TelemetryDB`.

---

## 3. Domain Obfuscation Mapping
To protect privacy at the edge, full URLs and descriptive tab titles should be rewritten to base domains or high-level application categories before adding differential privacy noise.

### Implementation Logic
We recommend implementing a function `obfuscateMetricName` which:
1. Detects and extracts URLs from tab titles.
2. Identifies specific browser application titles (e.g., matching "Chrome", "Firefox") and extracts the base domain if present.
3. Matches keywords against a known dictionary of work/private platforms.
4. Falls back to safe generic labels (e.g., `web-browser`, `terminal`, `vscode`, `generic-activity`) to hide document titles, search queries, or internal directories.

```cpp
#include <string>
#include <vector>
#include <algorithm>

// Extracts the base domain (e.g. "https://docs.google.com/document/d/..." -> "google.com")
std::string extractBaseDomain(const std::string& raw_url) {
    std::string host = raw_url;
    
    // 1. Strip protocol scheme
    size_t scheme_pos = host.find("://");
    if (scheme_pos != std::string::npos) {
        host = host.substr(scheme_pos + 3);
    }
    
    // 2. Strip path, query parameters, or ports
    size_t path_pos = host.find_first_of("/:?#");
    if (path_pos != std::string::npos) {
        host = host.substr(0, path_pos);
    }
    
    // 3. Strip "www." subdomain if present
    if (host.rfind("www.", 0) == 0) {
        host = host.substr(4);
    }
    
    // 4. Split parts to locate the root domain (handles common subdomains)
    std::vector<std::string> parts;
    size_t start = 0;
    size_t end = host.find('.');
    while (end != std::string::npos) {
        parts.push_back(host.substr(start, end - start));
        start = end + 1;
        end = host.find('.', start);
    }
    parts.push_back(host.substr(start));
    
    if (parts.size() >= 2) {
        size_t n = parts.size();
        std::string tld = parts[n - 1];
        std::string sld = parts[n - 2];
        
        // Handle double extension domains like co.uk, com.au, org.uk, net.cn
        if (n >= 3 && (sld == "co" || sld == "com" || sld == "org" || sld == "net" || sld == "gov" || sld == "edu" || sld == "ac")) {
            return parts[n - 3] + "." + sld + "." + tld;
        }
        return sld + "." + tld;
    }
    return host;
}

std::string obfuscateMetricName(const std::string& window_title) {
    std::string title = window_title;
    std::transform(title.begin(), title.end(), title.begin(), ::tolower);

    // 1. URL extraction
    size_t url_pos = title.find("http://");
    if (url_pos == std::string::npos) {
        url_pos = title.find("https://");
    }
    if (url_pos != std::string::npos) {
        std::string url_sub = window_title.substr(url_pos);
        size_t space = url_sub.find(' ');
        if (space != std::string::npos) {
            url_sub = url_sub.substr(0, space);
        }
        return extractBaseDomain(url_sub);
    }

    // 2. Keyword-based matching for specific apps/services
    struct MappingRule {
        std::string keyword;
        std::string category;
    };
    static const std::vector<MappingRule> rules = {
        {"github", "github.com"},
        {"gitlab", "gitlab.com"},
        {"stackoverflow", "stackoverflow.com"},
        {"stack overflow", "stackoverflow.com"},
        {"google search", "google.com"},
        {"gmail", "mail.google.com"},
        {"slack", "slack.com"},
        {"jira", "atlassian.net"},
        {"confluence", "atlassian.net"},
        {"figma", "figma.com"},
        {"youtube", "youtube.com"}
    };

    for (const auto& rule : rules) {
        if (title.find(rule.keyword) != std::string::npos) {
            return rule.category;
        }
    }

    // 3. Application Category Mapping
    if (title.find("chrome") != std::string::npos || title.find("browser") != std::string::npos) {
        return "web-browser";
    }
    if (title.find("visual studio code") != std::string::npos || title.find("code - ") != std::string::npos) {
        return "vscode";
    }
    if (title.find("terminal") != std::string::npos || title.find("bash") != std::string::npos) {
        return "terminal";
    }

    // 4. Default safe fallback
    return "generic-activity";
}
```

---

## 4. Command Line Laplace Calibration Utility
On daemon startup, or as a standalone calibration run, the user needs parameters to calibrate Laplace noise generation bounds.

### Configuration Parameters
- `--epsilon` / `-e`: Privacy loss parameters per transmission (double, e.g., `0.5`).
- `--sensitivity` / `-s`: Sensitivity of the query (double, default `1.0` corresponding to 10s usage pings).
- `--budget` / `-b`: Maximum cumulative 24h budget (double, e.g., `10.0`).
- `--secret` / `-k`: SHA-256 HMAC secret key for signed Chrome-extension-daemon communication loop.

### Command Line Parsing Implementation
A standard Unix-style parser inside `main.cpp` using `std::strcmp`:

```cpp
#include <iostream>
#include <string>
#include <cmath>
#include <cstring>
#include <cstdlib>

struct CLIConfig {
    double epsilon = 0.5;
    double sensitivity = 1.0;
    double budget = 10.0;
    std::string secret = "";
    bool calibrate_only = false;
};

void showHelp() {
    std::cout << "Chronos Edge-Daemon Calibration Utility\n"
              << "Arguments:\n"
              << "  -e, --epsilon <val>     Privacy parameter epsilon (default: 0.5)\n"
              << "  -s, --sensitivity <val> Ingestion step sensitivity (default: 1.0)\n"
              << "  -b, --budget <val>      Total daily leakage budget (default: 10.0)\n"
              << "  --secret <key>          HMAC secret key for secure IPC loop\n"
              << "  --calibrate             Print statistical noise bounds and exit\n";
}

CLIConfig parseArgs(int argc, char* argv[]) {
    CLIConfig config;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-e") == 0 || std::strcmp(argv[i], "--epsilon") == 0) {
            if (i + 1 < argc) config.epsilon = std::stod(argv[++i]);
        } else if (std::strcmp(argv[i], "-s") == 0 || std::strcmp(argv[i], "--sensitivity") == 0) {
            if (i + 1 < argc) config.sensitivity = std::stod(argv[++i]);
        } else if (std::strcmp(argv[i], "-b") == 0 || std::strcmp(argv[i], "--budget") == 0) {
            if (i + 1 < argc) config.budget = std::stod(argv[++i]);
        } else if (std::strcmp(argv[i], "--secret") == 0) {
            if (i + 1 < argc) config.secret = argv[++i];
        } else if (std::strcmp(argv[i], "--calibrate") == 0) {
            config.calibrate_only = true;
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            showHelp();
            std::exit(0);
        }
    }
    return config;
}
```

### Calibration Utility Logic
When `--calibrate` is provided, the utility simulates and calculates the Laplace distribution limits:

```cpp
void runCalibrationSimulation(const CLIConfig& config) {
    double scale = config.sensitivity / config.epsilon;
    double variance = 2.0 * scale * scale;
    double std_dev = std::sqrt(variance);

    // Analytical confidence bounds
    double ci_95 = scale * std::log(1.0 / 0.05); // Laplace 95% bound
    double ci_99 = scale * std::log(1.0 / 0.01); // Laplace 99% bound

    int max_pings = static_cast<int>(config.budget / config.epsilon);

    std::cout << "=== Differential Privacy Noise Calibration ===\n";
    std::cout << "Epsilon (e):       " << config.epsilon << "\n";
    std::cout << "Sensitivity (s):   " << config.sensitivity << "\n";
    std::cout << "Scale (b = s/e):   " << scale << "\n";
    std::cout << "Theoretical Variance: " << variance << "\n";
    std::cout << "Standard Deviation:   " << std_dev << "\n";
    std::cout << "95% Noise Range:   +/- " << ci_95 << "\n";
    std::cout << "99% Noise Range:   +/- " << ci_99 << "\n";
    std::cout << "Privacy Budget:    " << config.budget << "\n";
    std::cout << "Max Transmissions: " << max_pings << " before budget exhaustion\n";
    std::cout << "===============================================\n";
}
```

---

## 5. Linker Dependencies (SQLite3 Linker Evaluation)

### Environment Analysis
Running package audits in the user environment indicates that:
1. `libsqlite3-0` (the runtime library) is pre-installed on the Crostini Debian base.
2. `libsqlite3-dev` (the development headers) **is NOT pre-installed**.
As a result, building `edge-daemon` referencing `#include <sqlite3.h>` will fail compilation without adjustments.

### CMakeLists.txt Modification Strategies

We recommend two distinct approaches to link SQLite3 in the CMake build system:

#### Option A: Amalgamation Integration (Recommended for Crostini portability)
Bundling SQLite3 source files in the codebase is standard for SQLite. It eliminates external header dependencies.
1. Download `sqlite3.c` and `sqlite3.h` and place them in `edge-daemon/3rdparty/sqlite3/`.
2. Modify `CMakeLists.txt` to compile SQLite3 as a static library:

```cmake
cmake_minimum_required(VERSION 3.10)
project(EdgeDaemon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(src 3rdparty/sqlite3)

# Build sqlite3 dependency
add_library(sqlite3 STATIC 3rdparty/sqlite3/sqlite3.c)

# Build our core_lib
add_library(core_lib src/differential_privacy.cpp)
target_link_libraries(core_lib sqlite3)

add_executable(edge-daemon src/main.cpp)
target_link_libraries(edge-daemon core_lib pthread dl) # pthread & dl are required by SQLite3 on UNIX

enable_testing()
add_executable(edge-daemon-tests tests/differential_privacy_test.cpp)
target_link_libraries(edge-daemon-tests core_lib pthread dl)
add_test(NAME PropertyBasedDPTests COMMAND edge-daemon-tests)
```

#### Option B: Dynamic Linking via system package
If `libsqlite3-dev` is installed using `sudo apt install libsqlite3-dev`, use CMake's `FindSQLite3` module:

```cmake
cmake_minimum_required(VERSION 3.10)
project(EdgeDaemon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(src)

# Find system sqlite3 library
find_package(SQLite3 REQUIRED)

add_library(core_lib src/differential_privacy.cpp)
target_include_directories(core_lib PRIVATE ${SQLITE3_INCLUDE_DIRS})
target_link_libraries(core_lib PRIVATE ${SQLITE3_LIBRARIES})

add_executable(edge-daemon src/main.cpp)
target_link_libraries(edge-daemon core_lib ${SQLITE3_LIBRARIES})

enable_testing()
add_executable(edge-daemon-tests tests/differential_privacy_test.cpp)
target_link_libraries(edge-daemon-tests core_lib ${SQLITE3_LIBRARIES})
add_test(NAME PropertyBasedDPTests COMMAND edge-daemon-tests)
```
