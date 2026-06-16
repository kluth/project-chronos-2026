#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>

extern std::mutex g_config_mutex;
extern std::atomic<bool> g_override_paused;
extern std::chrono::steady_clock::time_point g_override_paused_until;

// F43: Encrypted IPC Bridge
std::string computeHmacSha256(const std::string& secret, const std::string& message);
bool constantTimeCompare(const std::string& a, const std::string& b);
std::string getHeaderValue(const std::string& request, const std::string& header_name);
bool verifySignature(const std::string& method, const std::string& path, const std::string& request, const std::string& body);
bool extractDouble(const std::string& json, const std::string& key, double& out_val);
bool parseDouble(const std::string& str, double& val);

// F48: Battery Power Saver
int getBatteryLevel(bool& discharging, const std::string& base_dir = "/sys/class/power_supply");

// F50: Ingestion pause status evaluator
bool isTelemetryPaused();


extern std::atomic<bool> g_tracking_paused;
extern std::atomic<bool> g_dbus_paused;

struct TelemetryEvent {
    std::string metric_name;
    double value;
};

struct DPConfig {
    double epsilon = 0.5;
    double sensitivity = 1.0;
    double budget = 0.0;
    std::string secret = "";
};

extern DPConfig g_config;

// Generates noise following the Laplace distribution centered at 0
// with scale = sensitivity / epsilon
double generateLaplaceNoise(double sensitivity, double epsilon);

// Applies epsilon-Differential Privacy to a telemetry event
TelemetryEvent anonymizeEvent(const TelemetryEvent& raw_event, double epsilon);

// F41: Crostini Network State Checker
bool hasActiveNonLoopbackInterface();
bool checkInternetRouting(const std::string& ip = "8.8.8.8", int port = 53, int timeout_ms = 500);
bool isNetworkOnline();

// F46: Local Domain Obfuscation Mapping
std::string obfuscateDomainOrCategory(const std::string& input);

// Safe curl process execution using fork/execvp
bool runCurlSafe(const std::string& metric_name, double value);

// F35: Local SQLite Offline Buffering
struct BufferedEvent {
    int id;
    TelemetryEvent event;
};

bool initDatabase(const std::string& db_path);
bool bufferEvent(const TelemetryEvent& event, const std::string& db_path);
std::vector<BufferedEvent> getBufferedEvents(const std::string& db_path);
bool deleteBufferedEvent(int id, const std::string& db_path);
void flushBuffer(const std::string& db_path);

// F39: Local Privacy Budget Tracker
bool initPrivacyBudgetTable(const std::string& db_path);
bool logPrivacyEpsilon(double epsilon, const std::string& db_path);
double getCumulativeEpsilon24h(const std::string& db_path);
double calculateAdjustedEpsilon(double base_epsilon, double budget, double cumulative_epsilon);

// F37: Native Process Scanner /proc Monitor
std::vector<std::string> scanNativeProcesses();

// F47: Automated Shared Folder Snapshots
std::string getBackupDirPath();
bool dumpBackupToJson(const std::string& db_path);


