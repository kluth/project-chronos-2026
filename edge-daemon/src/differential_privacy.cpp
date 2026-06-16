#include "differential_privacy.h"
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <iomanip>
#include <random>
#include <cmath>
#include <cctype>
#include <ctime>
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <filesystem>


// For Network State Checker
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <unistd.h>

// For Safe process execution
#include <sys/wait.h>

// For SQLite3
#include <sqlite3.h>

#include <mutex>
#include <unordered_map>

std::mutex g_config_mutex;
std::unordered_map<std::string, long long> g_processed_signatures;
std::mutex g_signatures_mutex;

// Define global configuration
DPConfig g_config;
std::atomic<bool> g_tracking_paused(false);
std::atomic<bool> g_dbus_paused(false);
std::atomic<bool> g_override_paused(false);
std::chrono::steady_clock::time_point g_override_paused_until;

bool parseDouble(const std::string& str, double& val) {
    std::stringstream ss(str);
    return (ss >> val) && ss.eof();
}

bool extractDouble(const std::string& json, const std::string& key, double& out_val) {
    size_t key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return false;
    
    size_t colon_pos = json.find(":", key_pos);
    if (colon_pos == std::string::npos) return false;
    
    size_t start = colon_pos + 1;
    while (start < json.size() && (std::isspace(json[start]) || json[start] == '"')) {
        start++;
    }
    
    size_t end = start;
    while (end < json.size() && (std::isdigit(json[end]) || json[end] == '.' || json[end] == '-' || json[end] == '+' || json[end] == 'e' || json[end] == 'E')) {
        end++;
    }
    
    if (end > start) {
        std::string val_str = json.substr(start, end - start);
        return parseDouble(val_str, out_val);
    }
    return false;
}

double generateLaplaceNoise(double sensitivity, double epsilon) {
    if (epsilon <= 1e-15 || !std::isfinite(epsilon) || sensitivity <= 0.0 || !std::isfinite(sensitivity)) {
        std::cerr << "[Warning] Invalid parameters for generateLaplaceNoise: sensitivity=" << sensitivity
                  << ", epsilon=" << epsilon << ". Returning 0.0 noise." << std::endl;
        return 0.0;
    }

    // scale (b) = sensitivity / epsilon
    double b = sensitivity / epsilon;

    // Use thread_local to ensure random engine is properly initialized per thread
    thread_local std::mt19937 generator(std::random_device{}());
    
    // Generate a uniform random variable u in the range (-0.5, 0.5)
    std::uniform_real_distribution<double> distribution(-0.5, 0.5);
    double u;
    do {
        u = distribution(generator);
    } while (u == -0.5 || u == 0.5);
    
    // Inverse cumulative distribution function for Laplace
    return -b * (u > 0 ? 1 : -1) * std::log(1.0 - 2.0 * std::abs(u));
}

TelemetryEvent anonymizeEvent(const TelemetryEvent& raw_event, double epsilon) {
    TelemetryEvent anonymized = raw_event;
    
    // Use the sensitivity from global configuration
    double sensitivity;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        sensitivity = g_config.sensitivity;
    }
    double noise = generateLaplaceNoise(sensitivity, epsilon);
    anonymized.value += noise;
    
    if (anonymized.metric_name == "keystrokes_per_minute" ||
        anonymized.metric_name == "mouse_pixels_per_minute" ||
        anonymized.metric_name == "active_minutes" ||
        anonymized.metric_name.find("duration") != std::string::npos ||
        anonymized.metric_name.find("_minutes") != std::string::npos ||
        anonymized.metric_name.find("active") != std::string::npos) {
        if (anonymized.value < 0.0) {
            anonymized.value = 0.0;
        }
    }
    
    return anonymized;
}

// F41: Crostini Network State Checker
bool hasActiveNonLoopbackInterface() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return false;
    }
    bool found = false;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        
        bool is_up = (ifa->ifa_flags & IFF_UP) != 0;
        bool is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
        
        if (is_up && !is_loopback) {
            int family = ifa->ifa_addr->sa_family;
            if (family == AF_INET || family == AF_INET6) {
                found = true;
                break;
            }
        }
    }
    freeifaddrs(ifaddr);
    return found;
}

