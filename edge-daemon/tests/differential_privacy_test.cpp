#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include "differential_privacy.h"

void testLaplaceNoiseDistribution() {
    double epsilon = 0.5;
    double sensitivity = 1.0;
    
    // Property-Based Test Concept:
    // Generate many noise samples and verify the mean converges near 0 
    // and variance approximates 2 * (sensitivity / epsilon)^2
    
    int num_samples = 10000;
    double sum = 0.0;
    double sum_sq = 0.0;

    for(int i = 0; i < num_samples; ++i) {
        double noise = generateLaplaceNoise(sensitivity, epsilon);
        sum += noise;
        sum_sq += (noise * noise);
    }

    double mean = sum / num_samples;
    double variance = (sum_sq / num_samples) - (mean * mean);
    double expected_variance = 2.0 * std::pow(sensitivity / epsilon, 2.0);

    // Assert that the mean is approximately 0 (unbiased noise)
    assert(std::abs(mean) < 0.5); 
    
    // Assert that variance is within a reasonable bound of the mathematical expectation
    assert(std::abs(variance - expected_variance) < (expected_variance * 0.2));
    
    std::cout << "[PASS] Laplace Noise Distribution Property Test (Epsilon = " << epsilon << ")" << std::endl;
}

void testAnonymizeTelemetry() {
    TelemetryEvent raw_event = { "active_minutes", 45.0 };
    double epsilon = 1.0;
    
    TelemetryEvent anonymized = anonymizeEvent(raw_event, epsilon);
    
    // Property: The anonymized value should NOT be identically equal to the raw value (privacy via noise)
    // There is a microscopic mathematical chance of it being equal, but practically 0 for floats
    assert(raw_event.value != anonymized.value);
    
    std::cout << "[PASS] Telemetry Anonymization Test" << std::endl;
}

int main() {
    std::cout << "Running Differential Privacy Tests..." << std::endl;
    testLaplaceNoiseDistribution();
    testAnonymizeTelemetry();
    std::cout << "All tests passed successfully." << std::endl;
    return 0;
}
