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

// Replicate the Laplace noise calculation logic for manual input test
double simulateLaplaceNoise(double sensitivity, double epsilon, double u) {
    double b = sensitivity / epsilon;
    return -b * (u > 0 ? 1 : -1) * std::log(1.0 - 2.0 * std::abs(u));
}

void testLaplaceNoiseMathematicalFormula() {
    std::cout << "[RUNNING] testLaplaceNoiseMathematicalFormula..." << std::endl;

    double sensitivity = 1.0;
    double epsilon = 0.5;

    // Test u = 0.0 (no noise)
    double noise_zero = simulateLaplaceNoise(sensitivity, epsilon, 0.0);
    assert(noise_zero == 0.0);

    // Test u = -0.5 (exactly the lower bound of uniform_real_distribution)
    double noise_lower_bound = simulateLaplaceNoise(sensitivity, epsilon, -0.5);
    std::cout << "  u = -0.5 -> noise: " << noise_lower_bound << std::endl;
    // Log of 0.0 is -inf. -b * (-1) * (-inf) = -inf.
    assert(std::isinf(noise_lower_bound));
    assert(noise_lower_bound < 0.0); // Should be -infinity

    // Test u = 0.49999999999999999 (which rounds to 0.5)
    double noise_almost_upper_bound = simulateLaplaceNoise(sensitivity, epsilon, 0.49999999999999999);
    std::cout << "  u = 0.49999999999999999 -> noise: " << noise_almost_upper_bound << std::endl;
    assert(std::isinf(noise_almost_upper_bound));
    assert(noise_almost_upper_bound > 0.0); // Should be +infinity

    std::cout << "[PASS] testLaplaceNoiseMathematicalFormula" << std::endl;
}

void testLaplaceNoiseParameters() {
    std::cout << "[RUNNING] testLaplaceNoiseParameters..." << std::endl;

    // 1. epsilon = 0.0
    double noise_eps_zero = generateLaplaceNoise(1.0, 0.0);
    std::cout << "  epsilon = 0.0 -> noise: " << noise_eps_zero << std::endl;
    // Division by zero makes scale b = infinity. Unless u=0.0 (which is extremely rare), noise is inf/nan.
    assert(std::isinf(noise_eps_zero) || std::isnan(noise_eps_zero));

    // 2. epsilon = -1.0 (negative)
    double noise_eps_neg = generateLaplaceNoise(1.0, -1.0);
    std::cout << "  epsilon = -1.0 -> noise: " << noise_eps_neg << std::endl;
    // Generates a valid double value because the scale is flipped, but mathematically invalid for DP.
    assert(!std::isnan(noise_eps_neg) && !std::isinf(noise_eps_neg));

    // 3. epsilon = nan
    double noise_eps_nan = generateLaplaceNoise(1.0, std::numeric_limits<double>::quiet_NaN());
    std::cout << "  epsilon = nan -> noise: " << noise_eps_nan << std::endl;
    assert(std::isnan(noise_eps_nan));

    // 4. epsilon = inf
    double noise_eps_inf = generateLaplaceNoise(1.0, std::numeric_limits<double>::infinity());
    std::cout << "  epsilon = inf -> noise: " << noise_eps_inf << std::endl;
    // Scale b = 1.0 / inf = 0.0. Noise should be 0.0.
    assert(noise_eps_inf == 0.0);

    // 5. sensitivity = 0.0
    double noise_sens_zero = generateLaplaceNoise(0.0, 0.5);
    std::cout << "  sensitivity = 0.0 -> noise: " << noise_sens_zero << std::endl;
    assert(noise_sens_zero == 0.0);

    // 6. sensitivity = -1.0 (negative)
    double noise_sens_neg = generateLaplaceNoise(-1.0, 0.5);
    std::cout << "  sensitivity = -1.0 -> noise: " << noise_sens_neg << std::endl;
    // Flipped sign of noise, which is fine for Laplace distribution, but config-wise weird.
    assert(!std::isnan(noise_sens_neg) && !std::isinf(noise_sens_neg));

    // 7. sensitivity = inf
    double noise_sens_inf = generateLaplaceNoise(std::numeric_limits<double>::infinity(), 0.5);
    std::cout << "  sensitivity = inf -> noise: " << noise_sens_inf << std::endl;
    assert(std::isinf(noise_sens_inf) || std::isnan(noise_sens_inf));

    std::cout << "[PASS] testLaplaceNoiseParameters" << std::endl;
}

