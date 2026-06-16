# Chronos Edge Daemon Enhancements Analysis (F37, F39, F44, F47)

This analysis report details the implementation plan for Sub-Milestone 2.2 features in the Project Chronos C++ Edge Daemon.

---

## 1. Architectural Design Overview

To implement local privacy budget tracking, native process scanning, device resource telemetry, and automated shared folder backups, we propose introducing a **multi-threaded architecture** to the `edge-daemon`.

### Background Telemetry Thread
Currently, the `edge-daemon` (defined in `edge-daemon/src/main.cpp`) runs a single-threaded blocking loop listening on TCP port 8888. It only processes telemetry when the ChromeOS extension posts window state changes.
We propose launching a separate background worker thread in `main.cpp` that wakes up periodically (e.g., every 10 seconds) to:
1. Scan running native processes (F37).
2. Collect CPU and RAM performance utilization metrics (F44).
3. Compute the active privacy budget and adjust noise configuration or pause tracking (F39).
4. Buffer the anonymized telemetry in SQLite.
5. Trigger automated backups (F47) every 5 minutes.

### Thread-Safe SQLite Operations
Since both the main TCP socket thread and the background telemetry thread will write to the SQLite database, we must ensure serialized access to prevent database locks and data corruption.
We propose declaring a global `std::recursive_mutex g_db_mutex` in `edge-daemon/src/differential_privacy.cpp` and locking it across all database operations. A recursive mutex is selected to allow a single thread to acquire the lock multiple times, preventing deadlocks when high-level functions (e.g. `flushBuffer`) call low-level database operations (e.g. `getBufferedEvents`, `deleteBufferedEvent`) that also lock the mutex.

---

## 2. Evidence Chain & Reference Mapping

| Feature | Target Location | Existing Logic / File Context |
|---|---|---|
| **F39 (Privacy Budget)** | `edge-daemon/src/differential_privacy.cpp:288` | `initDatabase` sets up database. We need to append the `privacy_budget_log` table. |
| **F39 (Noise Adjustment)** | `edge-daemon/src/differential_privacy.cpp:48` | `anonymizeEvent` applies differential privacy using a fixed epsilon. We must adjust this dynamically. |
| **F37 (Process Scanner)** | `edge-daemon/src/differential_privacy.cpp:150` | `obfuscateDomainOrCategory` standardizes window names. We can reuse these categories for processes. |
| **F44 (Resource Telemetry)** | `edge-daemon/src/main.cpp:133` | Events are packaged into `TelemetryEvent` structs. We can package CPU/RAM percentages as events. |
| **F47 (Backup Snapshot)** | `edge-daemon/src/differential_privacy.cpp` | SQLite getters (`getBufferedEvents`). We will extract events to serialize into JSON. |

---

## 3. Local Privacy Budget Tracker (F39)

### SQLite Database Update
We define a new SQLite table `privacy_budget_log` that acts as an append-only log of privacy consumption (epsilon spent) for every event ingested.
```sql
CREATE TABLE IF NOT EXISTS privacy_budget_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,
    epsilon REAL NOT NULL
);
```

### Proposed C++ Implementation

We propose adding the following functions to `edge-daemon/src/differential_privacy.cpp`:

