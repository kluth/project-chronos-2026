# Handoff Report — Sub-Milestone 2.4 Final Review

## 1. Observation

- **Implementation Code Files**:
  - `edge-daemon/src/differential_privacy.cpp`: Contains the core logic for differential privacy, SQLite buffering (`getBufferedEvents` line 397 containing `LIMIT 1000`), signature verification (`verifySignature` lines 860-940), and battery monitoring (`getBatteryLevel` lines 942-990).
  - `edge-daemon/src/main.cpp`: Contains the HTTP control logic (`/control` endpoint actions lines 552-658), DBus signal callbacks (`on_prepare_for_sleep` etc., lines 47-95), and the main entry point.
  - `edge-daemon/src/differential_privacy.h`: Defines headers and global variables (`g_dbus_paused` line 26).

- **Signature Replay Lock**:
  Checked that both the check and map insertion are done in a single block guarded by a single mutex lock:
  ```cpp
  {
      std::lock_guard<std::mutex> lock_sig(g_signatures_mutex);
      ...
      // Perform replay check
      if (g_processed_signatures.find(sig) != g_processed_signatures.end()) { ... }
      ...
      // Insert signature
      g_processed_signatures[sig] = request_time;
  }
  ```
  This is inside `edge-daemon/src/differential_privacy.cpp` lines 912-937.

- **Manual vs DBus Pause Isolation**:
  Observed that the DBus callbacks (`on_prepare_for_sleep`, `on_session_lock`, etc.) in `edge-daemon/src/main.cpp` write exclusively to `g_dbus_paused` (e.g., line 56: `g_dbus_paused = (active == TRUE);`).
  Observed that the manual pause/resume endpoint in `edge-daemon/src/main.cpp` writes exclusively to `g_tracking_paused` (lines 594, 597: `g_tracking_paused = true;` / `false;`).
  Evaluator `isTelemetryPaused()` in `edge-daemon/src/differential_privacy.cpp` evaluates `g_tracking_paused || g_dbus_paused` (line 993).

- **OOM Query Limit**:
  Observed that `getBufferedEvents` in `edge-daemon/src/differential_privacy.cpp` queries SQLite with a `LIMIT 1000` constraint:
  ```cpp
  const char* sql = "SELECT id, metric_name, value FROM telemetry_events ORDER BY id ASC LIMIT 1000;";
  ```

- **Battery Iterator Exception Safety and Filtration**:
  Observed that the loop iterating over `/sys/class/power_supply` is wrapped in `try-catch (...)` (lines 953-984).
  Observed that filenames not starting with "BAT" case-insensitively are skipped (lines 956-961):
  ```cpp
  if (filename.size() < 3 || 
      std::toupper(static_cast<unsigned char>(filename[0])) != 'B' ||
      std::toupper(static_cast<unsigned char>(filename[1])) != 'A' ||
      std::toupper(static_cast<unsigned char>(filename[2])) != 'T') {
      continue;
  }
  ```

- **Test Execution Results**:
  Executed `cmake .. && make && ./edge-daemon-tests` in `edge-daemon/build/`.
  Result:
  ```
  Running Differential Privacy Tests...
  [PASS] Laplace Noise Distribution Property Test (Epsilon = 0.5)
  [PASS] Telemetry Anonymization Test
  ...
  [PASS] Battery Power Saver Test
  [PASS] Signature Verification Test
  [PASS] Override Pause Test
  Running adversarial battery tests...
  [INFO] Lowest capacity returned correctly.
  [INFO] Peripheral battery filtered out correctly.
  [PASS] Adversarial Battery Tests completed.
  ...
  All tests passed successfully.
  ```

---

## 2. Logic Chain

1. **TOCTOU Replay Fix**:
   - Because signature replay check and map insertion are done within the exact same lock scope under `g_signatures_mutex` (Observation 1), concurrent duplicate signature requests cannot execute the check simultaneously before one of them inserts.
   - This ensures atomic verification and prevents replay attacks.

2. **Pause Isolation**:
   - Because DBus signals write only to `g_dbus_paused` and manual endpoints write only to `g_tracking_paused` (Observation 1), their states are completely isolated.
   - Toggling manual tracking does not interfere with sleep/lock state management, and vice versa.

3. **OOM Query Caps**:
   - Because `getBufferedEvents` limits results to `1000` (Observation 1), the memory consumed by telemetry queries is bounded, preventing OOM crashes.

4. **Battery Exception Safety and Filtration**:
   - Because the iterator loop is fully wrapped in a try-catch block (Observation 1), temporary filesystem changes during iteration will not crash the daemon.
   - Because of the case-insensitive "BAT" prefix check, wireless mouse and keyboard batteries are successfully excluded.

---

## 3. Caveats

- Live DBus signal handling could not be verified with actual operating system events due to sandbox constraints. Simulation testing was used instead.
- We assume all critical system batteries use the prefix "BAT" case-insensitively, which is standard for Linux power supplies.

---

## 4. Conclusion

The robustness, security, and concurrency fixes implemented for Sub-Milestone 2.4 are fully complete, correct, and robust. All test suites pass successfully. The verdict is **APPROVE**.

---

## 5. Verification Method

To independently run the tests:
1. Navigate to `/home/matthias/project/project-chronos/edge-daemon/build/`.
2. Run the build and test executable:
   ```bash
   cmake .. && make && ./edge-daemon-tests
   ```
3. Inspect `reviewer_sm2_4_final_2/review_report.md` and `reviewer_sm2_4_final_2/challenge_report.md` for detailed findings.