bool checkInternetRouting(const std::string& ip, int port, int timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    // Set socket to non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        close(sock);
        return false;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return false;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(sock);
        return false;
    }

    int res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (res < 0) {
        if (errno != EINPROGRESS) {
            close(sock);
            return false;
        }
        
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        res = select(sock + 1, nullptr, &write_fds, nullptr, &tv);
        if (res <= 0) {
            close(sock);
            return false;
        }

        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close(sock);
            return false;
        }
    }

    close(sock);
    return true;
}

bool isNetworkOnline() {
    if (!hasActiveNonLoopbackInterface()) {
        return false;
    }
    return checkInternetRouting("8.8.8.8", 53, 500);
}

// F46: Local Domain Obfuscation Mapping
std::string obfuscateDomainOrCategory(const std::string& input) {
    if (input == "keystrokes_per_minute" || input == "mouse_pixels_per_minute") {
        return input;
    }
    if (input.empty()) {
        return "generic-activity";
    }

    std::string clean = input;
    bool is_url = false;
    size_t scheme_pos = clean.find("://");
    if (scheme_pos != std::string::npos) {
        is_url = true;
        clean = clean.substr(scheme_pos + 3);
    } else if (clean.rfind("www.", 0) == 0) {
        is_url = true;
    }

    if (is_url) {
        size_t end_pos = clean.find_first_of("/: ");
        if (end_pos != std::string::npos) {
            clean = clean.substr(0, end_pos);
        }
        if (clean.rfind("www.", 0) == 0) {
            clean = clean.substr(4);
        }
        std::string cleaned = "";
        for (char c : clean) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') {
                cleaned += c;
            }
        }
        return cleaned;
    }

    // Window title: lowercase for matching
    std::string lower = "";
    for (char c : input) {
        lower += std::tolower(static_cast<unsigned char>(c));
    }

    if (lower.find("visual studio code") != std::string::npos ||
        lower.find("vscode") != std::string::npos ||
        lower.find("vs code") != std::string::npos ||
        lower.find(".cpp") != std::string::npos ||
        lower.find(".py") != std::string::npos ||
        lower.find(".h") != std::string::npos ||
        lower.find(".json") != std::string::npos ||
        lower.find(".ts") != std::string::npos ||
        lower.find(".js") != std::string::npos) {
        return "vscode";
    }

    if (lower.find("terminal") != std::string::npos ||
        lower.find("bash") != std::string::npos ||
        lower.find("zsh") != std::string::npos ||
        lower.find("tmux") != std::string::npos ||
        lower.find("fish") != std::string::npos) {
        return "terminal";
    }

    if (lower.find("chrome") != std::string::npos ||
        lower.find("firefox") != std::string::npos ||
        lower.find("safari") != std::string::npos ||
        lower.find("browser") != std::string::npos ||
        lower.find("edge") != std::string::npos) {
        return "web-browser";
    }

    std::vector<std::string> common_extensions = {".com", ".org", ".net", ".io", ".edu", ".gov", ".co"};
    for (const auto& ext : common_extensions) {
        size_t ext_pos = lower.find(ext);
        if (ext_pos != std::string::npos) {
            size_t start = lower.rfind(' ', ext_pos);
            if (start == std::string::npos) {
                start = 0;
            } else {
                start += 1;
            }
            size_t end = lower.find(' ', ext_pos);
            if (end == std::string::npos) {
                end = lower.length();
            }
            std::string domain_candidate = input.substr(start, end - start);
            size_t cand_scheme = domain_candidate.find("://");
            if (cand_scheme != std::string::npos) {
                domain_candidate = domain_candidate.substr(cand_scheme + 3);
            }
            if (domain_candidate.rfind("www.", 0) == 0) {
                domain_candidate = domain_candidate.substr(4);
            }
            size_t cand_end = domain_candidate.find_first_of("/: ");
            if (cand_end != std::string::npos) {
                domain_candidate = domain_candidate.substr(0, cand_end);
            }
            
            std::string cleaned = "";
            for (char c : domain_candidate) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') {
                    cleaned += c;
                }
            }
            domain_candidate = cleaned;
            
            if (!domain_candidate.empty()) {
                return domain_candidate;
            }
        }
    }

    return "generic-activity";
}

