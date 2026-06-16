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

// 1. Verify that uniform variable boundary u = -0.5 is safely avoided.
void verifyUniformBoundaryNoiseIsFinite() {
    std::cout << "[RUNNING] verifyUniformBoundaryNoiseIsFinite..." << std::endl;
    double sensitivity = 1.0;
    double epsilon = 0.5;
    for (int i = 0; i < 100000; ++i) {
        double noise = generateLaplaceNoise(sensitivity, epsilon);
        assert(std::isfinite(noise));
    }
    std::cout << "[PASS] verifyUniformBoundaryNoiseIsFinite" << std::endl;
}

// 2. Verify that epsilon = 0.0, 1e-300, negative, nan, and inf are rejected or clamped safely.
void verifyEpsilonBoundaries() {
    std::cout << "[RUNNING] verifyEpsilonBoundaries..." << std::endl;
    double sensitivity = 1.0;

    // epsilon = 0.0 -> noise should be 0.0
    double noise_zero = generateLaplaceNoise(sensitivity, 0.0);
    std::cout << "  epsilon = 0.0 -> noise: " << noise_zero << std::endl;
    assert(noise_zero == 0.0);

    // epsilon = 1e-300 (very small) -> noise should be 0.0 (safely avoided underflow/division by zero)
    double noise_underflow = generateLaplaceNoise(sensitivity, 1e-300);
    std::cout << "  epsilon = 1e-300 -> noise: " << noise_underflow << std::endl;
    assert(noise_underflow == 0.0);

    // epsilon = -1.0 (negative) -> noise should be 0.0
    double noise_neg = generateLaplaceNoise(sensitivity, -1.0);
    std::cout << "  epsilon = -1.0 -> noise: " << noise_neg << std::endl;
    assert(noise_neg == 0.0);

    // epsilon = nan -> noise should be 0.0
    double noise_nan = generateLaplaceNoise(sensitivity, std::numeric_limits<double>::quiet_NaN());
    std::cout << "  epsilon = nan -> noise: " << noise_nan << std::endl;
    assert(noise_nan == 0.0);

    // epsilon = inf -> noise should be 0.0
    double noise_inf = generateLaplaceNoise(sensitivity, std::numeric_limits<double>::infinity());
    std::cout << "  epsilon = inf -> noise: " << noise_inf << std::endl;
    assert(noise_inf == 0.0);

    // Test anonymizeEvent with invalid epsilons to verify no crashes or propagation
    TelemetryEvent ev = { "keystrokes_per_minute", 10.0 };
    TelemetryEvent anon_zero = anonymizeEvent(ev, 0.0);
    assert(anon_zero.value == 10.0); // No noise added since generateLaplaceNoise returned 0.0

    TelemetryEvent anon_nan = anonymizeEvent(ev, std::numeric_limits<double>::quiet_NaN());
    assert(anon_nan.value == 10.0);

    std::cout << "[PASS] verifyEpsilonBoundaries" << std::endl;
}

// 3. Verify that anonymized counts (keystrokes, mouse pixels) are clamped to >= 0.0
void verifyAnonymizedCountsClamping() {
    std::cout << "[RUNNING] verifyAnonymizedCountsClamping..." << std::endl;
    double epsilon = 0.5;

    // Run 5000 trials to ensure noise is sometimes negative, and check clamping
    for (int i = 0; i < 5000; ++i) {
        TelemetryEvent ev_keys = { "keystrokes_per_minute", 0.0 };
        TelemetryEvent anon_keys = anonymizeEvent(ev_keys, epsilon);
        assert(anon_keys.value >= 0.0);

        TelemetryEvent ev_mouse = { "mouse_pixels_per_minute", 0.0 };
        TelemetryEvent anon_mouse = anonymizeEvent(ev_mouse, epsilon);
        assert(anon_mouse.value >= 0.0);
        
        TelemetryEvent ev_active = { "active_minutes", 0.0 };
        TelemetryEvent anon_active = anonymizeEvent(ev_active, epsilon);
        assert(anon_active.value >= 0.0);
        
        TelemetryEvent ev_duration = { "window_duration", 0.0 };
        TelemetryEvent anon_duration = anonymizeEvent(ev_duration, epsilon);
        assert(anon_duration.value >= 0.0);
    }
    std::cout << "[PASS] verifyAnonymizedCountsClamping" << std::endl;
}

