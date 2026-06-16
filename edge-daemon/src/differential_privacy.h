#pragma once
#include <string>
#include <vector>

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

