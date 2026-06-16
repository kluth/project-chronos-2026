# Edge Tracker Enhancements - Architectural Analysis Report

This report provides detailed architectural recommendations and design specifications for implementing local buffering, network state checking, domain obfuscation, and Laplace noise calibration in the `edge-daemon` component of Project Chronos.

---

## 1. SQLite3 Database Integration

To prevent telemetry data loss when the client device is offline, a local SQLite3 storage buffer is required. Telemetry events will be queued locally and sent to the Firebase backend once connection is restored.

### 1.1 Schema Design
We require a lightweight schema to store anonymized telemetry events.
```sql
CREATE TABLE IF NOT EXISTS telemetry_buffer (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    metric_name TEXT NOT NULL,
    value REAL NOT NULL,
    timestamp INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    retry_count INTEGER NOT NULL DEFAULT 0
);
```

*Architectural Decision (Local Privacy)*: Events **must** be processed by the differential privacy engine (`anonymizeEvent`) *before* writing them to the SQLite database. This ensures that even if the local database is compromised, the stored data is already anonymized and satisfies the differential privacy guarantees.

### 1.2 Connection and Resource Management
The database connection should be managed using a dedicated class (`SQLiteBuffer`) following the RAII (Resource Acquisition Is Initialization) pattern.

```cpp
#include <sqlite3.h>
#include <string>
#include <vector>
#include <iostream>

struct TelemetryRecord {
    int id;
    std::string metric_name;
    double value;
    long long timestamp;
    int retry_count;
};

class SQLiteBuffer {
private:
    sqlite3* db = nullptr;
    std::string db_path;

public:
    SQLiteBuffer(const std::string& path) : db_path(path) {}
    ~SQLiteBuffer() { close(); }

    bool open() {
        // SQLITE_OPEN_FULLMUTEX ensures thread safety for multi-threaded access
        int rc = sqlite3_open_v2(db_path.c_str(), &db, 
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, 
                                 nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "[SQLite] Cannot open database: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }

        // Enable Write-Ahead Logging (WAL) for high concurrency and performance
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

        const char* create_query = 
            "CREATE TABLE IF NOT EXISTS telemetry_buffer ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  metric_name TEXT NOT NULL,"
            "  value REAL NOT NULL,"
            "  timestamp INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),"
            "  retry_count INTEGER NOT NULL DEFAULT 0"
            ");";

        char* err_msg = nullptr;
        rc = sqlite3_exec(db, create_query, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::cerr << "[SQLite] Table creation failed: " << err_msg << std::endl;
            sqlite3_free(err_msg);
            return false;
        }
        return true;
    }

    void close() {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }

    bool insertEvent(const std::string& name, double value) {
        const char* insert_sql = "INSERT INTO telemetry_buffer (metric_name, value) VALUES (?, ?);";
        sqlite3_stmt* stmt = nullptr;
        
        if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, value);

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return (rc == SQLITE_DONE);
    }

    std::vector<TelemetryRecord> fetchBatch(int limit) {
        std::vector<TelemetryRecord> batch;
        const char* select_sql = "SELECT id, metric_name, value, timestamp, retry_count "
                                 "FROM telemetry_buffer ORDER BY id ASC LIMIT ?;";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return batch;
        }

        sqlite3_bind_int(stmt, 1, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TelemetryRecord rec;
            rec.id = sqlite3_column_int(stmt, 0);
            rec.metric_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            rec.value = sqlite3_column_double(stmt, 2);
            rec.timestamp = sqlite3_column_int64(stmt, 3);
            rec.retry_count = sqlite3_column_int(stmt, 4);
            batch.push_back(rec);
        }

        sqlite3_finalize(stmt);
        return batch;
    }

    bool deleteRecord(int id) {
        const char* delete_sql = "DELETE FROM telemetry_buffer WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db, delete_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return (rc == SQLITE_DONE);
    }

    bool incrementRetry(int id) {
        const char* update_sql = "UPDATE telemetry_buffer SET retry_count = retry_count + 1 WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return (rc == SQLITE_DONE);
    }
};
```

### 1.3 Outbox Queue Thread Design
To maintain socket responsiveness, data ingestion must be decoupled from backend uploads:
1. **Socket Receiver Loop**: Extracts active windows, anonymizes the data, and writes the anonymized record to SQLite. Returns standard `200 OK` to the Chrome Extension immediately.
2. **Buffer Uploader Thread**: Wakes up every 10 seconds.
   - Performs a lightweight network check.
   - If online:
     - Fetches a batch of 10-20 oldest records from SQLite.
     - Loops through the records, executing `curl` or HTTPS POST.
     - Upon a successful upload, deletes the record.
     - Upon failure, increments retry count (optionally discards record if retries exceed a limit, e.g. 5).
   - If offline: Goes back to sleep.

