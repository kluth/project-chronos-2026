#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <cstdio>
#include "differential_privacy.h"

void testLaplaceNoiseDistribution() {
    double epsilon = 0.5;
    double sensitivity = 1.0;
    
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

    assert(std::abs(mean) < 0.5); 
    assert(std::abs(variance - expected_variance) < (expected_variance * 0.2));
    
    std::cout << "[PASS] Laplace Noise Distribution Property Test (Epsilon = " << epsilon << ")" << std::endl;
}

void testAnonymizeTelemetry() {
    TelemetryEvent raw_event = { "active_minutes", 45.0 };
    double epsilon = 1.0;
    
    TelemetryEvent anonymized = anonymizeEvent(raw_event, epsilon);
    assert(raw_event.value != anonymized.value);
    
    std::cout << "[PASS] Telemetry Anonymization Test" << std::endl;
}

void testDomainObfuscation() {
    assert(obfuscateDomainOrCategory("https://github.com/foo/bar") == "github.com");
    assert(obfuscateDomainOrCategory("http://www.google.com/search") == "google.com");
    assert(obfuscateDomainOrCategory("www.nytimes.com") == "nytimes.com");
    assert(obfuscateDomainOrCategory("Visual Studio Code") == "vscode");
    assert(obfuscateDomainOrCategory("terminal - bash") == "terminal");
    assert(obfuscateDomainOrCategory("Chrome Browser") == "web-browser");
    assert(obfuscateDomainOrCategory("random noise") == "generic-activity");
    assert(obfuscateDomainOrCategory("Visit google.com for search") == "google.com");
    assert(obfuscateDomainOrCategory("") == "generic-activity");
    std::cout << "[PASS] Local Domain Obfuscation Mapping Test" << std::endl;
}

void testSQLiteBuffering() {
    const std::string test_db = "test_buffer.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);
    auto initial_events = getBufferedEvents(test_db);
    assert(initial_events.empty());

    TelemetryEvent ev1 = {"dev-test-1", 42.0};
    TelemetryEvent ev2 = {"dev-test-2", 84.0};

    assert(bufferEvent(ev1, test_db) == true);
    assert(bufferEvent(ev2, test_db) == true);

    auto events = getBufferedEvents(test_db);
    assert(events.size() == 2);
    assert(events[0].event.metric_name == "dev-test-1");
    assert(events[0].event.value == 42.0);
    assert(events[1].event.metric_name == "dev-test-2");
    assert(events[1].event.value == 84.0);

    assert(deleteBufferedEvent(events[0].id, test_db) == true);
    auto events_after_del = getBufferedEvents(test_db);
    assert(events_after_del.size() == 1);
    assert(events_after_del[0].event.metric_name == "dev-test-2");

    assert(deleteBufferedEvent(events_after_del[0].id, test_db) == true);
    auto final_events = getBufferedEvents(test_db);
    assert(final_events.empty());

    std::remove(test_db.c_str());
    std::cout << "[PASS] Local SQLite Offline Buffering Test" << std::endl;
}

void testNetworkChecker() {
    // Stage 1 check
    bool active_if = hasActiveNonLoopbackInterface();
    std::cout << "Network interface check result: " << (active_if ? "Active interfaces found" : "No active interfaces") << std::endl;

    // Stage 2 check on local address (should either fail or succeed depending on bindings, but won't crash)
    checkInternetRouting("127.0.0.1", 65535, 50);

    // Call combined online check to verify no crashes
    isNetworkOnline();

    std::cout << "[PASS] Crostini Network State Checker Test (verification complete)" << std::endl;
}

int main() {
    std::cout << "Running Differential Privacy Tests..." << std::endl;
    testLaplaceNoiseDistribution();
    testAnonymizeTelemetry();
    testDomainObfuscation();
    testSQLiteBuffering();
    testNetworkChecker();
    std::cout << "All tests passed successfully." << std::endl;
    return 0;
}

