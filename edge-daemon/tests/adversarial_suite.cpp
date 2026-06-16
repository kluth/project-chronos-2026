#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <sqlite3.h>
#include "differential_privacy.h"

// Helper function to get memory usage of current process (RSS in KB)
long getProcessMemoryUsage() {
    std::ifstream stat_stream("/proc/self/status", std::ios_base::in);
    std::string line;
    while (std::getline(stat_stream, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::stringstream ss(line);
            std::string key;
            long value;
            ss >> key >> value;
            return value;
        }
    }
    return -1;
}

// 1. TOCTOU Concurrent Signature Replay Attempts Test
void testConcurrentSignatureReplay() {
    std::cout << "[RUNNING] TOCTOU Concurrent Signature Replay Test..." << std::endl;
    
    std::string original_secret = g_config.secret;
    g_config.secret = "adversarial_concurrent_secret";
    
    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string ts_str = std::to_string(now_ms);
    std::string body = "{\"action\":\"pause\"}";
    
    std::string sig = computeHmacSha256(g_config.secret, "POST\n/control\n" + ts_str + "\n" + body);
    std::string request = "X-Signature: " + sig + "\r\nX-Timestamp: " + ts_str + "\r\nHost: localhost\r\n\r\n";
    
    std::atomic<bool> start_signal(false);
    std::atomic<int> success_count(0);
    std::atomic<int> fail_count(0);
    
    const int thread_count = 30;
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_count; ++i) {
        threads.push_back(std::thread([&]() {
            while (!start_signal) {
                std::this_thread::yield();
            }
            if (verifySignature("POST", "/control", request, body)) {
                success_count++;
            } else {
                fail_count++;
            }
        }));
    }
    
    start_signal = true;
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "  Success count: " << success_count << " / " << thread_count << std::endl;
    std::cout << "  Fail count: " << fail_count << " / " << thread_count << std::endl;
    
    assert(success_count == 1);
    assert(fail_count == thread_count - 1);
    
    g_config.secret = original_secret;
    std::cout << "[PASS] TOCTOU Concurrent Signature Replay Test" << std::endl;
}

// 2. DBus Session Unlocks Overriding Manual Pauses Test
void testDbusUnlocksAndManualPauses() {
    std::cout << "[RUNNING] DBus Session Unlocks Overriding Manual Pauses Test..." << std::endl;
    
    bool original_tracking_paused = g_tracking_paused;
    bool original_dbus_paused = g_dbus_paused;
    bool original_override_paused = g_override_paused;
    auto original_override_paused_until = g_override_paused_until;
    
    // Case A: Manual Pause is active. DBus locks, then unlocks.
    g_tracking_paused = true;
    g_dbus_paused = false;
    g_override_paused = false;
    assert(isTelemetryPaused() == true);
    
    // DBus Lock signal received
    g_dbus_paused = true;
    assert(isTelemetryPaused() == true);
    
    // DBus Unlock signal received
    g_dbus_paused = false;
    // Telemetry MUST still be paused because of standard manual pause!
    assert(isTelemetryPaused() == true);
    
    // Case B: Override Pause (temporary) is active. DBus locks, then unlocks.
    g_tracking_paused = false;
    g_dbus_paused = false;
    g_override_paused = true;
    g_override_paused_until = std::chrono::steady_clock::now() + std::chrono::hours(1);
    assert(isTelemetryPaused() == true);
    
    // DBus Lock signal received
    g_dbus_paused = true;
    assert(isTelemetryPaused() == true);
    
    // DBus Unlock signal received
    g_dbus_paused = false;
    // Telemetry MUST still be paused because override pause is not expired!
    assert(isTelemetryPaused() == true);
    
    // Expire override pause
    g_override_paused_until = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    assert(isTelemetryPaused() == false);
    
    // Restore states
    g_tracking_paused = original_tracking_paused;
    g_dbus_paused = original_dbus_paused;
    g_override_paused = original_override_paused;
    g_override_paused_until = original_override_paused_until;
    
    std::cout << "[PASS] DBus Session Unlocks Overriding Manual Pauses Test" << std::endl;
}