---

## 2. Crostini Network State Checker

`edge-daemon` runs in the Crostini Linux container of ChromeOS. We must avoid spawning shell `curl` processes when the network is offline because doing so blocks, consumes system resources, and generates zombie tasks.

### 2.1 Interface Checking via `getifaddrs`
A simple check parses the list of network interfaces to determine if there is an interface other than loopback (`lo`) that is active and assigned an IP address.

```cpp
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>

bool isNetworkInterfaceAvailable() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return false;
    }

    bool has_active_interface = false;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;

        int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET || family == AF_INET6) {
            std::string name(ifa->ifa_name);
            if (name == "lo") continue; // Skip loopback

            // Verify if interface flags are UP and RUNNING
            if ((ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING)) {
                has_active_interface = true;
                break;
            }
        }
    }
    
    freeifaddrs(ifaddr);
    return has_active_interface;
}
```

### 2.2 The Crostini Virtual Ethernet Problem
Crostini resides in a lightweight VM. Its interface (`eth0`) connects to a host-level virtual bridge. Thus, **`eth0` remains UP and assigned a valid IP address (`10.x.x.x`) even if the physical ChromeOS host has disconnected from Wi-Fi/Ethernet**. 

*Architectural Recommendation*: We must perform a fast, non-blocking TCP socket connection check to confirm actual internet connectivity.

```cpp
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>

bool verifyInternetConnectivity(const std::string& test_ip = "8.8.8.8", int test_port = 53, int timeout_ms = 500) {
    if (!isNetworkInterfaceAvailable()) {
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    // Set non-blocking mode
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(test_port);
    inet_pton(AF_INET, test_ip.c_str(), &addr.sin_addr);

    int rc = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (rc < 0) {
        if (errno == EINPROGRESS) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);

            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            rc = select(sock + 1, nullptr, &write_fds, nullptr, &tv);
            if (rc > 0) {
                int socket_error = 0;
                socklen_t len = sizeof(socket_error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &len) == 0) {
                    if (socket_error == 0) {
                        close(sock);
                        return true; // Successfully connected
                    }
                }
            }
        }
    } else {
        close(sock);
        return true; // Synchronous connect succeeded
    }

    close(sock);
    return false;
}
```

This two-stage check (first check interface table, then a fast 500ms non-blocking TCP socket connect attempt to a reliable destination) provides reliable network status detection while completely eliminating process-level blocking or shell script timeouts.

---

## 3. Domain Obfuscation Mapping

The active browser tracking returns the raw tab title or URL. Processing these values at the edge is necessary to remove private parameters, user-identifying info, and deep URLs, converting them into a base domain before applying Laplace noise.

### 3.1 URL Parsing Architecture
For strings starting with `http://` or `https://`, we strip the scheme, split on path and port characters, and resolve to the base domain.

### 3.2 Title Suffix Rules mapping
For standard browser window titles, we use a suffix-matching rules engine to resolve the application/website context.