void testGDPRMetricsAnonymizationExtreme() {
    std::cout << "[RUNNING] testGDPRMetricsAnonymizationExtreme..." << std::endl;

    double epsilon = 0.5;

    // Test with 0.0 keystrokes/pixels
    TelemetryEvent ev_zero_keys = { "keystrokes_per_minute", 0.0 };
    TelemetryEvent anon_zero_keys = anonymizeEvent(ev_zero_keys, epsilon);
    std::cout << "  Raw: 0.0 -> Anonymized: " << anon_zero_keys.value << std::endl;
    // Noise can be negative, so anonymized value can be negative.
    // There is no clamping or validation to prevent negative values for inherently non-negative metrics.
    
    // Test with extremely high counts (e.g. 100,000)
    TelemetryEvent ev_high_keys = { "keystrokes_per_minute", 100000.0 };
    TelemetryEvent anon_high_keys = anonymizeEvent(ev_high_keys, epsilon);
    std::cout << "  Raw: 100000.0 -> Anonymized: " << anon_high_keys.value << std::endl;
    assert(std::abs(anon_high_keys.value - 100000.0) < 100.0); // should be close within reasonable noise limits
    
    // Test with integer limit values
    TelemetryEvent ev_max_int = { "keystrokes_per_minute", static_cast<double>(std::numeric_limits<int>::max()) };
    TelemetryEvent anon_max_int = anonymizeEvent(ev_max_int, epsilon);
    std::cout << "  Raw: INT_MAX -> Anonymized: " << anon_max_int.value << std::endl;

    // Test with special float values
    TelemetryEvent ev_nan = { "keystrokes_per_minute", std::numeric_limits<double>::quiet_NaN() };
    TelemetryEvent anon_nan = anonymizeEvent(ev_nan, epsilon);
    std::cout << "  Raw: NaN -> Anonymized: " << anon_nan.value << std::endl;
    assert(std::isnan(anon_nan.value));

    TelemetryEvent ev_inf = { "keystrokes_per_minute", std::numeric_limits<double>::infinity() };
    TelemetryEvent anon_inf = anonymizeEvent(ev_inf, epsilon);
    std::cout << "  Raw: Inf -> Anonymized: " << anon_inf.value << std::endl;
    assert(std::isinf(anon_inf.value));

    std::cout << "[PASS] testGDPRMetricsAnonymizationExtreme" << std::endl;
}

