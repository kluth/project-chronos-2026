## 2026-06-16T12:13:39Z

Implement the following fixes in the C++ Daemon:

1. TOCTOU Replay Cache Bypass:
- In `differential_privacy.cpp`, inside `verifySignature`, remove the separate check and insertion lock scopes. Instead, perform all verification checks (parsing, timestamp validation, HMAC computation, and constant-time comparison) outside the lock scope. Once the signature is verified, acquire the lock `std::lock_guard<std::mutex> lock_sig(g_signatures_mutex)` and perform both the replay check (find in map) and map insertion in that single lock scope.

2. Manual vs DBus Pause Collision:
- Introduce a new global atomic variable `std::atomic<bool> g_dbus_paused(false);` in `differential_privacy.cpp` (declare it extern in `differential_privacy.h`).
- In `main.cpp`, modify DBus signal handlers (`on_prepare_for_sleep`, `on_session_lock`, `on_session_unlock`, `on_screensaver`) to update `g_dbus_paused` instead of `g_tracking_paused`.
- In `isTelemetryPaused()`, check both flags: return true if `g_tracking_paused || g_dbus_paused || g_override_paused`.

3. SQLite OOM Crash Risk:
- In `getBufferedEvents` (in `differential_privacy.cpp`), add a `LIMIT 1000` clause to the SQL query:
  `SELECT id, metric_name, value FROM telemetry_events ORDER BY id ASC LIMIT 1000;`

4. Battery Monitor Dynamic Exception Safety:
- In `getBatteryLevel` (in `differential_privacy.cpp`), wrap the range-based for loop over `std::filesystem::directory_iterator` inside a `try-catch (...)` block to prevent unhandled filesystem exception crashes if a battery device is dynamically removed/disconnected mid-iteration.

5. Peripheral Battery Filtration:
- In `getBatteryLevel`, check the filename of each directory entry. Ignore the entry if its filename does not start with "BAT" (case-insensitive) to prevent wireless mice or peripheral batteries from polluting system battery power saver cycles.

6. Signed Overflow Safety:
- In `verifySignature`, make the timestamp subtraction logic overflow-safe. Compare `request_time` and `now_ms` using logic like:
  ```cpp
  if (request_time > now_ms + 5000) {
      std::cerr << "[Security IPC] Verification failed: future timestamp limit exceeded." << std::endl;
      return false;
  }
  if (now_ms >= request_time) {
      if (now_ms - request_time > 60000) {
          std::cerr << "[Security IPC] Verification failed: replay attack window exceeded." << std::endl;
          return false;
      }
  }
  ```
  This prevents signed integer subtraction undefined behavior.

Verification:
- Compile and run daemon tests in `edge-daemon/build/`: `cmake .. && make && ./edge-daemon-tests`
- Confirm all tests pass.