### 3.3 Complete Obfuscation Class Design
```cpp
#include <string>
#include <vector>
#include <algorithm>

struct ObfuscationRule {
    std::string pattern;
    std::string target_domain;
    bool is_suffix;
};

class DomainObfuscator {
private:
    std::vector<ObfuscationRule> rules;

    // Helper to strip protocol and parse base domain
    std::string extractDomainFromURL(const std::string& url) {
        std::string host = url;
        size_t proto_pos = host.find("://");
        if (proto_pos != std::string::npos) {
            host = host.substr(proto_pos + 3);
        }

        // Remove path, query, fragment and ports
        size_t end_pos = host.find_first_of("/:?#");
        if (end_pos != std::string::npos) {
            host = host.substr(0, end_pos);
        }

        return getBaseDomain(host);
    }

    // Heuristic helper to convert mail.google.com -> google.com
    std::string getBaseDomain(const std::string& host) {
        // Split host into parts
        std::vector<std::string> parts;
        size_t start = 0;
        size_t end = host.find('.');
        while (end != std::string::npos) {
            parts.push_back(host.substr(start, end - start));
            start = end + 1;
            end = host.find('.', start);
        }
        parts.push_back(host.substr(start));

        if (parts.size() <= 2) {
            return host;
        }

        // Detect double extension suffixes (e.g. .co.uk, .com.br)
        size_t n = parts.size();
        std::string last_two = parts[n-2] + "." + parts[n-1];
        if (last_two == "co.uk" || last_two == "com.br" || last_two == "org.uk" || last_two == "net.au") {
            if (n >= 3) {
                return parts[n-3] + "." + last_two;
            }
        }

        return parts[n-2] + "." + parts[n-1];
    }

public:
    DomainObfuscator() {
        // Core default rules mapping browser titles to generic platforms
        rules = {
            {" - Google Search", "google.com", true},
            {" - YouTube", "youtube.com", true},
            {" | GitHub", "github.com", true},
            {"GitHub - ", "github.com", false},
            {" - Slack", "slack.com", true},
            {"Gmail - ", "mail.google.com", false},
            {"Outlook - ", "outlook.live.com", false}
        };
    }

    std::string obfuscate(const std::string& input) {
        if (input.empty()) {
            return "idle";
        }

        // 1. Detect and parse if input is a raw URL
        if (input.rfind("http://", 0) == 0 || input.rfind("https://", 0) == 0) {
            return extractDomainFromURL(input);
        }

        // 2. Perform rule matches on browser window titles
        for (const auto& rule : rules) {
            if (rule.is_suffix) {
                if (input.length() >= rule.pattern.length() &&
                    input.compare(input.length() - rule.pattern.length(), rule.pattern.length(), rule.pattern) == 0) {
                    return rule.target_domain;
                }
            } else {
                if (input.rfind(rule.pattern, 0) == 0) {
                    return rule.target_domain;
                }
            }
        }

        // 3. Fallback Policy: To guarantee zero-leakage, unmapped/private titles 
        // must be generalized to an opaque classification instead of raw delivery.
        return "unclassified_activity";
    }
};
```

---

## 4. Command Line Laplace Calibration Utility

To support rapid validation, debugging, and setup of differential privacy regimes, we require a CLI tool that parses privacy parameters, calculates scale characteristics, and outputs sample noise values.

### 4.1 CLI Interface Requirements
Parameters:
- `--epsilon` (or `-e`): Privacy parameter $\epsilon > 0$. Smaller values yield more privacy (more noise).
- `--sensitivity` (or `-s`): Query sensitivity $S > 0$.
- `--budget` (or `-b`): Total privacy budget $B$.
- `--secret` (or `-k`): Seed string for deterministic, reproducible pseudorandom Laplace generation.

### 4.2 Mathematical Scale Definition
Laplace distribution scale parameter $b$:
$$b = \frac{S}{\epsilon}$$

The probability density function is:
$$f(x) = \frac{1}{2b} \exp\left(-\frac{|x|}{b}\right)$$

Variance of Laplace distribution:
$$\sigma^2 = 2 b^2 = 2 \left(\frac{S}{\epsilon}\right)^2$$

Standard deviation:
$$\sigma = \sqrt{2} b = \sqrt{2} \frac{S}{\epsilon}$$

### 4.3 Implementation Strategy with `getopt_long` and CSPRNG/Deterministic seed
By hashing the `--secret` argument into a seed value, the utility generates consistent noise samples, allowing developers to perform deterministic regression tests across different environments.

