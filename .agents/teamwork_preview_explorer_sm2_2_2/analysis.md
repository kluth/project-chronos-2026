# Analysis Report — Local Privacy Budget Tracker, Process Scanner, Resource Telemetry, and Shared Backups

## Executive Summary
This report analyzes and proposes the design and implementation details for four edge-daemon enhancements under Sub-Milestone 2.2:
1. **Local Privacy Budget Tracker (F39)**: Persistently records consumed privacy budget (epsilon) in SQLite and dynamically scales noise or pauses ingestion as limits are approached.
2. **Native Process Scanner /proc Monitor (F37)**: Local scanner in Crostini that identifies active development tools and registers them as telemetry.
3. **Device Resource Performance Telemetry (F44)**: Parses `/proc/stat` and `/proc/meminfo` to gather CPU and RAM utilization metrics.
4. **Automated Shared Folder Snapshots (F47)**: Periodically exports the buffered SQLite events as formatted JSON documents to `~/Downloads/chronos_backups/`.

---

## 1. Observation
We observed the following definitions, implementation states, and structures in the `edge-daemon` codebase:
* **Configuration State (`/edge-daemon/src/differential_privacy.h` lines 10-15)**:
  ```cpp
  struct DPConfig {
      double epsilon = 0.5;
      double sensitivity = 1.0;
      double budget = 0.0;
      std::string secret = "";
  };
  ```
  `g_config` is defined globally, and CLI parameters for `--budget` are already parsed in `/edge-daemon/src/main.cpp` (lines 44-53).
* **Ingestion Flow (`/edge-daemon/src/main.cpp` lines 112-164)**:
  Incoming HTTP requests from the Chrome extension are parsed for active windows:
  ```cpp
  std::string active_window = "";
  size_t pos = request.find("\"window\":\"");
  // ... extracts window title ...
  TelemetryEvent safe_event = anonymizeEvent(cleaned_event, g_config.epsilon);
  ```
* **SQLite Buffering System (`/edge-daemon/src/differential_privacy.cpp` lines 288-315)**:
  `initDatabase(db_path)` sets up the SQLite database `telemetry_buffer.db` and defines the `telemetry_events` table structure.
* **Testing Suite (`/edge-daemon/tests/differential_privacy_test.cpp`)**:
  Provides tests for Laplace noise, obfuscation mapping, and SQLite buffering. Baseline tests compile and pass via CMake target `edge-daemon-tests`.

---

## 2. Logic Chain & Technical Designs

### A. Local Privacy Budget Tracker (F39)
**Reasoning**: To track epsilon consumption over a sliding 24-hour window, the system must write every event's applied epsilon value alongside a timestamp to a persistent database. As cumulative leakage approaches the configured limit, we must increase the noise (by scaling down $\epsilon$, since noise scale $b = \Delta f / \epsilon$) or block ingestion when the limit is breached.

#### SQLite Schema Modification
We introduce the `privacy_budget_log` table:
```sql
CREATE TABLE IF NOT EXISTS privacy_budget_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL,
    epsilon REAL NOT NULL
);
```

#### Differential Privacy Calculations & Noise Adjustment
* **Checking Budget**:
  $$\text{Cumulative Epsilon} = \sum \epsilon_i \quad \text{for } t_i \ge t_{\text{current}} - 86400$$
* **Breach Condition**: If $\text{Cumulative Epsilon} \ge \text{Budget}$ (where $\text{Budget} > 0$), we pause ingestion.
* **Approaching Condition**: If $\text{Cumulative Epsilon} \ge 0.8 \times \text{Budget}$:
  We calculate an adjusted epsilon:
  $$\epsilon_{\text{adjusted}} = \epsilon_{\text{configured}} \times \left( \frac{\text{Budget} - \text{Cumulative Epsilon}}{0.2 \times \text{Budget}} \right)$$
  To prevent infinite noise scale, we set a minimum threshold $\epsilon_{\text{adjusted}} \ge 0.001$.

---

### B. Native Process Scanner `/proc` Monitor (F37)
**Reasoning**: The edge daemon runs in Crostini. Since ChromeOS window monitoring does not capture processes active inside the Linux container (like local terminals, code editors, compiler runs), a direct scan of the `/proc` filesystem is necessary to complement host tracking.