// Safe curl process execution using fork/execvp
bool runCurlSafe(const std::string& metric_name, double value) {
    std::string url = "https://europe-west3-project-chronos-2026.cloudfunctions.net/ingestTelemetry";
    std::string json_data = "{\"metric\":\"" + metric_name + "\",\"val\":" + std::to_string(value) + "}";

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    } else if (pid == 0) {
        const char* args[] = {
            "curl",
            "-s",
            "-f",
            "-X", "POST",
            "-H", "Content-Type: application/json",
            "-d", json_data.c_str(),
            url.c_str(),
            nullptr
        };
        
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }

        execvp("curl", const_cast<char* const*>(args));
        _exit(127);
    } else {
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            return false;
        }
        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            return (exit_code == 0);
        }
        return false;
    }
}

// F35: Local SQLite Offline Buffering
bool initDatabase(const std::string& db_path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    const char* sql = "CREATE TABLE IF NOT EXISTS telemetry_events ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "metric_name TEXT NOT NULL, "
                      "value REAL NOT NULL, "
                      "timestamp INTEGER NOT NULL);";
    char* err_msg = nullptr;
    rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) {
            sqlite3_free(err_msg);
        }
        sqlite3_close(db);
        return false;
    }

    sqlite3_close(db);
    return true;
}

bool bufferEvent(const TelemetryEvent& event, const std::string& db_path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    const char* sql = "INSERT INTO telemetry_events (metric_name, value, timestamp) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, event.metric_name.c_str(), -1, SQLITE_TRANSIENT);
    double safe_value = event.value;
    if (!std::isfinite(event.value)) {
        std::cerr << "[Warning] bufferEvent: event value is not finite (" << event.value << ") for metric " << event.metric_name << ". Clamping to 0.0." << std::endl;
        safe_value = 0.0;
    }
    sqlite3_bind_double(stmt, 2, safe_value);
    sqlite3_bind_int64(stmt, 3, std::time(nullptr));

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return (rc == SQLITE_DONE);
}

std::vector<BufferedEvent> getBufferedEvents(const std::string& db_path) {
    std::vector<BufferedEvent> events;
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return events;
    }

    const char* sql = "SELECT id, metric_name, value FROM telemetry_events ORDER BY id ASC LIMIT 1000;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return events;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BufferedEvent be;
        be.id = sqlite3_column_int(stmt, 0);
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        be.event.metric_name = name ? reinterpret_cast<const char*>(name) : "";
        be.event.value = sqlite3_column_double(stmt, 2);
        events.push_back(be);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return events;
}

bool deleteBufferedEvent(int id, const std::string& db_path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    const char* sql = "DELETE FROM telemetry_events WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_bind_int(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return (rc == SQLITE_DONE);
}

void flushBuffer(const std::string& db_path) {
    auto buffered = getBufferedEvents(db_path);
    for (const auto& be : buffered) {
        if (runCurlSafe(be.event.metric_name, be.event.value)) {
            deleteBufferedEvent(be.id, db_path);
        } else {
            break;
        }
    }
}

// F39: Local Privacy Budget Tracker
bool initPrivacyBudgetTable(const std::string& db_path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    const char* sql = "CREATE TABLE IF NOT EXISTS privacy_budget_log ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "epsilon REAL NOT NULL, "
                      "timestamp INTEGER NOT NULL);";
    char* err_msg = nullptr;
    rc = sqlite3_exec(db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) {
            sqlite3_free(err_msg);
        }
        sqlite3_close(db);
        return false;
    }

    sqlite3_close(db);
    return true;
}

bool logPrivacyEpsilon(double epsilon, const std::string& db_path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }

    const char* sql = "INSERT INTO privacy_budget_log (epsilon, timestamp) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    double safe_epsilon = std::isnan(epsilon) || std::isinf(epsilon) ? 0.0 : epsilon;
    sqlite3_bind_double(stmt, 1, safe_epsilon);
    sqlite3_bind_int64(stmt, 2, std::time(nullptr));

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return (rc == SQLITE_DONE);
}

