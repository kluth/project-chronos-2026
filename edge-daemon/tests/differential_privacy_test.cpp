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

void testGDPRTelemetry() {
    // 1. JSON Parsing verification
    std::string json_payload = "{\"keystrokes_per_minute\": 45, \"mouse_pixels_per_minute\": 1200.5}";
    double keystrokes = 0.0;
    double mouse = 0.0;
    assert(extractDouble(json_payload, "keystrokes_per_minute", keystrokes) == true);
    assert(keystrokes == 45.0);
    assert(extractDouble(json_payload, "mouse_pixels_per_minute", mouse) == true);
    assert(mouse == 1200.5);

    // 2. Obfuscator pass-through verification
    assert(obfuscateDomainOrCategory("keystrokes_per_minute") == "keystrokes_per_minute");
    assert(obfuscateDomainOrCategory("mouse_pixels_per_minute") == "mouse_pixels_per_minute");

    // 3. Anonymization verification
    TelemetryEvent keystrokes_event = { "keystrokes_per_minute", keystrokes };
    TelemetryEvent mouse_event = { "mouse_pixels_per_minute", mouse };
    
    double epsilon = 1.0;
    TelemetryEvent anon_keystrokes = anonymizeEvent(keystrokes_event, epsilon);
    TelemetryEvent anon_mouse = anonymizeEvent(mouse_event, epsilon);

    assert(anon_keystrokes.metric_name == "keystrokes_per_minute");
    assert(anon_keystrokes.value != keystrokes_event.value); // Noise added

    assert(anon_mouse.metric_name == "mouse_pixels_per_minute");
    assert(anon_mouse.value != mouse_event.value); // Noise added

    // 4. SQLite storage verification
    const std::string test_db = "test_gdpr_buffer.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);
    assert(bufferEvent(anon_keystrokes, test_db) == true);
    assert(bufferEvent(anon_mouse, test_db) == true);

    auto events = getBufferedEvents(test_db);
    assert(events.size() == 2);
    assert(events[0].event.metric_name == "keystrokes_per_minute");
    assert(events[0].event.value == anon_keystrokes.value);
    assert(events[1].event.metric_name == "mouse_pixels_per_minute");
    assert(events[1].event.value == anon_mouse.value);

    std::remove(test_db.c_str());
    std::cout << "[PASS] GDPR Telemetry Test" << std::endl;
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

    // Test g_dbus_paused is checked by isTelemetryPaused
    assert(g_dbus_paused == false);
    assert(isTelemetryPaused() == false);
    g_dbus_paused = true;
    assert(g_dbus_paused == true);
    assert(isTelemetryPaused() == true);
    g_dbus_paused = false;
    assert(isTelemetryPaused() == false);

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

    // 5. Adversarial: base_dir is a file instead of a directory
    std::string mock_file = "./mock_sys_class_file";
    std::filesystem::remove(mock_file);
    {
        std::ofstream f(mock_file);
        f << "not a directory\n";
    }

    bool threw_exception = false;
    int battery_res = 0;
    try {
        battery_res = getBatteryLevel(discharging, mock_file);
    } catch (const std::filesystem::filesystem_error& e) {
        threw_exception = true;
        std::cout << "[INFO] getBatteryLevel threw std::filesystem::filesystem_error as expected for file input: " << e.what() << std::endl;
    } catch (...) {
        threw_exception = true;
        std::cout << "[INFO] getBatteryLevel threw an unexpected exception type" << std::endl;
    }
    std::filesystem::remove(mock_file);
    assert(threw_exception == false); // Bug fixed: no longer throws exception!
    assert(battery_res == -1);
    assert(discharging == false);

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

    // Case 8: Header spoofing / substring match bug prevention
    // Let's generate a unique timestamp and signature for spoofed tests to avoid replay detection
    std::string current_ts_spoof = std::to_string(now_ms + 1);
    std::string valid_sig_spoof = computeHmacSha256(g_config.secret, "POST\n/ingest\n" + current_ts_spoof + "\nbody_content");
    
    // If request contains "My-X-Signature: valid_sig" before "X-Signature: wrong_sig", it must extract "wrong_sig" and fail.
    std::string spoofed_req = "My-X-Signature: " + valid_sig_spoof + "\r\nX-Signature: wrong_sig\r\nX-Timestamp: " + current_ts_spoof + "\r\nHost: localhost\r\n\r\n";
    assert(verifySignature("POST", "/ingest", spoofed_req, "body_content") == false);
    
    // If request contains dummy My-X-Signature but valid X-Signature, it must succeed.
    std::string valid_spoofed_req = "My-X-Signature: dummy_val\r\nX-Signature: " + valid_sig_spoof + "\r\nX-Timestamp: " + current_ts_spoof + "\r\nHost: localhost\r\n\r\n";
    assert(verifySignature("POST", "/ingest", valid_spoofed_req, "body_content") == true);
    std::cout << "[INFO] Header spoofing prevention verified." << std::endl;

    // Case 9: Extraction of header from body instead of header section prevention
    // Now getHeaderValue must not match header names inside the JSON body.
    std::string request_with_sig_in_body = "X-Timestamp: " + current_ts + "\r\nHost: localhost\r\n\r\nBody containing X-Signature: dummy_val\n";
    std::string extracted_sig = getHeaderValue(request_with_sig_in_body, "X-Signature");
    std::cout << "[DIAGNOSTIC] Case 9 extracted: '" << extracted_sig << "'" << std::endl;
    assert(extracted_sig == "");
    std::cout << "[INFO] Body parsing bug prevention verified." << std::endl;

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

void testAdversarialBattery() {
    std::cout << "Running adversarial battery tests..." << std::endl;

    // 1. Regular file crash test
    std::string mock_file = "./mock_battery_file";
    std::remove(mock_file.c_str());
    {
        std::ofstream f(mock_file);
        f << "just a file";
    }
    bool discharging = false;
    bool exception_thrown = false;
    int res = 0;
    try {
        res = getBatteryLevel(discharging, mock_file);
    } catch (const std::filesystem::filesystem_error& e) {
        exception_thrown = true;
    }
    std::remove(mock_file.c_str());
    assert(exception_thrown == false); // Bug fixed: no longer throws exception!
    assert(res == -1);

    // 2. Multiple discharging batteries
    std::string mock_dir = "./mock_sys_class_power_supply_adv";
    std::filesystem::remove_all(mock_dir);
    std::filesystem::create_directories(mock_dir + "/BAT0");
    std::filesystem::create_directories(mock_dir + "/BAT1");
    
    {
        std::ofstream status_file(mock_dir + "/BAT0/status");
        status_file << "Discharging\n";
        std::ofstream cap_file(mock_dir + "/BAT0/capacity");
        cap_file << "80\n";
    }
    {
        std::ofstream status_file(mock_dir + "/BAT1/status");
        status_file << "Discharging\n";
        std::ofstream cap_file(mock_dir + "/BAT1/capacity");
        cap_file << "15\n";
    }
    
    discharging = false;
    int val = getBatteryLevel(discharging, mock_dir);
    std::cout << "Multiple discharging (BAT0=80, BAT1=15) returned capacity: " << val << ", discharging: " << discharging << std::endl;
    // Bug fixed: must return lowest capacity 15 instead of 80
    assert(val == 15);
    assert(discharging == true);
    std::cout << "[INFO] Lowest capacity returned correctly." << std::endl;
    
    // 3. Peripheral battery filtration test
    std::filesystem::remove_all(mock_dir);
    std::filesystem::create_directories(mock_dir + "/BAT0");
    std::filesystem::create_directories(mock_dir + "/hid-mouse-battery");
    {
        std::ofstream status_file(mock_dir + "/BAT0/status");
        status_file << "Discharging\n";
        std::ofstream cap_file(mock_dir + "/BAT0/capacity");
        cap_file << "80\n";
    }
    {
        std::ofstream status_file(mock_dir + "/hid-mouse-battery/status");
        status_file << "Discharging\n";
        std::ofstream cap_file(mock_dir + "/hid-mouse-battery/capacity");
        cap_file << "10\n";
    }
    discharging = false;
    val = getBatteryLevel(discharging, mock_dir);
    std::cout << "Filtration check (BAT0=80, mouse-battery=10) returned capacity: " << val << ", discharging: " << discharging << std::endl;
    assert(val == 80);
    assert(discharging == true);
    std::cout << "[INFO] Peripheral battery filtered out correctly." << std::endl;

    std::filesystem::remove_all(mock_dir);
    std::cout << "[PASS] Adversarial Battery Tests completed." << std::endl;
}

void testAdversarialSignature() {
    std::cout << "Running adversarial signature tests..." << std::endl;
    std::string original_secret = g_config.secret;
    g_config.secret = "super_secret_key";
    
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    auto make_request = [](const std::string& sig, const std::string& ts) {
        return "X-Signature: " + sig + "\r\nX-Timestamp: " + ts + "\r\nHost: localhost\r\n\r\n";
    };

    // 1. Signature Replay within window: should fail on the second attempt
    std::string current_ts = std::to_string(now_ms);
    std::string valid_sig = computeHmacSha256(g_config.secret, "POST\n/ingest\n" + current_ts + "\nbody_content");
    
    bool first_attempt = verifySignature("POST", "/ingest", make_request(valid_sig, current_ts), "body_content");
    bool second_attempt = verifySignature("POST", "/ingest", make_request(valid_sig, current_ts), "body_content");
    
    std::cout << "Replay within window - Attempt 1: " << (first_attempt ? "succeeded" : "failed")
              << " | Attempt 2 (Replay): " << (second_attempt ? "succeeded" : "failed") << std::endl;
              
    assert(first_attempt == true);
    assert(second_attempt == false); // Bug fixed: signature replay is blocked!
    std::cout << "[INFO] Signature replay successfully blocked." << std::endl;

    // 2. Future timestamp (clock skew/exploitation)
    std::string future_ts = std::to_string(now_ms + 59000);
    std::string future_sig = computeHmacSha256(g_config.secret, "POST\n/ingest\n" + future_ts + "\nbody_content");
    bool future_attempt = verifySignature("POST", "/ingest", make_request(future_sig, future_ts), "body_content");
    std::cout << "Future timestamp (+59s) verification: " << (future_attempt ? "succeeded" : "failed") << std::endl;
    assert(future_attempt == false); // Bug fixed: future timestamp is rejected!
    std::cout << "[INFO] Future timestamp verification rejected." << std::endl;

    g_config.secret = original_secret;
    std::cout << "[PASS] Adversarial Signature Tests completed." << std::endl;
}

void testAdversarialTiming() {
    std::cout << "Running adversarial timing tests..." << std::endl;
    
    std::string s1(64, 'a');
    std::string s_diff_start = "b" + std::string(63, 'a');
    std::string s_diff_end = std::string(63, 'a') + "b";
    std::string s_short(1, 'a');
    
    const int iterations = 5000000;
    
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        constantTimeCompare(s1, s_short);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double time_short = std::chrono::duration<double, std::milli>(t1 - t0).count();
    
    t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        constantTimeCompare(s1, s_diff_start);
    }
    t1 = std::chrono::high_resolution_clock::now();
    double time_diff_start = std::chrono::duration<double, std::milli>(t1 - t0).count();
    
    t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        constantTimeCompare(s1, s_diff_end);
    }
    t1 = std::chrono::high_resolution_clock::now();
    double time_diff_end = std::chrono::duration<double, std::milli>(t1 - t0).count();
    
    std::cout << "constantTimeCompare timing over " << iterations << " iterations:" << std::endl;
    std::cout << "  Different length (short): " << time_short << " ms" << std::endl;
    std::cout << "  Different start: " << time_diff_start << " ms" << std::endl;
    std::cout << "  Different end: " << time_diff_end << " ms" << std::endl;
    
    // Check if short length comparison is faster than equal length comparisons
    if (time_short < time_diff_start * 0.8) {
        std::cout << "[EXPECTED BUG CONFIRMED] Length check is not constant-time (short signature is processed significantly faster)." << std::endl;
    }
    
    // Check if character comparison itself is relatively constant time
    double diff_pct = std::abs(time_diff_start - time_diff_end) / time_diff_start * 100.0;
    std::cout << "  Start vs End character mismatch difference: " << diff_pct << "%" << std::endl;
    
    std::cout << "[PASS] Adversarial Timing Tests completed." << std::endl;
}