#### Scanner Mechanism
We read the subdirectories in `/proc/` matching numeric PID folders. For each PID:
1. Retrieve the executable target path using `readlink("/proc/<PID>/exe")`.
2. Retrieve the argument string from `/proc/<PID>/cmdline` (handling null delimiters).
3. Match against known patterns:
   * **`vscode`**: Path or arguments containing `code` or `vscode`.
   * **`terminal`**: Executable matching `bash`, `zsh`, `tmux`, `fish`, or `terminal`.
   * **`development-tool`**: Tools matching `git`, `gcc`, `g++`, `python`, `node`.

---

### C. Device Resource Performance Telemetry (F44)
**Reasoning**: Tracking local system performance alongside active tasks helps relate productivity habits to hardware overhead. `/proc/stat` and `/proc/meminfo` expose high-frequency system stats.

#### Resource Calculations
1. **CPU Utilization %**:
   Read the first line `cpu ` of `/proc/stat` to get:
   $$\text{Total} = \text{user} + \text{nice} + \text{system} + \text{idle} + \text{iowait} + \text{irq} + \text{softirq} + \text{steal}$$
   $$\text{Idle} = \text{idle} + \text{iowait}$$
   We track delta since the last cycle:
   $$\text{CPU \%} = \frac{\Delta\text{Total} - \Delta\text{Idle}}{\Delta\text{Total}} \times 100.0$$
2. **RAM Utilization %**:
   Read `/proc/meminfo` keys `MemTotal` and `MemAvailable` (or fallback to `MemFree + Buffers + Cached`):
   $$\text{RAM \%} = \frac{\text{MemTotal} - \text{MemAvailable}}{\text{MemTotal}} \times 100.0$$

---

### D. Automated Shared Folder Snapshots (F47)
**Reasoning**: Telemetry stored locally must be backed up to the host-accessible ChromeOS Downloads directory so that data is resilient to container termination.

#### Snapshot Logic
* Check time delta since last backup. If $\ge 300$ seconds, trigger backup.
* Query all telemetry events in the `telemetry_events` table.
* Build a JSON-formatted payload string.
* Resolve backup directory `~/Downloads/chronos_backups/` and create it if necessary using `std::filesystem::create_directories`.
* Write the JSON file named using format `chronos_backup_%Y%m%d_%H%M%S.json`.

---

## 3. Proposed Code Modifications

### Proposed Additions to `/edge-daemon/src/differential_privacy.h`
```cpp
// F39: Privacy Budget Tracker
bool logEpsilon(double epsilon, const std::string& db_path);
double getCumulativeEpsilon(const std::string& db_path);

// F37: Native Process Scanner /proc Monitor
std::vector<std::string> scanLocalProcesses();

// F44: Device Resource Performance Telemetry
double calculateCPUUtilization();
double calculateRAMUtilization();

// F47: Automated Shared Folder Snapshots
bool dumpBackup(const std::string& db_path);
```

