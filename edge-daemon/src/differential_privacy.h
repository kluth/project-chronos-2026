#pragma once
#include <string>
#include <vector>
#include <atomic>

extern std::atomic<bool> g_tracking_paused;

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

// F44: Device Resource Performance Telemetry
struct CpuStats {
    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;
};

bool readCpuStats(CpuStats& stats);
double calculateCpuUtilization(const CpuStats& prev, const CpuStats& curr);
bool getRamUtilization(double& ram_percent);

// F47: Automated Shared Folder Snapshots
std::string getBackupDirPath();
bool dumpBackupToJson(const std::string& db_path);