double getCumulativeEpsilon24h(const std::string& db_path) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return 0.0;
    }

    long long current_time = std::time(nullptr);
    long long threshold = current_time - 86400; // 24h ago

    // First clean up records older than 24h
    {
        const char* sql_cleanup = "DELETE FROM privacy_budget_log WHERE timestamp < ?;";
        sqlite3_stmt* stmt_cleanup = nullptr;
        if (sqlite3_prepare_v2(db, sql_cleanup, -1, &stmt_cleanup, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt_cleanup, 1, threshold);
            sqlite3_step(stmt_cleanup);
            sqlite3_finalize(stmt_cleanup);
        }
    }

    double cumulative_epsilon = 0.0;
    const char* sql = "SELECT SUM(epsilon) FROM privacy_budget_log WHERE timestamp >= ?;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, threshold);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            cumulative_epsilon = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return cumulative_epsilon;
}

double calculateAdjustedEpsilon(double base_epsilon, double budget, double cumulative_epsilon) {
    if (budget <= 0.0 || cumulative_epsilon >= budget) {
        return 0.0;
    }
    double warning_threshold = 0.8 * budget;
    if (cumulative_epsilon < warning_threshold) {
        return base_epsilon;
    }
    double remaining_ratio = (budget - cumulative_epsilon) / (budget - warning_threshold);
    double adjusted = base_epsilon * remaining_ratio;
    double min_epsilon = 0.005;
    if (adjusted < min_epsilon) {
        adjusted = min_epsilon;
    }
    return adjusted;
}

// F37: Native Process Scanner /proc Monitor
std::vector<std::string> scanNativeProcesses() {
    std::vector<std::string> detected;
    DIR* dir = opendir("/proc");
    if (!dir) return detected;
    
    struct dirent* entry = nullptr;
    std::set<std::string> target_tools = {
        "code", "code-oss", "bash", "zsh", "fish", "tmux", 
        "python", "python3", "node", "docker", "git", 
        "gcc", "g++", "make", "cmake"
    };
    
    std::set<std::string> found;
    
    while ((entry = readdir(dir)) != nullptr) {
        bool is_pid = true;
        for (int i = 0; entry->d_name[i] != '\0'; ++i) {
            if (!std::isdigit(entry->d_name[i])) {
                is_pid = false;
                break;
            }
        }
        if (!is_pid || entry->d_name[0] == '\0') continue;
        
        std::string comm_path = std::string("/proc/") + entry->d_name + "/comm";
        std::ifstream comm_file(comm_path);
        if (comm_file.is_open()) {
            std::string comm_name;
            std::getline(comm_file, comm_name);
            while (!comm_name.empty() && (comm_name.back() == '\n' || comm_name.back() == '\r' || comm_name.back() == ' ')) {
                comm_name.pop_back();
            }
            if (target_tools.count(comm_name)) {
                found.insert(comm_name);
            }
        }
    }
    closedir(dir);
    
    for (const auto& tool : found) {
        detected.push_back(tool);
    }
    return detected;
}



// F47: Automated Shared Folder Snapshots
std::string getBackupDirPath() {
    const char* home = std::getenv("HOME");
    std::string path;
    if (home) {
        path = std::string(home) + "/Downloads/chronos_backups";
    } else {
        path = "./chronos_backups";
    }
    return path;
}

