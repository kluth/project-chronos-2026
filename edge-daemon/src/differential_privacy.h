#pragma once
#include <string>

struct TelemetryEvent {
    std::string metric_name;
    double value;
};

// Generates noise following the Laplace distribution centered at 0
// with scale = sensitivity / epsilon
double generateLaplaceNoise(double sensitivity, double epsilon);

// Applies epsilon-Differential Privacy to a telemetry event
TelemetryEvent anonymizeEvent(const TelemetryEvent& raw_event, double epsilon);