```cpp
#include <mutex>
#include <ctime>

// Declared globally in differential_privacy.cpp
std::recursive_mutex g_db_mutex;

// Log epsilon spent for an event
bool logEpsilonSpent(double epsilon, const std::string& db_path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    const char* sql = "INSERT INTO privacy_budget_log (timestamp, epsilon) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_int64(stmt, 1, std::time(nullptr));
    sqlite3_bind_double(stmt, 2, epsilon);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return (rc == SQLITE_DONE);
}

// Compute cumulative epsilon over the last 24 hours (86,400 seconds)
double get24HourEpsilonBudget(const std::string& db_path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0.0;
    }

    const char* sql = "SELECT SUM(epsilon) FROM privacy_budget_log WHERE timestamp >= ?;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return 0.0;
    }

    sqlite3_bind_int64(stmt, 1, std::time(nullptr) - 86400);

    double cumulative_epsilon = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        cumulative_epsilon = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);

    // Prune entries older than 24 hours to keep the database size stable
    const char* prune_sql = "DELETE FROM privacy_budget_log WHERE timestamp < ?;";
    sqlite3_stmt* prune_stmt = nullptr;
    if (sqlite3_prepare_v2(db, prune_sql, -1, &prune_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(prune_stmt, 1, std::time(nullptr) - 86400);
        sqlite3_step(prune_stmt);
        sqlite3_finalize(prune_stmt);
    }

    sqlite3_close(db);
    return cumulative_epsilon;
}

// Adjust Laplace noise based on current cumulative budget
double getAdjustedEpsilon(double config_epsilon, double budget_limit, double cumulative_24h) {
    if (budget_limit <= 0.0) {
        return config_epsilon; // Budget tracking disabled/unlimited
    }

    if (cumulative_24h >= budget_limit) {
        return 0.0; // Budget completely exhausted: pause ingestion
    }

    // Approaching limit threshold: 80% of budget
    double threshold = 0.8 * budget_limit;
    if (cumulative_24h >= threshold) {
        // Linearly scale down epsilon as we get closer to budget limit
        double ratio = (cumulative_24h - threshold) / (budget_limit - threshold); // 0.0 to 1.0
        double scale_factor = 1.0 - ratio; // 1.0 down to 0.0
        
        // Enforce a floor of 10% of configured epsilon to prevent infinite noise
        if (scale_factor < 0.1) {
            scale_factor = 0.1;
        }
        return config_epsilon * scale_factor;
    }

    return config_epsilon;
}
```

---

## 4. Native Process Scanner `/proc` Monitor (F37)

The POSIX-compliant scanner iterates over numeric directories in `/proc/` to extract active processes.

### Proposed C++ Implementation
```cpp
#include <dirent.h>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>

std::vector<std::string> scanActiveProcesses() {
    std::vector<std::string> active_tools;
    DIR* dir = opendir("/proc");
    if (!dir) {
        return active_tools;
    }

    struct dirent* entry;
    bool has_vscode = false;
    bool has_terminal = false;
    bool has_compiler = false;

    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        // Process IDs are numeric directories
        if (std::all_of(name.begin(), name.end(), ::isdigit)) {
            std::string comm_path = "/proc/" + name + "/comm";
            std::ifstream file(comm_path);
            if (file.is_open()) {
                std::string comm;
                if (std::getline(file, comm)) {
                    // Strip potential trailing newline
                    if (!comm.empty() && comm.back() == '\n') {
                        comm.pop_back();
                    }
                    
                    // Standardize to lowercase
                    std::string lower_comm = "";
                    for (char c : comm) {
                        lower_comm += std::tolower(static_cast<unsigned char>(c));
                    }

                    if (lower_comm.find("code") != std::string::npos ||
                        lower_comm.find("vscode") != std::string::npos) {
                        has_vscode = true;
                    } else if (lower_comm == "bash" || lower_comm == "zsh" ||
                               lower_comm == "tmux" || lower_comm == "fish" ||
                               lower_comm == "sh" || lower_comm == "xterm") {
                        has_terminal = true;
                    } else if (lower_comm == "gcc" || lower_comm == "g++" ||
                               lower_comm == "clang" || lower_comm == "make" ||
                               lower_comm == "ninja" || lower_comm == "cmake" ||
                               lower_comm == "python" || lower_comm == "python3" ||
                               lower_comm == "node") {
                        has_compiler = true;
                    }
                }
            }
        }
    }
    closedir(dir);

    if (has_vscode) active_tools.push_back("vscode");
    if (has_terminal) active_tools.push_back("terminal");
    if (has_compiler) active_tools.push_back("compiler");

    return active_tools;
}
```