### Proposed Additions to `/edge-daemon/src/differential_privacy.cpp`
```cpp
#include <dirent.h>
#include <fstream>
#include <set>
#include <filesystem>
#include <cstdlib>
#include <limits.h>

namespace fs = std::filesystem;

// F39 Implementation
bool logEpsilon(double epsilon, const std::string& db_path) {
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

double getCumulativeEpsilon(const std::string& db_path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0.0;
    }
    
    // Prune logs older than 24 hours
    time_t cut_off = std::time(nullptr) - 86400;
    char* err_msg = nullptr;
    std::string prune_sql = "DELETE FROM privacy_budget_log WHERE timestamp < " + std::to_string(cut_off) + ";";
    sqlite3_exec(db, prune_sql.c_str(), nullptr, nullptr, &err_msg);
    if (err_msg) {
        sqlite3_free(err_msg);
    }

    const char* sql = "SELECT SUM(epsilon) FROM privacy_budget_log WHERE timestamp >= ?;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return 0.0;
    }
    sqlite3_bind_int64(stmt, 1, cut_off);
    
    double sum = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sum = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return sum;
}

// F37 Implementation
std::vector<std::string> scanLocalProcesses() {
    std::set<std::string> detected;
    DIR* dir = opendir("/proc");
    if (!dir) return {};

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        bool is_numeric = true;
        for (int i = 0; entry->d_name[i] != '\0'; ++i) {
            if (!std::isdigit(entry->d_name[i])) {
                is_numeric = false;
                break;
            }
        }
        if (!is_numeric || entry->d_name[0] == '\0') {
            continue;
        }

        std::string pid = entry->d_name;
        
        // Check executable symlink
        char exe_path[PATH_MAX];
        std::string symlink_path = "/proc/" + pid + "/exe";
        ssize_t len = readlink(symlink_path.c_str(), exe_path, sizeof(exe_path) - 1);
        std::string exe_str = "";
        if (len != -1) {
            exe_path[len] = '\0';
            exe_str = exe_path;
        }

        std::string exe_lower = "";
        for (char c : exe_str) {
            exe_lower += std::tolower(static_cast<unsigned char>(c));
        }

        // Check command line parameters
        std::ifstream cmd_file("/proc/" + pid + "/cmdline");
        std::string cmdline = "";
        if (cmd_file.is_open()) {
            char ch;
            while (cmd_file.get(ch)) {
                cmdline += (ch == '\0') ? ' ' : ch;
            }
        }
        std::string cmd_lower = "";
        for (char c : cmdline) {
            cmd_lower += std::tolower(static_cast<unsigned char>(c));
        }

        // Categorize dev tools
        if (exe_lower.find("code") != std::string::npos || cmd_lower.find("vscode") != std::string::npos) {
            detected.insert("vscode");
        } else if (exe_lower.find("bash") != std::string::npos || exe_lower.find("zsh") != std::string::npos ||
                   exe_lower.find("tmux") != std::string::npos || exe_lower.find("fish") != std::string::npos ||
                   exe_lower.find("terminal") != std::string::npos || cmd_lower.find("gnome-terminal") != std::string::npos) {
            detected.insert("terminal");
        } else if (exe_lower.find("git") != std::string::npos || exe_lower.find("gcc") != std::string::npos ||
                   exe_lower.find("g++") != std::string::npos || exe_lower.find("python") != std::string::npos ||
                   exe_lower.find("node") != std::string::npos) {
            detected.insert("development-tool");
        }
    }
    closedir(dir);
    return std::vector<std::string>(detected.begin(), detected.end());
}

// F44 Implementation
struct CPUState {
    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;
};

bool readCPUState(CPUState& state) {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return false;
    std::string line;
    if (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string cpu;
        ss >> cpu;
        if (cpu == "cpu") {
            ss >> state.user >> state.nice >> state.system >> state.idle
               >> state.iowait >> state.irq >> state.softirq >> state.steal;
            return true;
        }
    }
    return false;
}

double calculateCPUUtilization() {
    static CPUState prev;
    static bool has_prev = false;
    CPUState curr;
    if (!readCPUState(curr)) return 0.0;
    
    if (!has_prev) {
        prev = curr;
        has_prev = true;
        return 0.0;
    }
    
    unsigned long long prev_idle = prev.idle + prev.iowait;
    unsigned long long curr_idle = curr.idle + curr.iowait;
    
    unsigned long long prev_total = prev.user + prev.nice + prev.system + prev.idle +
                                   prev.iowait + prev.irq + prev.softirq + prev.steal;
    unsigned long long curr_total = curr.user + curr.nice + curr.system + curr.idle +
                                   curr.iowait + curr.irq + curr.softirq + curr.steal;
    
    double utilization = 0.0;
    if (curr_total > prev_total) {
        unsigned long long total_delta = curr_total - prev_total;
        unsigned long long idle_delta = curr_idle - prev_idle;
        utilization = 100.0 * (total_delta - idle_delta) / total_delta;
    }
    prev = curr;
    return utilization;
}

double calculateRAMUtilization() {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) return 0.0;
    
    std::string line;
    unsigned long long mem_total = 0;
    unsigned long long mem_available = 0;
    unsigned long long mem_free = 0;
    unsigned long long buffers = 0;
    unsigned long long cached = 0;
    bool has_available = false;

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string key;
        unsigned long long val;
        std::string unit;
        ss >> key >> val >> unit;
        
        if (key == "MemTotal:") {
            mem_total = val;
        } else if (key == "MemFree:") {
            mem_free = val;
        } else if (key == "MemAvailable:") {
            mem_available = val;
            has_available = true;
        } else if (key == "Buffers:") {
            buffers = val;
        } else if (key == "Cached:") {
            cached = val;
        }
    }

    if (mem_total == 0) return 0.0;

    unsigned long long used;
    if (has_available) {
        used = mem_total - mem_available;
    } else {
        used = mem_total - mem_free - buffers - cached;
    }
    return 100.0 * used / mem_total;
}

// F47 Implementation
std::string escapeJsonString(const std::string& input) {
    std::string output = "";
    for (char c : input) {
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else output += c;
    }
    return output;
}

bool dumpBackup(const std::string& db_path) {
    const char* home = std::getenv("HOME");
    if (!home) return false;
    
    std::string backup_dir = std::string(home) + "/Downloads/chronos_backups";
    try {
        if (!fs::exists(backup_dir)) {
            if (!fs::create_directories(backup_dir)) {
                return false;
            }
        }
    } catch (...) {
        return false;
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    const char* sql = "SELECT metric_name, value, timestamp FROM telemetry_events ORDER BY id ASC;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    std::string json = "[\n";
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) {
            json += ",\n";
        }
        first = false;
        
        const unsigned char* name = sqlite3_column_text(stmt, 0);
        std::string metric_name = name ? reinterpret_cast<const char*>(name) : "";
        double value = sqlite3_column_double(stmt, 1);
        long long timestamp = sqlite3_column_int64(stmt, 2);

        json += "  {\n";
        json += "    \"metric_name\": \"" + escapeJsonString(metric_name) + "\",\n";
        json += "    \"value\": " + std::to_string(value) + ",\n";
        json += "    \"timestamp\": " + std::to_string(timestamp) + "\n";
        json += "  }";
    }
    json += "\n]";
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    time_t now = std::time(nullptr);
    struct tm* t = std::localtime(&now);
    char name_buf[64];
    std::strftime(name_buf, sizeof(name_buf), "chronos_backup_%Y%m%d_%H%M%S.json", t);
    
    std::string backup_filepath = backup_dir + "/" + std::string(name_buf);
    std::ofstream out_file(backup_filepath);
    if (!out_file.is_open()) {
        return false;
    }
    out_file << json;
    out_file.close();
    return true;
}
```