bool dumpBackupToJson(const std::string& db_path) {
    std::string backup_dir = getBackupDirPath();
    
    try {
        std::filesystem::create_directories(backup_dir);
    } catch (const std::exception& e) {
        std::cerr << "Failed to create backup directory: " << e.what() << std::endl;
        return false;
    }
    
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << backup_dir << "/chronos_backup_" 
        << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".json";
    std::string filename = oss.str();
    
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    
    std::stringstream json;
    json << "{\n";
    json << "  \"timestamp\": " << now << ",\n";
    
    json << "  \"buffered_telemetry\": [\n";
    const char* sql_tel = "SELECT id, metric_name, value, timestamp FROM telemetry_events ORDER BY id ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql_tel, -1, &stmt, nullptr) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) json << ",\n";
            first = false;
            
            int id = sqlite3_column_int(stmt, 0);
            const unsigned char* name = sqlite3_column_text(stmt, 1);
            double val = sqlite3_column_double(stmt, 2);
            long long ts = sqlite3_column_int64(stmt, 3);
            
            std::string metric_name = name ? reinterpret_cast<const char*>(name) : "";
            std::string escaped_name = "";
            for (char c : metric_name) {
                if (c == '"' || c == '\\') {
                    escaped_name += '\\';
                }
                escaped_name += c;
            }
            
            std::string val_str;
            if (!std::isfinite(val)) {
                val_str = "0.0";
            } else {
                std::ostringstream vos;
                vos << val;
                val_str = vos.str();
            }
            
            json << "    {\n"
                 << "      \"id\": " << id << ",\n"
                 << "      \"metric_name\": \"" << escaped_name << "\",\n"
                 << "      \"value\": " << val_str << ",\n"
                 << "      \"timestamp\": " << ts << "\n"
                 << "    }";
        }
        sqlite3_finalize(stmt);
    }
    json << "\n  ],\n";
    
    json << "  \"privacy_budget_log\": [\n";
    const char* sql_pb = "SELECT id, epsilon, timestamp FROM privacy_budget_log ORDER BY id ASC;";
    stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql_pb, -1, &stmt, nullptr) == SQLITE_OK) {
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) json << ",\n";
            first = false;
            
            int id = sqlite3_column_int(stmt, 0);
            double eps = sqlite3_column_double(stmt, 1);
            long long ts = sqlite3_column_int64(stmt, 2);
            
            std::string eps_str;
            if (!std::isfinite(eps)) {
                eps_str = "0.0";
            } else {
                std::ostringstream eoss;
                eoss << eps;
                eps_str = eoss.str();
            }
            
            json << "    {\n"
                 << "      \"id\": " << id << ",\n"
                 << "      \"epsilon\": " << eps_str << ",\n"
                 << "      \"timestamp\": " << ts << "\n"
                 << "    }";
        }
        sqlite3_finalize(stmt);
    }
    json << "\n  ]\n";
    json << "}\n";
    
    sqlite3_close(db);
    
    std::ofstream out_file(filename);
    if (!out_file.is_open()) {
        std::cerr << "Failed to write backup file: " << filename << std::endl;
        return false;
    }
    out_file << json.str();
    out_file.close();
    
    std::cout << "[Backup] SQLite tables backed up to " << filename << std::endl;
    return true;
}

std::string computeHmacSha256(const std::string& secret, const std::string& message) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    
    HMAC(EVP_sha256(), secret.c_str(), secret.length(),
         reinterpret_cast<const unsigned char*>(message.c_str()), message.length(),
         hash, &length);
         
    std::stringstream ss;
    for (unsigned int i = 0; i < length; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

bool constantTimeCompare(const std::string& a, const std::string& b) {
    size_t len_a = a.length();
    size_t len_b = b.length();
    size_t cmp_len = (len_a == len_b) ? len_a : len_b;
    int diff = (len_a != len_b);
    for (size_t i = 0; i < cmp_len; ++i) {
        char ca = (i < len_a) ? a[i] : 0;
        char cb = (i < len_b) ? b[i] : 0;
        diff |= (ca ^ cb);
    }
    return diff == 0;
}

std::string getHeaderValue(const std::string& request, const std::string& header_name) {
    size_t header_end = request.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        header_end = request.find("\n\n");
    }
    std::string header_section = (header_end == std::string::npos) ? request : request.substr(0, header_end);
    
    std::istringstream stream(header_section);
    std::string line;
    std::string target_lower = header_name;
    std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(), ::tolower);
    
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) continue;
        
        std::string key = line.substr(0, colon_pos);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        size_t key_first = key.find_first_not_of(" \t");
        size_t key_last = key.find_last_not_of(" \t");
        if (key_first != std::string::npos && key_last != std::string::npos) {
            key = key.substr(key_first, key_last - key_first + 1);
        }
        
        if (key == target_lower) {
            std::string val = line.substr(colon_pos + 1);
            size_t val_first = val.find_first_not_of(" \t");
            size_t val_last = val.find_last_not_of(" \t");
            if (val_first == std::string::npos || val_last == std::string::npos) {
                return "";
            }
            return val.substr(val_first, val_last - val_first + 1);
        }
    }
    return "";
}

