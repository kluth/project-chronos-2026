#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <limits>
#include <sstream>
#include <sqlite3.h>
#include "differential_privacy.h"

// Test 1: Verify uniform variable boundary u = -0.5 is safely avoided.
// While we cannot force thread_local random generator to produce exactly -0.5,
// we can run generateLaplaceNoise many times and verify no infinite values are generated.
// We also statically verify the redraw logic in the source code.
void verifyBoundaryRedraw() {
    std::cout << "[RUNNING] verifyBoundaryRedraw..." << std::endl;
    
    // Run 10,000 iterations and verify all returned noise values are finite.
    for (int i = 0; i < 10000; ++i) {
        double noise = generateLaplaceNoise(1.0, 0.5);
        assert(std::isfinite(noise));
    }
    
    std::cout << "[PASS] verifyBoundaryRedraw (all 10000 noise values were finite)" << std::endl;
}

// Test 2: Verify epsilon rejection and clamping.
void verifyEpsilonRejection() {
    std::cout << "[RUNNING] verifyEpsilonRejection..." << std::endl;

    // epsilon = 0.0
    double noise_zero = generateLaplaceNoise(1.0, 0.0);
    assert(noise_zero == 0.0);

    // epsilon = 1e-300
    double noise_underflow = generateLaplaceNoise(1.0, 1e-300);
    assert(noise_underflow == 0.0);

    // epsilon = negative
    double noise_neg = generateLaplaceNoise(1.0, -0.5);
    assert(noise_neg == 0.0);

    // epsilon = nan
    double noise_nan = generateLaplaceNoise(1.0, std::numeric_limits<double>::quiet_NaN());
    assert(noise_nan == 0.0);

    // epsilon = inf
    double noise_inf = generateLaplaceNoise(1.0, std::numeric_limits<double>::infinity());
    assert(noise_inf == 0.0);

    // sensitivity = 0.0
    double noise_sens_zero = generateLaplaceNoise(0.0, 0.5);
    assert(noise_sens_zero == 0.0);

    // sensitivity = negative
    double noise_sens_neg = generateLaplaceNoise(-1.0, 0.5);
    assert(noise_sens_neg == 0.0);

    // sensitivity = nan
    double noise_sens_nan = generateLaplaceNoise(std::numeric_limits<double>::quiet_NaN(), 0.5);
    assert(noise_sens_nan == 0.0);

    // sensitivity = inf
    double noise_sens_inf = generateLaplaceNoise(std::numeric_limits<double>::infinity(), 0.5);
    assert(noise_sens_inf == 0.0);

    std::cout << "[PASS] verifyEpsilonRejection" << std::endl;
}

// Test 3: Verify anonymized counts are clamped to >= 0.0.
void verifyNegativeClamping() {
    std::cout << "[RUNNING] verifyNegativeClamping..." << std::endl;

    TelemetryEvent ev_keys = { "keystrokes_per_minute", 0.0 };
    TelemetryEvent ev_mouse = { "mouse_pixels_per_minute", 0.0 };

    // Run multiple times to ensure we generate negative noise and verify it is clamped
    bool saw_clamping = false;
    for (int i = 0; i < 5000; ++i) {
        TelemetryEvent anon_keys = anonymizeEvent(ev_keys, 0.1); // lower epsilon = larger noise
        TelemetryEvent anon_mouse = anonymizeEvent(ev_mouse, 0.1);
        
        assert(anon_keys.value >= 0.0);
        assert(anon_mouse.value >= 0.0);
        
        if (anon_keys.value == 0.0 || anon_mouse.value == 0.0) {
            saw_clamping = true;
        }
    }
    assert(saw_clamping); // we must have hit negative noise at least once in 5000 runs
    std::cout << "[PASS] verifyNegativeClamping" << std::endl;
}