// 4. Verify that NaN or Inf values are clamped or rejected before SQLite database writes
void verifySQLiteWriteSanitization() {
    std::cout << "[RUNNING] verifySQLiteWriteSanitization..." << std::endl;

    const std::string test_db = "test_verification_gdpr.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);
    assert(initPrivacyBudgetTable(test_db) == true);

    // bufferEvent with NaN, Inf, -Inf
    TelemetryEvent ev_nan = { "keystrokes_per_minute", std::numeric_limits<double>::quiet_NaN() };
    TelemetryEvent ev_inf = { "mouse_pixels_per_minute", std::numeric_limits<double>::infinity() };
    TelemetryEvent ev_neg_inf = { "active_minutes", -std::numeric_limits<double>::infinity() };

    assert(bufferEvent(ev_nan, test_db) == true);
    assert(bufferEvent(ev_inf, test_db) == true);
    assert(bufferEvent(ev_neg_inf, test_db) == true);

    // Verify stored values are clamped to 0.0 (not NaN/Inf)
    auto events = getBufferedEvents(test_db);
    assert(events.size() == 3);
    for (const auto& ev : events) {
        std::cout << "  Retrieved " << ev.event.metric_name << " value: " << ev.event.value << std::endl;
        assert(std::isfinite(ev.event.value));
        assert(ev.event.value == 0.0);
    }

    // Verify logPrivacyEpsilon sanitizes NaN/Inf to 0.0
    assert(logPrivacyEpsilon(std::numeric_limits<double>::quiet_NaN(), test_db) == true);
    assert(logPrivacyEpsilon(std::numeric_limits<double>::infinity(), test_db) == true);
    assert(logPrivacyEpsilon(-std::numeric_limits<double>::infinity(), test_db) == true);

    // Check budget table values using manual select to ensure no NaNs exist
    sqlite3* db = nullptr;
    assert(sqlite3_open(test_db.c_str(), &db) == SQLITE_OK);
    const char* sql = "SELECT epsilon FROM privacy_budget_log;";
    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        double eps = sqlite3_column_double(stmt, 0);
        std::cout << "  Retrieved privacy log epsilon: " << eps << std::endl;
        assert(std::isfinite(eps));
        assert(eps == 0.0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    std::remove(test_db.c_str());
    std::cout << "[PASS] verifySQLiteWriteSanitization" << std::endl;
}

// 5. Verify that JSON database backups serialize valid JSON even if non-finite values are tested
// (by directly inserting inf to database bypassing bufferEvent clamping, and asserting backup outputs valid JSON)
void verifyJsonBackupValidityWithNonFiniteValues() {
    std::cout << "[RUNNING] verifyJsonBackupValidityWithNonFiniteValues..." << std::endl;

    const std::string test_db = "test_verification_json.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);
    assert(initPrivacyBudgetTable(test_db) == true);

    // Manually insert non-finite values bypass clamping to see if JSON backup handles them safely
    sqlite3* db = nullptr;
    assert(sqlite3_open(test_db.c_str(), &db) == SQLITE_OK);
    
    // Insert Inf directly to telemetry_events
    const char* sql_insert_event = "INSERT INTO telemetry_events (metric_name, value, timestamp) VALUES (?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(db, sql_insert_event, -1, &stmt, nullptr) == SQLITE_OK);
    
    sqlite3_bind_text(stmt, 1, "test_inf", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, std::numeric_limits<double>::infinity());
    sqlite3_bind_int64(stmt, 3, 123456790);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    
    sqlite3_finalize(stmt);

    // Insert Inf directly to privacy_budget_log
    const char* sql_insert_pb = "INSERT INTO privacy_budget_log (epsilon, timestamp) VALUES (?, ?);";
    stmt = nullptr;
    assert(sqlite3_prepare_v2(db, sql_insert_pb, -1, &stmt, nullptr) == SQLITE_OK);
    
    sqlite3_bind_double(stmt, 1, std::numeric_limits<double>::infinity());
    sqlite3_bind_int64(stmt, 2, 123456792);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // Now dump backup to JSON
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

    // Read the JSON backup content
    std::ifstream f(backup_file);
    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string json_content = buffer.str();
    f.close();

    std::cout << "  Backup JSON content: " << json_content << std::endl;

    // Check that there is NO raw "nan" or "inf" in JSON numbers.
    // They must have been converted to "0.0" (e.g. `"value": 0.0` or `"epsilon": 0.0`)
    bool contains_raw_nan = json_content.find(": nan") != std::string::npos || json_content.find(": -nan") != std::string::npos;
    bool contains_raw_inf = json_content.find(": inf") != std::string::npos || json_content.find(": -inf") != std::string::npos;
    
    assert(!contains_raw_nan);
    assert(!contains_raw_inf);

    // Cleanup
    std::remove(test_db.c_str());
    std::filesystem::remove(backup_file);

    std::cout << "[PASS] verifyJsonBackupValidityWithNonFiniteValues" << std::endl;
}

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "Running GDPR Telemetry Robustness Verification Tests" << std::endl;
    std::cout << "===========================================" << std::endl;

    verifyUniformBoundaryNoiseIsFinite();
    verifyEpsilonBoundaries();
    verifyAnonymizedCountsClamping();
    verifySQLiteWriteSanitization();
    verifyJsonBackupValidityWithNonFiniteValues();

    std::cout << "===========================================" << std::endl;
    std::cout << "All GDPR Telemetry Robustness Verification Tests Passed!" << std::endl;
    std::cout << "===========================================" << std::endl;
    return 0;
}