bool verifySignature(const std::string& method, const std::string& path, const std::string& request, const std::string& body) {
    std::string config_secret;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        config_secret = g_config.secret;
    }
    if (config_secret.empty()) {
        return true;
    }
    
    std::string sig = getHeaderValue(request, "X-Signature");
    std::string timestamp_str = getHeaderValue(request, "X-Timestamp");
    
    if (sig.empty() || timestamp_str.empty()) {
        std::cerr << "[Security IPC] Verification failed: missing signature or timestamp." << std::endl;
        return false;
    }
    
    long long request_time = 0;
    try {
        request_time = std::stoll(timestamp_str);
    } catch (...) {
        return false;
    }
    
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Future timestamp check (overflow-safe)
    if (request_time > now_ms + 5000) {
        std::cerr << "[Security IPC] Verification failed: future timestamp limit exceeded." << std::endl;
        return false;
    }
    
    // Replay attack window check (expired timestamp > 60s, overflow-safe)
    if (now_ms >= request_time) {
        if (now_ms - request_time > 60000) {
            std::cerr << "[Security IPC] Verification failed: replay attack window exceeded." << std::endl;
            return false;
        }
    }
    
    // Compute expected HMAC and compare in constant time outside the lock
    std::string message = method + "\n" + path + "\n" + timestamp_str + "\n" + body;
    std::string expected_sig = computeHmacSha256(config_secret, message);
    
    if (!constantTimeCompare(sig, expected_sig)) {
        std::cerr << "[Security IPC] Verification failed: signature mismatch." << std::endl;
        return false;
    }
    
    // Once signature is verified, perform replay check and map insertion in a single lock scope
    {
        std::lock_guard<std::mutex> lock_sig(g_signatures_mutex);
        
        // Prune signatures older than 60 seconds (overflow-safe)
        for (auto it = g_processed_signatures.begin(); it != g_processed_signatures.end(); ) {
            if (now_ms >= it->second) {
                if (now_ms - it->second > 60000) {
                    it = g_processed_signatures.erase(it);
                    continue;
                }
            } else if (it->second > now_ms + 5000) {
                it = g_processed_signatures.erase(it);
                continue;
            }
            ++it;
        }
        
        // Perform replay check
        if (g_processed_signatures.find(sig) != g_processed_signatures.end()) {
            std::cerr << "[Security IPC] Verification failed: replay attack detected." << std::endl;
            return false;
        }
        
        // Insert signature
        g_processed_signatures[sig] = request_time;
    }
    
    return true;
}

int getBatteryLevel(bool& discharging, const std::string& base_dir) {
    discharging = false;
    std::error_code ec;
    if (!std::filesystem::exists(base_dir, ec) || ec) return -1;
    
    auto it = std::filesystem::directory_iterator(base_dir, ec);
    if (ec) return -1;
    
    int min_capacity = -1;
    int lowest_discharging_capacity = -1;
    
    try {
        for (const auto& entry : it) {
            std::string filename = entry.path().filename().string();
            if (filename.size() < 3 || 
                std::toupper(static_cast<unsigned char>(filename[0])) != 'B' ||
                std::toupper(static_cast<unsigned char>(filename[1])) != 'A' ||
                std::toupper(static_cast<unsigned char>(filename[2])) != 'T') {
                continue;
            }

            std::ifstream status_file(entry.path() / "status");
            std::ifstream cap_file(entry.path() / "capacity");
            
            if (status_file.is_open() && cap_file.is_open()) {
                std::string status;
                int capacity = -1;
                if (status_file >> status && cap_file >> capacity) {
                    if (status == "Discharging") {
                        discharging = true;
                        if (lowest_discharging_capacity == -1 || capacity < lowest_discharging_capacity) {
                            lowest_discharging_capacity = capacity;
                        }
                    }
                    if (min_capacity == -1 || capacity < min_capacity) {
                        min_capacity = capacity;
                    }
                }
            }
        }
    } catch (...) {
        std::cerr << "[Battery Monitor] Exception during battery directory iteration" << std::endl;
    }
    
    if (discharging) {
        return lowest_discharging_capacity;
    }
    return min_capacity;
}

bool isTelemetryPaused() {
    if (g_tracking_paused || g_dbus_paused) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_config_mutex);
    if (g_override_paused) {
        if (std::chrono::steady_clock::now() >= g_override_paused_until) {
            g_override_paused = false;
            std::cout << "[Daemon] Override pause has expired. Resuming tracking." << std::endl;
            return false;
        }
        return true;
    }
    return false;
}


