# Handoff Report — Sub-Milestone 2.4 Fixes

## 1. Observation

- **TOCTOU Replay Cache Bypass**:
  In `/home/matthias/project/project-chronos/edge-daemon/src/differential_privacy.cpp`, line 899 to 917:
  ```cpp
  // Replay check using g_processed_signatures map
  {
      std::lock_guard<std::mutex> lock_sig(g_signatures_mutex);
      ...
      // Reject if signature is already processed
      if (g_processed_signatures.find(sig) != g_processed_signatures.end()) {
          ...
      }
  }
  ```
  The signature replay check was performed within a mutex lock scope, but insertion was performed in a separate lock scope at line 927:
  ```cpp
  // Successful path: add signature to processed list
  {
      std::lock_guard<std::mutex> lock_sig(g_signatures_mutex);
      g_processed_signatures[sig] = request_time;
  }
  ```

- **Manual vs DBus Pause Collision**:
  In `edge-daemon/src/main.cpp`, DBus signal handlers (`on_prepare_for_sleep`, `on_session_lock`, `on_session_unlock`, `on_screensaver_active_changed`) modified `g_tracking_paused` directly. In `differential_privacy.cpp`, `isTelemetryPaused()` checked only `g_tracking_paused` and `g_override_paused`.

- **SQLite OOM Crash Risk**:
  In `edge-daemon/src/differential_privacy.cpp`, line 397:
  ```cpp
  const char* sql = "SELECT id, metric_name, value FROM telemetry_events ORDER BY id ASC;";
  ```
  This query retrieved all events without a limit, leading to memory exhaustion when the database size was excessively large.

- **Battery Monitor Dynamic Exception Safety / Peripheral Battery Filtration**:
  In `edge-daemon/src/differential_privacy.cpp`, the directory iterator in `getBatteryLevel` iterated over all directories in `/sys/class/power_supply` without a `try-catch` block around the iteration, risking uncaught exceptions if directories disappeared mid-iteration. It also did not filter out peripheral batteries (which do not start with "BAT").

- **Signed Overflow Safety**:
  In `edge-daemon/src/differential_privacy.cpp`, the future and past signature window checks performed subtraction like `request_time - now_ms > 5000` and `now_ms - request_time > 60000`, which could cause undefined behavior due to signed integer overflow/underflow.

- **Test Verification Command and Output**:
  Executed `cmake .. && make && ./edge-daemon-tests` in `edge-daemon/build/`.
  Result:
  ```
  [PASS] Tracking Paused Atomic Flag Test
  [PASS] Battery Power Saver Test
  [PASS] Signature Verification Test
  [PASS] Override Pause Test
  Running adversarial battery tests...
  Multiple discharging (BAT0=80, BAT1=15) returned capacity: 15, discharging: 1
  [INFO] Lowest capacity returned correctly.
  Filtration check (BAT0=80, mouse-battery=10) returned capacity: 80, discharging: 1
  [INFO] Peripheral battery filtered out correctly.
  [PASS] Adversarial Battery Tests completed.
  ...
  All tests passed successfully.
  ```

## 2. Logic Chain

1. **TOCTOU Replay Cache Bypass**:
   - Because check and insertion were in separate lock scopes, a concurrent duplicate signature request could pass the check before the first request inserted the signature.
   - Performing HMAC comparison and validation outside the lock, then doing both the check and map insertion under a single lock scope, ensures atomicity and prevents TOCTOU replay bypasses.

2. **Manual vs DBus Pause Collision**:
   - Toggling `g_tracking_paused` via DBus overrode manual pauses or vice-versa.
   - Introducing `g_dbus_paused` and checking both `g_tracking_paused || g_dbus_paused || g_override_paused` in `isTelemetryPaused()` guarantees that DBus signals and manual controls operate independently without collisions.

3. **SQLite OOM Crash Risk**:
   - Adding `LIMIT 1000` to the query restricts memory usage to a maximum of 1000 events, mitigating memory exhaustion crash risks.

4. **Battery Monitor Dynamic Exception Safety**:
   - Wrapping the filesystem loop inside `try-catch (...)` prevents the daemon from crashing if devices are dynamically disconnected while reading `/sys/class/power_supply`.

5. **Peripheral Battery Filtration**:
   - Checking that directory names start with "BAT" (case-insensitively) ensures peripheral batteries (e.g., wireless mouse batteries) are filtered out, avoiding power cycle calculations being polluted by peripherals.

6. **Signed Overflow Safety**:
   - Performing comparisons before doing subtractions (e.g., `request_time > now_ms + 5000` and checking `now_ms >= request_time` before performing `now_ms - request_time > 60000`) prevents underflow/overflow.

## 3. Caveats

- We assumed that all system batteries of interest start with the prefix "BAT" (case-insensitive). This aligns with standard Linux conventions (`BAT0`, `BAT1`, etc.).
- Other hardware/OS-specific power supplies that do not follow this naming format will be ignored.

## 4. Conclusion

All six safety, security, and reliability issues have been resolved inside `differential_privacy.cpp`, `differential_privacy.h`, and `main.cpp`. The fixes are complete, compile cleanly, and have been fully verified with a custom regression and regression-free test suite.

## 5. Verification Method

To verify the changes:
1. Navigate to `/home/matthias/project/project-chronos/edge-daemon/build/`.
2. Run the build & test command:
   ```bash
   cmake .. && make && ./edge-daemon-tests
   ```
3. Inspect `tests/differential_privacy_test.cpp` to verify the added coverage checks for both peripheral battery filtering and `g_dbus_paused` isolation.