---

## 5. Device Resource Performance Telemetry (F44)

Calculates CPU and RAM utilization directly from `/proc/stat` and `/proc/meminfo`.

### Proposed C++ Implementation
```cpp
struct CpuTicks {
    unsigned long long idle = 0;
    unsigned long long active = 0;
    unsigned long long total = 0;
};

CpuTicks readCpuTicks() {
    CpuTicks ticks;
    std::ifstream file("/proc/stat");
    if (!file.is_open()) {
        return ticks;
    }

    std::string line;
    if (std::getline(file, line)) {
        if (line.rfind("cpu ", 0) == 0) {
            std::stringstream ss(line);
            std::string label;
            ss >> label; // Skip "cpu"
            
            unsigned long long user = 0, nice = 0, system = 0, idle = 0;
            unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
            if (ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
                ticks.idle = idle + iowait;
                ticks.active = user + nice + system + irq + softirq + steal;
                ticks.total = ticks.idle + ticks.active;
            }
        }
    }
    return ticks;
}

double calculateCpuUtilization() {
    static CpuTicks prev_ticks;
    CpuTicks curr_ticks = readCpuTicks();

    if (curr_ticks.total == 0) {
        return 0.0;
    }

    if (prev_ticks.total == 0) {
        prev_ticks = curr_ticks;
        return 100.0 * curr_ticks.active / curr_ticks.total;
    }

    unsigned long long delta_total = curr_ticks.total - prev_ticks.total;
    unsigned long long delta_idle = curr_ticks.idle - prev_ticks.idle;
    
    double util = 0.0;
    if (delta_total > 0) {
        util = 100.0 * (delta_total - delta_idle) / delta_total;
    }

    prev_ticks = curr_ticks;
    return util;
}

double calculateRamUtilization() {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) {
        return 0.0;
    }

    std::string line;
    unsigned long long mem_total = 0;
    unsigned long long mem_available = 0;
    unsigned long long mem_free = 0;
    bool found_total = false;
    bool found_available = false;

    while (std::getline(file, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::stringstream ss(line.substr(9));
            ss >> mem_total;
            found_total = true;
        } else if (line.rfind("MemAvailable:", 0) == 0) {
            std::stringstream ss(line.substr(13));
            ss >> mem_available;
            found_available = true;
        } else if (line.rfind("MemFree:", 0) == 0) {
            std::stringstream ss(line.substr(8));
            ss >> mem_free;
        }
    }

    if (mem_total == 0) {
        return 0.0;
    }

    // Fallback if MemAvailable is missing (older kernels)
    unsigned long long avail = found_available ? mem_available : mem_free;
    unsigned long long mem_used = mem_total - avail;
    return 100.0 * mem_used / mem_total;
}
```

---

## 6. Automated Shared Folder Snapshots (F47)

Saves backup JSONs to `~/Downloads/chronos_backups/` periodically. Uses C++17 `<filesystem>` for folder creation and writes raw JSON text without external library dependencies.