### Proposed Additions to `initDatabase` in `/edge-daemon/src/differential_privacy.cpp`
```cpp
    // Add table creation for privacy_budget_log
    const char* budget_sql = "CREATE TABLE IF NOT EXISTS privacy_budget_log ("
                             "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                             "timestamp INTEGER NOT NULL, "
                             "epsilon REAL NOT NULL);";
    rc = sqlite3_exec(db, budget_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) sqlite3_free(err_msg);
        sqlite3_close(db);
        return false;
    }
```

---

## 4. Ingestion Loop Integration (`/edge-daemon/src/main.cpp`)

Modify the main TCP accept loop to integrate budget checks, device resources, process scanner, and backups:

```cpp
    while (true) {
        int new_socket = accept(server_fd, nullptr, nullptr);
        if (new_socket < 0) continue;

        char buffer[2048] = {0};
        read(new_socket, buffer, 2048);
        std::string request(buffer);

        // --- 1. PRIVACY BUDGET CHECK ---
        double cumulative_eps = getCumulativeEpsilon(DB_PATH);
        bool budget_exceeded = (g_config.budget > 0.0 && cumulative_eps >= g_config.budget);
        
        if (budget_exceeded) {
            std::cout << "Privacy Budget exceeded (Limit: " << g_config.budget 
                      << ", Consumed: " << cumulative_eps << ")! Ingestion paused." << std::endl;
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"paused\",\"reason\":\"privacy budget exceeded\"}";
            send(new_socket, response.c_str(), response.length(), 0);
            close(new_socket);
            continue;
        }

        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"ok\"}";
        send(new_socket, response.c_str(), response.length(), 0);
        close(new_socket);

        std::string active_window = "";
        size_t pos = request.find("\"window\":\"");
        if (pos != std::string::npos) {
            size_t end_pos = request.find("\"", pos + 10);
            if (end_pos != std::string::npos) {
                active_window = request.substr(pos + 10, end_pos - (pos + 10));
            }
        }

        if (!active_window.empty()) {
            // --- 2. ADJUST NOISE / CALCULATE EPSILON ---
            double current_epsilon = g_config.epsilon;
            if (g_config.budget > 0.0 && cumulative_eps >= 0.8 * g_config.budget) {
                double ratio = (g_config.budget - cumulative_eps) / (0.2 * g_config.budget);
                current_epsilon = g_config.epsilon * ratio;
                if (current_epsilon < 0.001) current_epsilon = 0.001; // Avoid division by zero noise scale
                std::cout << "[Budget Warning] Approaching limit. Epsilon reduced to: " << current_epsilon << std::endl;
            }

            std::vector<TelemetryEvent> events_to_ingest;

            // Host Window Ingestion
            TelemetryEvent event = { active_window, 10.0 };
            std::string cleaned_metric = obfuscateDomainOrCategory(event.metric_name);
            events_to_ingest.push_back({ cleaned_metric, event.value });

            // F44: Device Resource Ingestion
            double cpu_util = calculateCPUUtilization();
            double ram_util = calculateRAMUtilization();
            events_to_ingest.push_back({ "cpu_utilization", cpu_util });
            events_to_ingest.push_back({ "ram_utilization", ram_util });

            // F37: Local Processes scanner in Crostini
            std::vector<std::string> tools = scanLocalProcesses();
            for (const auto& tool : tools) {
                events_to_ingest.push_back({ tool, 10.0 });
            }

            // Ingest and apply differential privacy for each collected telemetry point
            for (const auto& raw_ev : events_to_ingest) {
                TelemetryEvent safe_event = anonymizeEvent(raw_ev, current_epsilon);
                
                // Track epsilon consumed in SQLite
                logEpsilon(current_epsilon, DB_PATH);

                if (isNetworkOnline()) {
                    flushBuffer(DB_PATH);
                    if (!runCurlSafe(safe_event.metric_name, safe_event.value)) {
                        bufferEvent(safe_event, DB_PATH);
                    }
                } else {
                    bufferEvent(safe_event, DB_PATH);
                }
            }

            // --- 4. BACKUP SNAPSHOTS TRIGGER ---
            static time_t last_snapshot = 0;
            time_t now = std::time(nullptr);
            if (now - last_snapshot >= 300) {
                if (dumpBackup(DB_PATH)) {
                    std::cout << "[Backup] Backup created successfully." << std::endl;
                }
                last_snapshot = now;
            }
        }
    }
```