// 3. SQLite Offline Database Query OOM Stress Test
void testSqliteOomStress() {
    std::cout << "[RUNNING] SQLite Offline Database Query OOM Stress Test..." << std::endl;
    
    const std::string oom_db = "test_oom_stress.db";
    std::remove(oom_db.c_str());
    
    assert(initDatabase(oom_db) == true);
    assert(initPrivacyBudgetTable(oom_db) == true);
    
    // Insert 150,000 telemetry events using transaction for speed
    sqlite3* db = nullptr;
    int rc = sqlite3_open(oom_db.c_str(), &db);
    assert(rc == SQLITE_OK);
    
    rc = sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    assert(rc == SQLITE_OK);
    
    const char* sql = "INSERT INTO telemetry_events (metric_name, value, timestamp) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    assert(rc == SQLITE_OK);
    
    for (int i = 0; i < 150000; ++i) {
        sqlite3_reset(stmt);
        std::string metric = "metric_oom_stress_" + std::to_string(i);
        sqlite3_bind_text(stmt, 1, metric.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, static_cast<double>(i));
        sqlite3_bind_int64(stmt, 3, std::time(nullptr));
        rc = sqlite3_step(stmt);
        assert(rc == SQLITE_DONE);
    }
    sqlite3_finalize(stmt);
    
    rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    assert(rc == SQLITE_OK);
    sqlite3_close(db);
    
    long mem_before_query = getProcessMemoryUsage();
    std::cout << "  Memory before getBufferedEvents: " << mem_before_query << " KB" << std::endl;
    
    // Call getBufferedEvents (should limit to 1000 items)
    auto events = getBufferedEvents(oom_db);
    long mem_after_query = getProcessMemoryUsage();
    std::cout << "  Memory after getBufferedEvents: " << mem_after_query << " KB" << std::endl;
    std::cout << "  Retrieved events count: " << events.size() << std::endl;
    assert(events.size() == 1000);
    
    // Verify memory difference is small (OOM protection via LIMIT 1000)
    if (mem_before_query > 0 && mem_after_query > 0) {
        long diff = mem_after_query - mem_before_query;
        std::cout << "  Memory delta: " << diff << " KB" << std::endl;
        // Should not increase memory dramatically for reading 1000 events
        assert(diff < 20000); // Allow reasonable overhead, but not unbounded size of 150,000 events
    }
    
    // Now trigger dumpBackupToJson (this serializes everything, OOM test)
    long mem_before_backup = getProcessMemoryUsage();
    std::cout << "  Memory before dumpBackupToJson (150k events): " << mem_before_backup << " KB" << std::endl;
    
    bool backup_res = dumpBackupToJson(oom_db);
    assert(backup_res == true);
    
    long mem_after_backup = getProcessMemoryUsage();
    std::cout << "  Memory after dumpBackupToJson: " << mem_after_backup << " KB" << std::endl;
    
    if (mem_before_backup > 0 && mem_after_backup > 0) {
        long backup_diff = mem_after_backup - mem_before_backup;
        std::cout << "  Backup Memory delta: " << backup_diff << " KB" << std::endl;
    }
    
    // Cleanup generated backup JSON files and database
    std::remove(oom_db.c_str());
    std::string backup_dir = getBackupDirPath();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(backup_dir, ec)) {
        if (entry.path().filename().string().rfind("chronos_backup_", 0) == 0) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
    
    std::cout << "[PASS] SQLite Offline Database Query OOM Stress Test" << std::endl;
}

// 4. Dynamic Battery Supply Unplugging Crashes Test
void testDynamicBatteryUnplugging() {
    std::cout << "[RUNNING] Dynamic Battery Supply Unplugging Crashes Test..." << std::endl;
    
    std::string mock_dir = "./mock_battery_dynamic_unplug";
    std::filesystem::remove_all(mock_dir);
    std::filesystem::create_directories(mock_dir);
    
    std::atomic<bool> active(true);
    std::atomic<int> read_count(0);
    
    // Background thread reading battery level constantly
    std::thread reader_thread([&]() {
        bool discharging = false;
        while (active) {
            getBatteryLevel(discharging, mock_dir);
            read_count++;
        }
    });
    
    // Main thread creating, modifying, and deleting directories rapidly
    for (int i = 0; i < 500; ++i) {
        std::string bat_path = mock_dir + "/BAT" + std::to_string(i % 3);
        std::filesystem::create_directories(bat_path);
        
        {
            std::ofstream status(bat_path + "/status");
            status << "Discharging\n";
            std::ofstream capacity(bat_path + "/capacity");
            capacity << std::to_string(50 + (i % 10)) << "\n";
        }
        
        std::this_thread::yield();
        
        std::filesystem::remove_all(bat_path);
        
        std::this_thread::yield();
    }
    
    active = false;
    reader_thread.join();
    
    std::filesystem::remove_all(mock_dir);
    std::cout << "  Completed battery unplug simulation. Total queries performed: " << read_count << std::endl;
    std::cout << "[PASS] Dynamic Battery Supply Unplugging Crashes Test" << std::endl;
}