### Proposed C++ Implementation
```cpp
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>

std::string escapeJsonString(const std::string& input) {
    std::string out = "";
    for (char c : input) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

bool backupDatabaseToJson(const std::string& db_path) {
    char* home_env = std::getenv("HOME");
    if (!home_env) {
        std::cerr << "[Backup] Error: HOME environment variable is not set." << std::endl;
        return false;
    }
    
    std::string home(home_env);
    std::filesystem::path backup_dir = std::filesystem::path(home) / "Downloads" / "chronos_backups";
    
    try {
        if (!std::filesystem::exists(backup_dir)) {
            std::filesystem::create_directories(backup_dir);
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "[Backup] Error: Failed to create backup directory: " << e.what() << std::endl;
        return false;
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    // 1. Fetch buffered telemetry events
    std::stringstream telemetry_ss;
    telemetry_ss << "[";
    const char* tel_sql = "SELECT id, metric_name, value, timestamp FROM telemetry_events ORDER BY id ASC;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, tel_sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) {
                telemetry_ss << ",";
            }
            first = false;
            int id = sqlite3_column_int(stmt, 0);
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            std::string metric = name ? reinterpret_cast<const char*>(name) : "";
            double value = sqlite3_column_double(stmt, 2);
            long long ts = sqlite3_column_int64(stmt, 3);

            telemetry_ss << "{\"id\":" << id 
                         << ",\"metric_name\":\"" << escapeJsonString(metric) 
                         << "\",\"value\":" << value 
                         << ",\"timestamp\":" << ts << "}";
        }
        sqlite3_finalize(stmt);
    }
    telemetry_ss << "]";

    // 2. Fetch privacy budget log events
    std::stringstream budget_ss;
    budget_ss << "[";
    const char* bud_sql = "SELECT id, timestamp, epsilon FROM privacy_budget_log ORDER BY id ASC;";
    rc = sqlite3_prepare_v2(db, bud_sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) {
                budget_ss << ",";
            }
            first = false;
            int id = sqlite3_column_int(stmt, 0);
            long long ts = sqlite3_column_int64(stmt, 1);
            double eps = sqlite3_column_double(stmt, 2);

            budget_ss << "{\"id\":" << id 
                      << ",\"timestamp\":" << ts 
                      << ",\"epsilon\":" << eps << "}";
        }
        sqlite3_finalize(stmt);
    }
    budget_ss << "]";

    sqlite3_close(db);

    long long current_time = std::time(nullptr);
    std::stringstream json_doc;
    json_doc << "{\n"
             << "  \"backup_timestamp\": " << current_time << ",\n"
             << "  \"buffered_telemetry\": " << telemetry_ss.str() << ",\n"
             << "  \"privacy_budget_log\": " << budget_ss.str() << "\n"
             << "}\n";

    std::filesystem::path backup_file = backup_dir / ("chronos_backup_" + std::to_string(current_time) + ".json");
    std::ofstream file(backup_file);
    if (!file.is_open()) {
        std::cerr << "[Backup] Error: Failed to open backup file " << backup_file << std::endl;
        return false;
    }
    
    file << json_doc.str();
    file.close();
    std::cout << "[Backup] Successfully backed up telemetry to: " << backup_file << std::endl;
    return true;
}

void runBackupIfNeeded(const std::string& db_path) {
    static long long last_backup_time = 0;
    long long now = std::time(nullptr);
    // Trigger backup every 5 minutes (300 seconds)
    if (now - last_backup_time >= 300) {
        backupDatabaseToJson(db_path);
        last_backup_time = now;
    }
}
```

---

## 7. Integration Plan: Modified Entry Points

### Diffs for `edge-daemon/src/differential_privacy.h`
```cpp
// ... Inserted into struct DPConfig:
struct DPConfig {
    double epsilon = 0.5;
    double sensitivity = 1.0;
    double budget = 0.0;
    std::string secret = "";
};

extern DPConfig g_config;
extern std::recursive_mutex g_db_mutex;

// F39: Local Privacy Budget Tracker
bool logEpsilonSpent(double epsilon, const std::string& db_path);
double get24HourEpsilonBudget(const std::string& db_path);
double getAdjustedEpsilon(double config_epsilon, double budget_limit, double cumulative_24h);

// F37: Native Process Scanner /proc Monitor
std::vector<std::string> scanActiveProcesses();

// F44: Device Resource Performance Telemetry
struct CpuTicks {
    unsigned long long idle = 0;
    unsigned long long active = 0;
    unsigned long long total = 0;
};
CpuTicks readCpuTicks();
double calculateCpuUtilization();
double calculateRamUtilization();

// F47: Automated Shared Folder Snapshots
bool backupDatabaseToJson(const std::string& db_path);
void runBackupIfNeeded(const std::string& db_path);
```