---

## 5. Caveats
* **Budget Tracking Concurrency**: In high-frequency telemetry configurations, concurrent writes to the database could lock SQLite, but the current loop is synchronous, avoiding contention issues.
* **Process Detection Limitations**: Reading `/proc` targets only local Linux tools executing within the Crostini container. Host ChromeOS/Android apps are outside Crostini's namespace and will still rely on ChromeOS-extension focus hooks.
* **CPU Measurement Interval**: The first calculation of CPU utilization returns 0.0 as it acts as a calibration baseline. Subsequent measurements rely on the elapsed interval between telemetry updates.

---

## 6. Conclusion
The proposed integration coordinates privacy bounds enforcement with multi-source local performance and workflow metrics collection. Keeping all telemetry paths inside the standard `anonymizeEvent` flow guarantees differential privacy consistency. Persisting the privacy budget in SQLite prevents escaper/tamper workflows through container restarts.

---

## 7. Verification Method
1. **Compilation**: Run `make` inside the `/edge-daemon/` directory.
2. **Unit Tests**: Co-locate unit tests inside `tests/differential_privacy_test.cpp` verifying:
   * `logEpsilon` registers transactions and `getCumulativeEpsilon` correctly computes sliding sum.
   * `/proc` scanner accurately isolates test running processes.
   * `/proc/stat` and `/proc/meminfo` parser handles system strings and floats correctly.
   * `dumpBackup` writes a valid JSON file to a custom directory.
3. **Execution Verification**: Start daemon with limits:
   ```bash
   ./edge-daemon --epsilon 0.1 --budget 0.5
   ```
   Submit 6 request simulation headers to verify the system transitions to reduced epsilon state after 4 entries and rejects requests on the 6th entry.