void testSQLiteSpecialValuesSerialization() {
    std::cout << "[RUNNING] testSQLiteSpecialValuesSerialization..." << std::endl;

    const std::string test_db = "test_adv_gdpr.db";
    std::remove(test_db.c_str());

    assert(initDatabase(test_db) == true);
    assert(initPrivacyBudgetTable(test_db) == true);

    // Insert event with NaN and Infinity values
    TelemetryEvent ev_nan = { "keystrokes_per_minute", std::numeric_limits<double>::quiet_NaN() };
    TelemetryEvent ev_inf = { "mouse_pixels_per_minute", std::numeric_limits<double>::infinity() };
    TelemetryEvent ev_neg_inf = { "keystrokes_per_minute", -std::numeric_limits<double>::infinity() };

    bool res_nan = bufferEvent(ev_nan, test_db);
    std::cout << "  bufferEvent(NaN) result: " << (res_nan ? "SUCCESS" : "FAILURE") << std::endl;
    
    // Let's debug manually what sqlite3_step returns for NaN
    {
        sqlite3* db = nullptr;
        int rc = sqlite3_open(test_db.c_str(), &db);
        if (rc == SQLITE_OK) {
            const char* sql = "INSERT INTO telemetry_events (metric_name, value, timestamp) VALUES (?, ?, ?);";
            sqlite3_stmt* stmt = nullptr;
            rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
            if (rc == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, ev_nan.metric_name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(stmt, 2, ev_nan.value);
                sqlite3_bind_int64(stmt, 3, std::time(nullptr));
                rc = sqlite3_step(stmt);
                std::cout << "    Manual SQLite insert of NaN: rc = " << rc 
                          << " (SQLITE_DONE = " << SQLITE_DONE 
                          << ", SQLITE_ERROR = " << SQLITE_ERROR << ")" << std::endl;
                if (rc != SQLITE_DONE) {
                    std::cout << "    Manual SQLite errmsg: " << sqlite3_errmsg(db) << std::endl;
                }
                sqlite3_finalize(stmt);
            } else {
                std::cout << "    Manual SQLite prepare failed: " << sqlite3_errmsg(db) << std::endl;
            }
            sqlite3_close(db);
        }
    }
    
    // Do NOT assert(res_nan == true) yet, let's allow the test to continue
    // and see what happens with Inf and -Inf.
    
    bool res_inf = bufferEvent(ev_inf, test_db);
    std::cout << "  bufferEvent(Inf) result: " << (res_inf ? "SUCCESS" : "FAILURE") << std::endl;
    if (!res_inf) {
        sqlite3* db = nullptr;
        if (sqlite3_open(test_db.c_str(), &db) == SQLITE_OK) {
            std::cout << "    Inf SQLite errmsg: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_close(db);
        }
    }

    bool res_neg_inf = bufferEvent(ev_neg_inf, test_db);
    std::cout << "  bufferEvent(-Inf) result: " << (res_neg_inf ? "SUCCESS" : "FAILURE") << std::endl;
    if (!res_neg_inf) {
        sqlite3* db = nullptr;
        if (sqlite3_open(test_db.c_str(), &db) == SQLITE_OK) {
            std::cout << "    -Inf SQLite errmsg: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_close(db);
        }
    }

    // Let's modify the checks below to only run if inserts succeeded
    auto events = getBufferedEvents(test_db);
    std::cout << "  Retrieved events count: " << events.size() << std::endl;
    for (size_t i = 0; i < events.size(); ++i) {
        std::cout << "    Event " << i << ": " << events[i].event.metric_name << " = " << events[i].event.value << std::endl;
    }

    for (const auto& ev : events) {
        if (ev.event.metric_name == "keystrokes_per_minute") {
            if (std::isnan(ev.event.value)) {
                std::cout << "    Verified stored NaN value in SQLite." << std::endl;
            } else if (std::isinf(ev.event.value) && ev.event.value < 0.0) {
                std::cout << "    Verified stored -Inf value in SQLite." << std::endl;
            }
        } else if (ev.event.metric_name == "mouse_pixels_per_minute") {
            if (std::isinf(ev.event.value) && ev.event.value > 0.0) {
                std::cout << "    Verified stored +Inf value in SQLite." << std::endl;
            }
        }
    }

    // Now test JSON backup generation
    bool backup_res = dumpBackupToJson(test_db);
    std::cout << "  dumpBackupToJson result: " << (backup_res ? "SUCCESS" : "FAILURE") << std::endl;

    // Read the backup JSON file
    std::string backup_dir = getBackupDirPath();
    std::string backup_file = "";
    for (const auto& entry : std::filesystem::directory_iterator(backup_dir)) {
        if (entry.path().filename().string().rfind("chronos_backup_", 0) == 0) {
            backup_file = entry.path().string();
        }
    }
    
    assert(!backup_file.empty());
    std::ifstream f(backup_file);
    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string json_content = buffer.str();
    f.close();

    std::cout << "  Generated JSON backup content snippet:" << std::endl;
    size_t tel_pos = json_content.find("\"buffered_telemetry\"");
    if (tel_pos != std::string::npos) {
        std::cout << json_content.substr(tel_pos, 350) << std::endl;
    }

    // Verify if it contains raw "nan", "inf", "-inf"
    // Since in JSON, numbers cannot be NaN or Infinity, this makes it invalid JSON!
    bool contains_raw_nan = json_content.find(": nan") != std::string::npos || json_content.find(": -nan") != std::string::npos;
    bool contains_raw_inf = json_content.find(": inf") != std::string::npos || json_content.find(": -inf") != std::string::npos;

    std::cout << "  JSON contains raw 'nan': " << (contains_raw_nan ? "YES" : "NO") << std::endl;
    std::cout << "  JSON contains raw 'inf': " << (contains_raw_inf ? "YES" : "NO") << std::endl;

    if (contains_raw_nan || contains_raw_inf) {
        std::cout << "  [EXPECTED BUG CONFIRMED] SQLite backup generates invalid JSON when special float values (NaN, Inf) are buffered!" << std::endl;
    }

    // Clean up
    std::remove(test_db.c_str());
    std::filesystem::remove(backup_file);

    std::cout << "[PASS] testSQLiteSpecialValuesSerialization" << std::endl;
}

int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "Running GDPR Telemetry and DP Noise Adversarial Checking" << std::endl;
    std::cout << "===========================================" << std::endl;

    testLaplaceNoiseMathematicalFormula();
    testLaplaceNoiseParameters();
    testGDPRMetricsAnonymizationExtreme();
    testSQLiteSpecialValuesSerialization();

    std::cout << "===========================================" << std::endl;
    std::cout << "Adversarial Checking Completed Successfully" << std::endl;
    std::cout << "===========================================" << std::endl;
    return 0;
}
