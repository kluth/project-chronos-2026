# Challenge Report — GDPR Compliance Metrics Adversarial Review

## Challenge Summary

**Overall risk assessment**: HIGH

The adversarial review of the C++ daemon's GDPR compliance telemetry processing (specifically aggregate keystrokes and mouse distance metrics) has revealed significant vulnerabilities. These include physical bounds leakage, database insertion failures leading to silent data loss, and malformed JSON serialization output violating RFC 8259 when handling extreme values, zero epsilon, or tiny epsilon bounds.

---

## Challenges

### [High] Challenge 1: Lack of Clamping to Physical Lower Bounds (Negative Anonymized Metrics)
- **Assumption challenged**: The assumption that adding zero-centered Laplace noise to non-negative telemetry metrics (like keystrokes per minute and mouse pixels per minute) does not require bounding/clamping.
- **Attack scenario**: When a user is inactive or has very low activity (e.g. 0 keystrokes or 0 mouse pixels), adding Laplace noise (which can be negative) results in a negative anonymized value in approximately 50% of cases. 
- **Blast radius**: Storing or transmitting negative keystrokes/pixels is physically impossible. More importantly, this leaks private user state: a negative anonymized value strongly indicates that the true user activity was very close to `0`, undermining the goal of differential privacy. It also risks breaking downstream databases or analysis charts that expect non-negative values.
- **Mitigation**: Clamp the anonymized telemetry value to a minimum of `0.0` after adding Laplace noise.

### [High] Challenge 2: Division by Zero and Overflow in Laplace Noise (Epsilon = 0.0 / Tiny Epsilon)
- **Assumption challenged**: The assumption that epsilon (the privacy parameter) is always a well-bounded, non-zero positive number during noise generation.
- **Attack scenario**: Epsilon can become `0.0` when the daily privacy budget is exhausted (returned by `calculateAdjustedEpsilon`), or can be configured to `0.0` or a tiny value (e.g. `1e-320`) via the `/control` endpoint. This causes `generateLaplaceNoise()` to perform a division by zero or underflow division (`scale b = sensitivity / epsilon`), which yields `inf` or `-inf`.
- **Blast radius**: The anonymized telemetry event value becomes `inf` or `-inf` (or `nan` if the random uniform variable is exactly 0). Storing and exporting these values leads to serialization corruption and database errors.
- **Mitigation**: Add a validation guard in `anonymizeEvent()` and `generateLaplaceNoise()` to ensure `epsilon` is strictly greater than some small positive threshold (e.g., `epsilon >= 0.001`). If `epsilon` is below this threshold or budget is exhausted, ingestion should either be paused or the event dropped gracefully rather than generating invalid noise.

### [Medium] Challenge 3: Database Buffering Failure for NaN Values (Silent Data Loss)
- **Assumption challenged**: The assumption that SQLite database queries (`bufferEvent`) always succeed and do not require error handling.
- **Attack scenario**: If the Laplace noise generation produces `NaN` (due to floating point math edges), the daemon attempts to buffer the event in the SQLite database. SQLite rejects the insertion of `NaN` values, returning a non-DONE error.
- **Blast radius**: Because the return value of `bufferEvent` is ignored in both `backgroundTelemetryThread` and the main server loop, the event is silently lost. There is no error log or warning.
- **Mitigation**: Validate that the value to be buffered is a finite number (`std::isfinite(event.value)`) before attempting to insert it into the database. Additionally, check the return value of `bufferEvent()` and log a warning on failure.

### [High] Challenge 4: Invalid JSON Serialization in Backup (RFC 8259 Violation)
- **Assumption challenged**: The assumption that C++ `std::stringstream` formatting of `double` values automatically produces valid JSON.
- **Attack scenario**: When `inf` or `-inf` is successfully buffered (which SQLite allows as a `REAL`), `dumpBackupToJson` serializes these values as raw `inf` or `-inf` (e.g. `"value": inf,`) instead of a valid JSON number.
- **Blast radius**: Raw `inf`, `-inf`, or `nan` are syntax errors in the JSON standard (RFC 8259). The generated backup JSON files become corrupt and cannot be parsed by standard JSON parsers (e.g. JavaScript `JSON.parse` or Python `json.loads` will throw a SyntaxError).
- **Mitigation**: Ensure that values are checked for finiteness before serialization. If a value is non-finite, serialize it as `null` or a string representation (e.g., `"infinity"`), or omit the event.

---

## Stress Test Results

The test suite in `tests/differential_privacy_test.cpp` was extended with the `testAdversarialGDPRMetrics` function to evaluate these conditions. The results are as follows:

| Test Scenario | Expected Behavior | Actual Behavior | Pass/Fail |
|---|---|---|---|
| Anonymization of 0 keystrokes / pixels | Non-negative values only (clamped to 0.0) | Negative values generated in ~50% of runs (e.g., -8.95 pixels) | **FAIL** (Expected bug confirmed) |
| Laplace Noise with `epsilon = 0.0` | Graceful error/fallback, finite value | Returned `-inf` or `inf` (division by zero) | **FAIL** (Expected bug confirmed) |
| Laplace Noise with `epsilon = 1e-320` | Graceful error/fallback, finite value | Returned `inf` (division overflow) | **FAIL** (Expected bug confirmed) |
| Buffering of `NaN` values in SQLite | Safe validation/rejection with logging | Query fails, event silently dropped | **FAIL** (Expected bug confirmed) |
| JSON Serialization of `inf` value in backup | Valid RFC 8259 JSON output | Generates `"value": inf`, which is invalid JSON | **FAIL** (Expected bug confirmed) |

---

## Unchallenged Areas

- **C++ network socket data transmission**: The actual network transfer of invalid JSON was not tested against the real cloud function receiver since the test environment runs in `CODE_ONLY` mode (no external network access). However, the malformed JSON output was confirmed locally.