// Test 4: Verify NaN or Inf values are clamped/rejected before SQLite database writes.
void verifySQLiteSpecialValues() {
    std::cout << "[RUNNING] verifySQLiteSpecialValues..." << std::endl;

    const std::string test_db = "test_verification.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);

    TelemetryEvent ev_nan = { "keystrokes_per_minute", std::numeric_limits<double>::quiet_NaN() };
    TelemetryEvent ev_inf = { "mouse_pixels_per_minute", std::numeric_limits<double>::infinity() };
    TelemetryEvent ev_neg_inf = { "keystrokes_per_minute", -std::numeric_limits<double>::infinity() };

    // Write should succeed because the code clamps non-finite values to 0.0
    assert(bufferEvent(ev_nan, test_db) == true);
    assert(bufferEvent(ev_inf, test_db) == true);
    assert(bufferEvent(ev_neg_inf, test_db) == true);

    // Retrieve events and verify value is clamped to 0.0
    auto events = getBufferedEvents(test_db);
    assert(events.size() == 3);
    for (const auto& ev : events) {
        std::cout << "  Stored value for " << ev.event.metric_name << ": " << ev.event.value << std::endl;
        assert(ev.event.value == 0.0);
        assert(std::isfinite(ev.event.value));
    }

    std::remove(test_db.c_str());
    std::cout << "[PASS] verifySQLiteSpecialValues" << std::endl;
}

// Test 5: Verify JSON backups contain no raw inf/nan literals.
// To test this thoroughly, we will temporarily insert non-finite values into SQLite bypass-style (if possible)
// or check that dumpBackupToJson is robust and handles non-finite values if they somehow end up in SQLite.
void verifyJSONBackupValidity() {
    std::cout << "[RUNNING] verifyJSONBackupValidity..." << std::endl;

    const std::string test_db = "test_backup_verif.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);
    assert(initPrivacyBudgetTable(test_db) == true);

    // We manually insert Inf and -Inf into the database bypass-style to test the backup serialization logic
    sqlite3* db = nullptr;
    assert(sqlite3_open(test_db.c_str(), &db) == SQLITE_OK);

    const char* sql = "INSERT INTO telemetry_events (metric_name, value, timestamp) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);

    // Bind Inf
    sqlite3_bind_text(stmt, 1, "test_inf", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, std::numeric_limits<double>::infinity());
    sqlite3_bind_int64(stmt, 3, std::time(nullptr));
    assert(sqlite3_step(stmt) == SQLITE_DONE);

    // Bind -Inf
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, "test_neg_inf", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, -std::numeric_limits<double>::infinity());
    sqlite3_bind_int64(stmt, 3, std::time(nullptr));
    assert(sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // Perform backup dump
    assert(dumpBackupToJson(test_db) == true);

    // Locate the backup file
    std::string backup_dir = getBackupDirPath();
    std::string backup_file = "";
    for (const auto& entry : std::filesystem::directory_iterator(backup_dir)) {
        if (entry.path().filename().string().rfind("chronos_backup_", 0) == 0) {
            backup_file = entry.path().string();
        }
    }
    assert(!backup_file.empty());

    // Read the backup JSON file
    std::ifstream f(backup_file);
    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string json_content = buffer.str();
    f.close();

    std::cout << "  Backup file: " << backup_file << std::endl;
    std::cout << "  Backup contents:\n" << json_content << std::endl;

    // Verify if it contains raw "nan", "inf", "-inf"
    bool contains_raw_nan = json_content.find(": nan") != std::string::npos || json_content.find(": -nan") != std::string::npos;
    bool contains_raw_inf = json_content.find(": inf") != std::string::npos || json_content.find(": -inf") != std::string::npos;

    assert(!contains_raw_nan);
    assert(!contains_raw_inf);

    // Clean up
    std::remove(test_db.c_str());
    std::filesystem::remove(backup_file);

    std::cout << "[PASS] verifyJSONBackupValidity" << std::endl;
}

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "Starting GDPR Metrics Robustness Verification Suite" << std::endl;
    std::cout << "===========================================" << std::endl;

    verifyBoundaryRedraw();
    verifyEpsilonRejection();
    verifyNegativeClamping();
    verifySQLiteSpecialValues();
    verifyJSONBackupValidity();

    std::cout << "===========================================" << std::endl;
    std::cout << "All Robustness Verification Tests Passed!" << std::endl;
    std::cout << "===========================================" << std::endl;
    return 0;
}