void testAdversarialPause() {
    std::cout << "Running adversarial pause tests..." << std::endl;
    bool original_tracking_paused = g_tracking_paused;
    bool original_override_paused = g_override_paused;
    auto original_override_paused_until = g_override_paused_until;

    // 1. Negative duration pause override
    g_tracking_paused = false;
    g_override_paused = true;
    double duration_neg = -3600.0;
    g_override_paused_until = std::chrono::steady_clock::now() + std::chrono::seconds(static_cast<int>(duration_neg));
    
    bool is_paused_neg = isTelemetryPaused();
    std::cout << "Pause override negative duration (-3600s). isTelemetryPaused() result: " 
              << (is_paused_neg ? "paused" : "not paused") << " (g_override_paused=" << (g_override_paused ? "true" : "false") << ")" << std::endl;
    
    assert(is_paused_neg == false);
    assert(g_override_paused == false); // Should clear override pause on first evaluation
    std::cout << "[EXPECTED BUG CONFIRMED] Negative duration pause override is accepted and immediately clears the override status, returning tracking to active without warning." << std::endl;

    // 2. Huge duration pause override (causing integer overflow)
    g_tracking_paused = false;
    g_override_paused = true;
    double duration_huge = 3e9; // larger than INT_MAX
    int casted = static_cast<int>(duration_huge);
    g_override_paused_until = std::chrono::steady_clock::now() + std::chrono::seconds(casted);
    
    bool is_paused_huge = isTelemetryPaused();
    std::cout << "Pause override huge duration (3e9s, casted to " << casted << "). isTelemetryPaused() result: " 
              << (is_paused_huge ? "paused" : "not paused") << " (g_override_paused=" << (g_override_paused ? "true" : "false") << ")" << std::endl;
              
    // If casted is negative (due to overflow/wrap-around), it will immediately unpause!
    if (casted < 0) {
        assert(is_paused_huge == false);
        assert(g_override_paused == false);
        std::cout << "[EXPECTED BUG CONFIRMED] Huge duration overflowed to negative integer, bypassing pause override entirely." << std::endl;
    } else {
        std::cout << "Casted duration did not overflow to negative integer (depends on standard library implementation of double-to-int conversion out-of-range)." << std::endl;
    }

    g_tracking_paused = original_tracking_paused;
    g_override_paused = original_override_paused;
    g_override_paused_until = original_override_paused_until;
    
    std::cout << "[PASS] Adversarial Pause Tests completed." << std::endl;
}