```cpp
#include <iostream>
#include <string>
#include <cmath>
#include <random>
#include <getopt.h>
#include <iomanip>

double generateLaplaceNoiseWithSeed(double sensitivity, double epsilon, unsigned int seed) {
    double b = sensitivity / epsilon;
    std::mt19937 generator(seed);
    std::uniform_real_distribution<double> distribution(-0.5, 0.5);
    double u = distribution(generator);
    return -b * (u > 0 ? 1 : -1) * std::log(1.0 - 2.0 * std::abs(u));
}

void printHelp() {
    std::cout << "Usage: calibrate-laplace [options]\n"
              << "Options:\n"
              << "  -e, --epsilon <val>      Epsilon parameter (default: 0.5)\n"
              << "  -s, --sensitivity <val>  Sensitivity parameter (default: 1.0)\n"
              << "  -b, --budget <val>       Total privacy budget (default: 5.0)\n"
              << "  -k, --secret <str>       Secret key/seed for reproducible noise generation\n"
              << "  -h, --help               Display this help message\n";
}

int main_utility(int argc, char* argv[]) {
    double epsilon = 0.5;
    double sensitivity = 1.0;
    double budget = 5.0;
    std::string secret = "";

    struct option long_options[] = {
        {"epsilon", required_argument, nullptr, 'e'},
        {"sensitivity", required_argument, nullptr, 's'},
        {"budget", required_argument, nullptr, 'b'},
        {"secret", required_argument, nullptr, 'k'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "e:s:b:k:h", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'e':
                epsilon = std::stod(optarg);
                break;
            case 's':
                sensitivity = std::stod(optarg);
                break;
            case 'b':
                budget = std::stod(optarg);
                break;
            case 'k':
                secret = optarg;
                break;
            case 'h':
            default:
                printHelp();
                return 0;
        }
    }

    if (epsilon <= 0 || sensitivity <= 0) {
        std::cerr << "Error: Epsilon and Sensitivity must be positive non-zero values.\n";
        return 1;
    }

    double b = sensitivity / epsilon;
    double variance = 2.0 * b * b;
    double std_dev = std::sqrt(variance);

    std::cout << std::fixed << std::setprecision(5);
    std::cout << "========================================\n";
    std::cout << "  Laplace Differential Privacy Calibration\n";
    std::cout << "========================================\n";
    std::cout << "Epsilon (e)      : " << epsilon << "\n";
    std::cout << "Sensitivity (s)  : " << sensitivity << "\n";
    std::cout << "Privacy Budget(b): " << budget << "\n";
    std::cout << "----------------------------------------\n";
    std::cout << "Calculated Scale : " << b << "\n";
    std::cout << "Expected Variance: " << variance << "\n";
    std::cout << "Standard Dev (sd): " << std_dev << "\n";
    std::cout << "Max Allocations  : " << static_cast<int>(budget / epsilon) << " queries before budget depletion\n";
    std::cout << "========================================\n";

    if (!secret.empty()) {
        unsigned int seed = std::hash<std::string>{}(secret);
        std::cout << "Seeded Generator (Secret: \"" << secret << "\", Seed: " << seed << ")\n";
        std::cout << "Sample Noise Output:\n";
        for (int i = 0; i < 5; ++i) {
            // Re-seeding with changing index to get a deterministic sequence
            double noise = generateLaplaceNoiseWithSeed(sensitivity, epsilon, seed + i);
            std::cout << "  Sample [" << i << "]: " << noise << "\n";
        }
    } else {
        std::cout << "Unseeded Random Samples:\n";
        std::random_device rd;
        for (int i = 0; i < 5; ++i) {
            double noise = generateLaplaceNoiseWithSeed(sensitivity, epsilon, rd());
            std::cout << "  Sample [" << i << "]: " << noise << "\n";
        }
    }
    std::cout << "========================================\n";

    return 0;
}
```

---

## 5. CMake & Linker Dependency Evaluation

### 5.1 System Library Verification
An examination of the build system dependencies reveals a major roadblock:
- `libsqlite3-0` is installed at the system level.
- **However, `libsqlite3-dev` (containing `sqlite3.h`) is NOT installed.** Compilation of database source code will fail.

### 5.2 Build System Integration Options
We propose two options to integrate SQLite3 linking into `/edge-daemon/CMakeLists.txt`:

#### Option A: Package-Manager Driven (System SQLite3)
Instruct the setup script or developer to install the dev headers (`sudo apt-get install libsqlite3-dev`). Modify `CMakeLists.txt` to find and link the library:

```cmake
# Add SQLite3 Package Detection
find_package(SQLite3 REQUIRED)

include_directories(src ${SQLite3_INCLUDE_DIRS})

add_library(core_lib src/differential_privacy.cpp)
# If core_lib includes sqlite, link it
target_link_libraries(core_lib PRIVATE ${SQLite3_LIBRARIES})

add_executable(edge-daemon src/main.cpp)
target_link_libraries(edge-daemon core_lib ${SQLite3_LIBRARIES})
```

#### Option B: Self-Contained/Amalgamation (Recommended)
To prevent compile failures in sandboxed or offline container environments (like developer Crostini setups without internet/package access), we can vendor the SQLite3 amalgamation files:
1. Download `sqlite3.h` and `sqlite3.c` from sqlite.org and place them in `edge-daemon/src/`.
2. Update `CMakeLists.txt` to build and link it directly:

```cmake
# Build SQLite3 as a local dependency
add_library(sqlite3_static STATIC src/sqlite3.c)

add_library(core_lib src/differential_privacy.cpp)

add_executable(edge-daemon src/main.cpp)
# Link static sqlite3 and core library
target_link_libraries(edge-daemon core_lib sqlite3_static pthread dl)
```
*Benefits of Option B*: Extremely portable, guarantees build success without system dependencies, and locks the SQLite version across all deployments.
