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

void testPrivacyBudgetTracker() {
    const std::string test_db = "test_budget.db";
    std::remove(test_db.c_str());

    assert(initPrivacyBudgetTable(test_db) == true);
    
    // Cumulative budget should be 0 initially
    assert(getCumulativeEpsilon24h(test_db) == 0.0);

    // Log some epsilon usage
    assert(logPrivacyEpsilon(0.5, test_db) == true);
    assert(logPrivacyEpsilon(0.3, test_db) == true);

    // Cumulative should be 0.8
    double cum = getCumulativeEpsilon24h(test_db);
    assert(std::abs(cum - 0.8) < 1e-9);

    // Test adjustment logic
    double base_eps = 0.5;
    double budget = 1.0;
    
    // Below warning threshold (warning threshold is 0.8 * budget = 0.8)
    assert(calculateAdjustedEpsilon(base_eps, budget, 0.7) == base_eps);
    
    // At warning threshold
    assert(calculateAdjustedEpsilon(base_eps, budget, 0.8) == base_eps);

    // Midway between warning and budget (0.9 is halfway between 0.8 and 1.0)
    // ratio = (1.0 - 0.9) / (1.0 - 0.8) = 0.1 / 0.2 = 0.5
    // adjusted = base_eps * 0.5 = 0.25
    double adjusted = calculateAdjustedEpsilon(base_eps, budget, 0.9);
    assert(std::abs(adjusted - 0.25) < 1e-9);

    // Exceeding budget
    assert(calculateAdjustedEpsilon(base_eps, budget, 1.1) == 0.0);

    std::remove(test_db.c_str());
    std::cout << "[PASS] Local Privacy Budget Tracker Test" << std::endl;
}

void testProcessScanner() {
    auto tools = scanNativeProcesses();
    std::cout << "Process scanner run complete. Found " << tools.size() << " active target tools." << std::endl;
    for (const auto& t : tools) {
        std::cout << "  - " << t << std::endl;
    }
    std::cout << "[PASS] Native Process Scanner /proc Monitor Test" << std::endl;
}

void testResourcePerformanceTelemetry() {
    CpuStats stats1, stats2;
    bool read1 = readCpuStats(stats1);
    std::cout << "CPU stats reading: " << (read1 ? "Success" : "Failure") << std::endl;
    
    if (read1) {
        stats2 = stats1;
        stats2.idle += 100;
        stats2.user += 100;
        double util = calculateCpuUtilization(stats1, stats2);
        assert(std::abs(util - 50.0) < 1.0);
    }

    double ram_util = 0.0;
    bool read_ram = getRamUtilization(ram_util);
    std::cout << "RAM stats reading: " << (read_ram ? "Success" : "Failure") << std::endl;
    if (read_ram) {
        assert(ram_util >= 0.0 && ram_util <= 100.0);
    }
    std::cout << "[PASS] Device Resource Performance Telemetry Test" << std::endl;
}

void testSharedFolderSnapshots() {
    const std::string test_db = "test_snapshot.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);
    assert(initPrivacyBudgetTable(test_db) == true);

    TelemetryEvent ev = {"test-snap", 10.0};
    assert(bufferEvent(ev, test_db) == true);
    assert(logPrivacyEpsilon(0.5, test_db) == true);

    bool backup_res = dumpBackupToJson(test_db);
    assert(backup_res == true);

    std::remove(test_db.c_str());
    std::cout << "[PASS] Automated Shared Folder Snapshots Test" << std::endl;
}

int main() {
    std::cout << "Running Differential Privacy Tests..." << std::endl;
    testLaplaceNoiseDistribution();
    testAnonymizeTelemetry();
    testDomainObfuscation();
    testSQLiteBuffering();
    testNetworkChecker();
    testPrivacyBudgetTracker();
    testProcessScanner();
    testResourcePerformanceTelemetry();
    testSharedFolderSnapshots();
    std::cout << "All tests passed successfully." << std::endl;
    return 0;
}