// 5. Peripheral Battery Filters Test
void testPeripheralBatteryFilters() {
    std::cout << "[RUNNING] Peripheral Battery Filters Test..." << std::endl;
    
    std::string mock_dir = "./mock_battery_filters";
    std::filesystem::remove_all(mock_dir);
    std::filesystem::create_directories(mock_dir);
    
    // Create multiple power supplies
    std::filesystem::create_directories(mock_dir + "/BAT0");
    std::filesystem::create_directories(mock_dir + "/bat1");
    std::filesystem::create_directories(mock_dir + "/hid-keyboard-battery");
    std::filesystem::create_directories(mock_dir + "/wacom_battery_1");
    std::filesystem::create_directories(mock_dir + "/AC");
    
    // BAT0 (Charging, 90%)
    {
        std::ofstream status(mock_dir + "/BAT0/status");
        status << "Charging\n";
        std::ofstream capacity(mock_dir + "/BAT0/capacity");
        capacity << "90\n";
    }
    // bat1 (Discharging, 45%) - should match even with lowercase bat
    {
        std::ofstream status(mock_dir + "/bat1/status");
        status << "Discharging\n";
        std::ofstream capacity(mock_dir + "/bat1/capacity");
        capacity << "45\n";
    }
    // hid-keyboard-battery (Discharging, 12%) - should be ignored
    {
        std::ofstream status(mock_dir + "/hid-keyboard-battery/status");
        status << "Discharging\n";
        std::ofstream capacity(mock_dir + "/hid-keyboard-battery/capacity");
        capacity << "12\n";
    }
    // wacom_battery_1 (Discharging, 8%) - should be ignored
    {
        std::ofstream status(mock_dir + "/wacom_battery_1/status");
        status << "Discharging\n";
        std::ofstream capacity(mock_dir + "/wacom_battery_1/capacity");
        capacity << "8\n";
    }
    
    bool discharging = false;
    int capacity = getBatteryLevel(discharging, mock_dir);
    
    std::cout << "  Battery Level: " << capacity << "%, Discharging: " << discharging << std::endl;
    
    // Output should be 45% (from bat1), discharging should be true.
    // It must NOT be 8% or 12% (peripherals) or 90% (BAT0 charging, since bat1 discharging takes precedence).
    assert(discharging == true);
    assert(capacity == 45);
    
    // Case 2: Only internal BAT0 is charging, mouse is discharging
    std::filesystem::remove_all(mock_dir + "/bat1");
    discharging = false;
    capacity = getBatteryLevel(discharging, mock_dir);
    std::cout << "  (Internal charging only) Battery Level: " << capacity << "%, Discharging: " << discharging << std::endl;
    // Output should be 90% (from BAT0), discharging should be false (since mouse is filtered out).
    assert(discharging == false);
    assert(capacity == 90);
    
    std::filesystem::remove_all(mock_dir);
    std::cout << "[PASS] Peripheral Battery Filters Test" << std::endl;
}

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "Running Chronos SM2.4 Adversarial Validation Suite" << std::endl;
    std::cout << "===========================================" << std::endl;
    
    testConcurrentSignatureReplay();
    testDbusUnlocksAndManualPauses();
    testSqliteOomStress();
    testDynamicBatteryUnplugging();
    testPeripheralBatteryFilters();
    
    std::cout << "===========================================" << std::endl;
    std::cout << "All Chronos SM2.4 Adversarial Tests Passed!" << std::endl;
    std::cout << "===========================================" << std::endl;
    return 0;
}
