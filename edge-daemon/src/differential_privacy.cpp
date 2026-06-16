#include "differential_privacy.h"
#include <random>
#include <cmath>

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
    
    // Assuming sensitivity of 1.0 for standard metric aggregation (can be parameterized later)
    double noise = generateLaplaceNoise(1.0, epsilon);
    anonymized.value += noise;
    
    return anonymized;
}