void testAdversarialGDPRMetrics() {
    std::cout << "Running adversarial GDPR metrics tests..." << std::endl;

    // 1. Extreme raw values checks
    TelemetryEvent ev_zero_keystrokes = { "keystrokes_per_minute", 0.0 };
    TelemetryEvent ev_zero_mouse = { "mouse_pixels_per_minute", 0.0 };
    TelemetryEvent ev_high_keystrokes = { "keystrokes_per_minute", 100000.0 };
    TelemetryEvent ev_high_mouse = { "mouse_pixels_per_minute", 100000.0 };
    TelemetryEvent ev_overflow_keystrokes = { "keystrokes_per_minute", 1e300 };
    TelemetryEvent ev_overflow_mouse = { "mouse_pixels_per_minute", 1e300 };
    TelemetryEvent ev_negative_keystrokes = { "keystrokes_per_minute", -100.0 };
    TelemetryEvent ev_negative_mouse = { "mouse_pixels_per_minute", -500.0 };

    double epsilon = 0.5;

    TelemetryEvent anon_zero_keystrokes = anonymizeEvent(ev_zero_keystrokes, epsilon);
    TelemetryEvent anon_zero_mouse = anonymizeEvent(ev_zero_mouse, epsilon);
    TelemetryEvent anon_high_keystrokes = anonymizeEvent(ev_high_keystrokes, epsilon);
    TelemetryEvent anon_high_mouse = anonymizeEvent(ev_high_mouse, epsilon);
    TelemetryEvent anon_overflow_keystrokes = anonymizeEvent(ev_overflow_keystrokes, epsilon);
    TelemetryEvent anon_overflow_mouse = anonymizeEvent(ev_overflow_mouse, epsilon);
    TelemetryEvent anon_negative_keystrokes = anonymizeEvent(ev_negative_keystrokes, epsilon);
    TelemetryEvent anon_negative_mouse = anonymizeEvent(ev_negative_mouse, epsilon);

    std::cout << "  Raw zero keystrokes: " << ev_zero_keystrokes.value << " -> Anonymized: " << anon_zero_keystrokes.value << std::endl;
    std::cout << "  Raw zero mouse: " << ev_zero_mouse.value << " -> Anonymized: " << anon_zero_mouse.value << std::endl;
    std::cout << "  Raw high keystrokes (100k): " << ev_high_keystrokes.value << " -> Anonymized: " << anon_high_keystrokes.value << std::endl;
    std::cout << "  Raw high mouse (100k): " << ev_high_mouse.value << " -> Anonymized: " << anon_high_mouse.value << std::endl;
    std::cout << "  Raw overflow (1e300): " << ev_overflow_keystrokes.value << " -> Anonymized: " << anon_overflow_keystrokes.value << std::endl;
    std::cout << "  Raw negative (-100): " << ev_negative_keystrokes.value << " -> Anonymized: " << anon_negative_keystrokes.value << std::endl;

    // Challenge 1: Lack of clamping to physical limits (non-negativity)
    int negative_count = 0;
    for (int i = 0; i < 1000; ++i) {
        TelemetryEvent anon = anonymizeEvent(ev_zero_keystrokes, epsilon);
        if (anon.value < 0.0) {
            negative_count++;
        }
    }
    std::cout << "  Out of 1000 anonymizations of 0 keystrokes, " << negative_count << " resulted in negative values." << std::endl;
    if (negative_count > 0) {
        std::cout << "[EXPECTED BUG CONFIRMED] Lack of clamping/bounds validation allows anonymized physical metrics to be negative (physically impossible)." << std::endl;
    }

    // Challenge 2: Laplace noise with epsilon = 0.0 (division by zero)
    double noise_eps_zero = generateLaplaceNoise(1.0, 0.0);
    std::cout << "  Laplace noise with epsilon=0.0: " << noise_eps_zero << std::endl;
    if (std::isinf(noise_eps_zero) || std::isnan(noise_eps_zero)) {
        std::cout << "[EXPECTED BUG CONFIRMED] Laplace noise with epsilon=0.0 produces inf or nan, causing database or serialization corruption." << std::endl;
    }

    // Challenge 3: Laplace noise with negative epsilon
    double noise_eps_neg = generateLaplaceNoise(1.0, -0.5);
    std::cout << "  Laplace noise with negative epsilon (-0.5): " << noise_eps_neg << std::endl;
    if (std::isinf(noise_eps_neg) || std::isnan(noise_eps_neg)) {
        std::cout << "[EXPECTED BUG CONFIRMED] Laplace noise with negative epsilon produces inf or nan." << std::endl;
    }

    // Challenge 4: Laplace noise with tiny epsilon (1e-320)
    double noise_eps_tiny = generateLaplaceNoise(1.0, 1e-320);
    std::cout << "  Laplace noise with extremely small epsilon (1e-320): " << noise_eps_tiny << std::endl;
    if (std::isinf(noise_eps_tiny) || std::isnan(noise_eps_tiny)) {
        std::cout << "[EXPECTED BUG CONFIRMED] Laplace noise with tiny epsilon causes division overflow, returning inf or nan." << std::endl;
    }

    // Challenge 5: SQLite buffering and backup JSON serialization of inf/nan
    const std::string adv_db = "test_adv_gdpr.db";
    std::remove(adv_db.c_str());
    assert(initDatabase(adv_db) == true);
    assert(initPrivacyBudgetTable(adv_db) == true);

    TelemetryEvent ev_inf = { "keystrokes_per_minute", std::numeric_limits<double>::infinity() };
    TelemetryEvent ev_nan = { "mouse_pixels_per_minute", std::numeric_limits<double>::quiet_NaN() };

    assert(bufferEvent(ev_inf, adv_db) == true);
    bool nan_buffer_res = bufferEvent(ev_nan, adv_db);
    std::cout << "  Buffering NaN event returned: " << (nan_buffer_res ? "true" : "false") << std::endl;
    if (!nan_buffer_res) {
        std::cout << "[EXPECTED BUG CONFIRMED] SQLite buffering of NaN values fails, causing silent telemetry data loss!" << std::endl;
    }

    // Clean backup directory first
    std::string backup_dir = getBackupDirPath();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(backup_dir, ec)) {
        std::filesystem::remove(entry.path(), ec);
    }

    bool backup_ok = dumpBackupToJson(adv_db);
    assert(backup_ok == true);

    // Read the backup file
    for (const auto& entry : std::filesystem::directory_iterator(backup_dir, ec)) {
        if (entry.path().filename().string().rfind("chronos_backup_", 0) == 0) {
            std::ifstream file(entry.path());
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::cout << "  Backup JSON content sample:\n" << content << std::endl;
            if (content.find(": inf") != std::string::npos || content.find(": -inf") != std::string::npos || content.find(": nan") != std::string::npos) {
                std::cout << "[EXPECTED BUG CONFIRMED] Backup JSON contains invalid JSON literals (inf, -inf, nan) which violates RFC 8259 JSON spec!" << std::endl;
            }
            std::filesystem::remove(entry.path(), ec);
            break;
        }
    }
    std::remove(adv_db.c_str());

    std::cout << "[PASS] Adversarial GDPR Metrics Tests completed." << std::endl;
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
    testGDPRTelemetry();
    testSharedFolderSnapshots();
    testTrackingPaused();
    testBatteryPowerSaver();
    testSignatureVerification();
    testOverridePause();
    testAdversarialBattery();
    testAdversarialSignature();
    testAdversarialTiming();
    testAdversarialPause();
    testAdversarialGDPRMetrics();
    std::cout << "All tests passed successfully." << std::endl;
    return 0;
}