### Proposed Background Thread in `edge-daemon/src/main.cpp`
Add the thread worker routine and start it before the TCP socket accept loop.

```cpp
void nativeTelemetryLoop(const std::string& db_path) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        double cumulative_24h = 0.0;
        {
            std::lock_guard<std::recursive_mutex> lock(g_db_mutex);
            cumulative_24h = get24HourEpsilonBudget(db_path);
        }
        
        double adjusted_epsilon = getAdjustedEpsilon(g_config.epsilon, g_config.budget, cumulative_24h);
        if (adjusted_epsilon <= 0.0 && g_config.budget > 0.0) {
            std::cout << "[Native Telemetry] Budget exceeded (" << cumulative_24h 
                      << "/" << g_config.budget << "). Skipping native tracking." << std::endl;
            continue;
        }

        // F37: Scan active processes
        auto tools = scanActiveProcesses();
        for (const auto& tool : tools) {
            TelemetryEvent ev = { tool, 10.0 };
            TelemetryEvent safe_ev = anonymizeEvent(ev, adjusted_epsilon);
            
            std::lock_guard<std::recursive_mutex> lock(g_db_mutex);
            if (bufferEvent(safe_ev, db_path)) {
                logEpsilonSpent(adjusted_epsilon, db_path);
            }
        }

        // F44: Device Performance Telemetry
        double cpu = calculateCpuUtilization();
        double ram = calculateRamUtilization();

        TelemetryEvent cpu_ev = { "cpu_utilization", cpu };
        TelemetryEvent ram_ev = { "ram_utilization", ram };

        TelemetryEvent safe_cpu_ev = anonymizeEvent(cpu_ev, adjusted_epsilon);
        TelemetryEvent safe_ram_ev = anonymizeEvent(ram_ev, adjusted_epsilon);

        {
            std::lock_guard<std::recursive_mutex> lock(g_db_mutex);
            if (bufferEvent(safe_cpu_ev, db_path)) {
                logEpsilonSpent(adjusted_epsilon, db_path);
            }
            if (bufferEvent(safe_ram_ev, db_path)) {
                logEpsilonSpent(adjusted_epsilon, db_path);
            }
        }

        // F47: Backups
        {
            std::lock_guard<std::recursive_mutex> lock(g_db_mutex);
            runBackupIfNeeded(db_path);
        }
    }
}
```

Start the thread inside `main()`:
```cpp
    const std::string DB_PATH = "telemetry_buffer.db";
    if (!initDatabase(DB_PATH)) {
        std::cerr << "Failed to initialize SQLite buffer database." << std::endl;
        return 1;
    }

    // Start background telemetry scanning thread
    std::thread native_thread(nativeTelemetryLoop, DB_PATH);
    native_thread.detach();
```

Inside the socket accept loop handling in `main.cpp`, we wrap all database transactions and ingestion checks under `g_db_mutex`:
```cpp
        if (!active_window.empty()) {
            double cumulative_24h = 0.0;
            {
                std::lock_guard<std::recursive_mutex> lock(g_db_mutex);
                cumulative_24h = get24HourEpsilonBudget(DB_PATH);
            }
            
            double adjusted_epsilon = getAdjustedEpsilon(g_config.epsilon, g_config.budget, cumulative_24h);
            if (adjusted_epsilon <= 0.0 && g_config.budget > 0.0) {
                std::cout << "[Edge Daemon] Budget exceeded (" << cumulative_24h 
                          << "/" << g_config.budget << "). Discarding event: " << active_window << std::endl;
                continue;
            }

            TelemetryEvent event = { active_window, 10.0 };
            std::string cleaned_metric = obfuscateDomainOrCategory(event.metric_name);
            TelemetryEvent cleaned_event = { cleaned_metric, event.value };

            TelemetryEvent safe_event = anonymizeEvent(cleaned_event, adjusted_epsilon);
            
            if (isNetworkOnline()) {
                std::lock_guard<std::recursive_mutex> lock(g_db_mutex);
                flushBuffer(DB_PATH);
                if (!runCurlSafe(safe_event.metric_name, safe_event.value)) {
                    bufferEvent(safe_event, DB_PATH);
                    logEpsilonSpent(adjusted_epsilon, DB_PATH);
                } else {
                    logEpsilonSpent(adjusted_epsilon, DB_PATH);
                }
            } else {
                std::lock_guard<std::recursive_mutex> lock(g_db_mutex);
                bufferEvent(safe_event, DB_PATH);
                logEpsilonSpent(adjusted_epsilon, DB_PATH);
            }
        }
```

