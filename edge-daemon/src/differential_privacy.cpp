#include "differential_privacy.h"
#include <random>
#include <cmath>
#include <cctype>
#include <ctime>
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstring>

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

// Define global configuration
DPConfig g_config;

double generateLaplaceNoise(double sensitivity, double epsilon) {
    // scale (b) = sensitivity / epsilon
    double b = sensitivity / epsilon;

    // Use thread_local to ensure random engine is properly initialized per thread
    thread_local std::mt19937 generator(std::random_device{}());
    
    // Generate a uniform random variable u in the range (-0.5, 0.5)
    std::uniform_real_distribution<double> distribution(-0.5, 0.5);
    double u = distribution(generator);
    
    // Inverse cumulative distribution function for Laplace
    return -b * (u > 0 ? 1 : -1) * std::log(1.0 - 2.0 * std::abs(u));
}

TelemetryEvent anonymizeEvent(const TelemetryEvent& raw_event, double epsilon) {
    TelemetryEvent anonymized = raw_event;
    
    // Use the sensitivity from global configuration
    double noise = generateLaplaceNoise(g_config.sensitivity, epsilon);
    anonymized.value += noise;
    
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
        return clean;
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
    sqlite3_bind_double(stmt, 2, event.value);
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

    const char* sql = "SELECT id, metric_name, value FROM telemetry_events ORDER BY id ASC;";
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

