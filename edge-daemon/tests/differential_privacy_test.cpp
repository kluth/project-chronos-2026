#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <chrono>
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

void testTrackingPaused() {
    assert(g_tracking_paused == false);
    g_tracking_paused = true;
    assert(g_tracking_paused == true);
    g_tracking_paused = false;
    assert(g_tracking_paused == false);
    std::cout << "[PASS] Tracking Paused Atomic Flag Test" << std::endl;
}

void testBatteryPowerSaver() {
    // 1. If base_dir doesn't exist, should return -1
    bool discharging = false;
    assert(getBatteryLevel(discharging, "./non_existent_power_supply") == -1);
    assert(discharging == false);

    // Create mock directory structure
    std::string mock_dir = "./mock_sys_class_power_supply";
    std::filesystem::remove_all(mock_dir);
    std::filesystem::create_directories(mock_dir);

    // 2. Mock AC Adapter (no capacity or status files)
    std::filesystem::create_directories(mock_dir + "/AC");
    {
        std::ofstream online_file(mock_dir + "/AC/online");
        online_file << "1\n";
    }

    // Call getBatteryLevel - should return -1 as there's no battery capacity/status
    assert(getBatteryLevel(discharging, mock_dir) == -1);
    assert(discharging == false);

    // 3. Mock battery charging (charging status)
    std::filesystem::create_directories(mock_dir + "/BAT0");
    {
        std::ofstream status_file(mock_dir + "/BAT0/status");
        status_file << "Charging\n";
        std::ofstream cap_file(mock_dir + "/BAT0/capacity");
        cap_file << "15\n";
    }

    assert(getBatteryLevel(discharging, mock_dir) == 15);
    assert(discharging == false);

    // 4. Mock battery discharging
    {
        std::ofstream status_file(mock_dir + "/BAT0/status");
        status_file << "Discharging\n";
    }

    assert(getBatteryLevel(discharging, mock_dir) == 15);
    assert(discharging == true);

    // Clean up
    std::filesystem::remove_all(mock_dir);
    std::cout << "[PASS] Battery Power Saver Test" << std::endl;
}

void testSignatureVerification() {
    std::string original_secret = g_config.secret;

    // Case 1: Empty secret - verification should succeed unconditionally
    g_config.secret = "";
    assert(verifySignature("POST", "/ingest", "", "") == true);

    // Set a secret
    g_config.secret = "super_secret_key";

    // Case 2: Missing signature or timestamp
    assert(verifySignature("POST", "/ingest", "Host: localhost\r\n\r\n", "body_content") == false);

    // Helper to build headers
    auto make_request = [](const std::string& sig, const std::string& ts) {
        return "X-Signature: " + sig + "\r\nX-Timestamp: " + ts + "\r\nHost: localhost\r\n\r\n";
    };

    // Case 3: Replay attack window check (expired timestamp)
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string expired_ts = std::to_string(now_ms - 61000);
    std::string expired_sig = computeHmacSha256(g_config.secret, "POST\n/ingest\n" + expired_ts + "\nbody_content");
    assert(verifySignature("POST", "/ingest", make_request(expired_sig, expired_ts), "body_content") == false);

    // Case 4: Replay attack window check (future timestamp)
    std::string future_ts = std::to_string(now_ms + 61000);
    std::string future_sig = computeHmacSha256(g_config.secret, "POST\n/ingest\n" + future_ts + "\nbody_content");
    assert(verifySignature("POST", "/ingest", make_request(future_sig, future_ts), "body_content") == false);

    // Case 5: Signature mismatch (wrong signature value)
    std::string current_ts = std::to_string(now_ms);
    assert(verifySignature("POST", "/ingest", make_request("wrong_signature_value", current_ts), "body_content") == false);

    // Case 6: Success path
    std::string valid_sig = computeHmacSha256(g_config.secret, "POST\n/ingest\n" + current_ts + "\nbody_content");
    assert(verifySignature("POST", "/ingest", make_request(valid_sig, current_ts), "body_content") == true);

    // Case 7: Constant time compare test
    assert(constantTimeCompare("abc", "abc") == true);
    assert(constantTimeCompare("abc", "def") == false);
    assert(constantTimeCompare("abc", "abcd") == false);

    g_config.secret = original_secret;
    std::cout << "[PASS] Signature Verification Test" << std::endl;
}

void testOverridePause() {
    bool original_tracking_paused = g_tracking_paused;
    bool original_override_paused = g_override_paused;
    auto original_override_paused_until = g_override_paused_until;

    // Case 1: No pause active
    g_tracking_paused = false;
    g_override_paused = false;
    assert(isTelemetryPaused() == false);

    // Case 2: Standard tracking paused is active
    g_tracking_paused = true;
    assert(isTelemetryPaused() == true);

    // Case 3: Override pause is active and not expired
    g_tracking_paused = false;
    g_override_paused = true;
    g_override_paused_until = std::chrono::steady_clock::now() + std::chrono::hours(1);
    assert(isTelemetryPaused() == true);
    assert(g_override_paused == true);

    // Case 4: Override pause is active but expired
    g_override_paused_until = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    assert(isTelemetryPaused() == false);
    assert(g_override_paused == false);

    g_tracking_paused = original_tracking_paused;
    g_override_paused = original_override_paused;
    g_override_paused_until = original_override_paused_until;

    std::cout << "[PASS] Override Pause Test" << std::endl;
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
    testTrackingPaused();
    testBatteryPowerSaver();
    testSignatureVerification();
    testOverridePause();
    std::cout << "All tests passed successfully." << std::endl;
    return 0;
}