---

## 8. Proposed Verification Suite

To verify the correct operation of all features, we propose the following unit tests to be added to `edge-daemon/tests/differential_privacy_test.cpp`:

```cpp
#include <cstdio>
#include <cassert>

// Test Privacy Budget Tracker SQLite operations and adjustment
void testPrivacyBudgetTracker() {
    const std::string test_db = "test_budget.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);
    
    // Log budget spendings
    assert(logEpsilonSpent(0.5, test_db) == true);
    assert(logEpsilonSpent(0.3, test_db) == true);
    
    double cumulative = get24HourEpsilonBudget(test_db);
    assert(std::abs(cumulative - 0.8) < 1e-9);
    
    // Test adjustment calculation
    double budget_limit = 1.0;
    // Cumulative 0.8 is exactly 80% of budget. Epsilon should scale down.
    double adjusted = getAdjustedEpsilon(0.5, budget_limit, cumulative);
    assert(adjusted < 0.5);
    assert(adjusted >= 0.05);

    // Cumulative exceeds limit
    assert(getAdjustedEpsilon(0.5, budget_limit, 1.2) == 0.0);

    std::remove(test_db.c_str());
    std::cout << "[PASS] Local Privacy Budget Tracker Test" << std::endl;
}

// Test process scanner execution (safety checks)
void testProcessScanner() {
    auto tools = scanActiveProcesses();
    // Verify scan does not crash and handles values safely
    // (At least one terminal or shell is usually running in a development environment)
    std::cout << "[INFO] Process scanner detected tools: ";
    for (const auto& t : tools) {
        std::cout << t << " ";
    }
    std::cout << std::endl;
    std::cout << "[PASS] Native Process Scanner Test" << std::endl;
}

// Test CPU and RAM parsing validity bounds
void testResourcePerformanceTelemetry() {
    double cpu = calculateCpuUtilization();
    double ram = calculateRamUtilization();

    assert(cpu >= 0.0 && cpu <= 100.0);
    assert(ram >= 0.0 && ram <= 100.0);

    std::cout << "[INFO] Resource Telemetry -> CPU: " << cpu << "%, RAM: " << ram << "%" << std::endl;
    std::cout << "[PASS] Device Resource Performance Telemetry Test" << std::endl;
}

// Test folder creation and serialization format
void testBackupDatabaseToJson() {
    const std::string test_db = "test_backup.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);
    TelemetryEvent ev = {"test-metric", 99.9};
    bufferEvent(ev, test_db);
    logEpsilonSpent(0.4, test_db);

    assert(backupDatabaseToJson(test_db) == true);
    
    // Check that directory chronos_backups exists under ~/Downloads
    char* home_env = std::getenv("HOME");
    assert(home_env != nullptr);
    std::string home(home_env);
    std::filesystem::path backup_dir = std::filesystem::path(home) / "Downloads" / "chronos_backups";
    assert(std::filesystem::exists(backup_dir));
    assert(std::filesystem::is_directory(backup_dir));

    std::remove(test_db.c_str());
    std::cout << "[PASS] Automated Shared Folder Snapshot Test" << std::endl;
}
```
