# Handoff Report: Chrome Extension & Security Analysis (SM2.4)

## 1. Observation
- **Chrome Extension Ingestion Fetch Location**: `/chromeos-extension/background.js` lines 20-26:
  ```javascript
  20:         fetch('http://localhost:8888/ingest', {
  21:           method: 'POST',
  22:           headers: { 'Content-Type': 'application/json' },
  23:           body: JSON.stringify({ window: activeTitle })
  24:         }).catch(err => {
  25:           // Silently fail if the C++ daemon inside Crostini is offline
  26:         });
  ```
- **Hardcoded Interval**: `/chromeos-extension/background.js` line 30:
  ```javascript
  30: }, 10000); // 10 second ingestion cycle
  ```
- **CLI Secret Key Option**: `/edge-daemon/src/main.cpp` lines 359-365:
  ```cpp
  359:         } else if (arg == "--secret") {
  360:             if (i + 1 < argc) {
  361:                 g_config.secret = argv[++i];
  362:             } else {
  363:                 std::cerr << "Error: --secret requires a value" << std::endl;
  364:                 return 1;
  365:             }
  ```
- **Existing Build/Test Environment**: Evaluated by executing `ctest` in `edge-daemon/build/`, which completed successfully:
  ```
  1/1 Test #1: PropertyBasedDPTests .............   Passed    0.28 sec
  100% tests passed, 0 tests failed out of 1
  ```
- **Manifest Permissions**: `/chromeos-extension/manifest.json` line 6 shows:
  ```json
  "permissions": ["tabs", "idle"]
  ```

## 2. Logic Chain
1. **F43 (IPC Security)**: The extension currently communicates over HTTP loopback directly with zero validation. To enforce cryptographic verification under constraint F43, the extension must compute an HMAC-SHA256 signature using `crypto.subtle` APIs, sending it as `X-Chronos-Signature` alongside a timestamp `X-Chronos-Timestamp`. The C++ daemon needs to compile and verify this signature; since no standard crypto function exists in the local source file `differential_privacy.cpp`, it is logical to introduce OpenSSL `libcrypto` as a linked library via CMake.
2. **F38 (Privacy Masking)**: Since the extension has `"tabs"` permission, the URL and title are fully accessible. Adding a regex lookup helper inside `background.js` allows evaluating the title/domain against patterns. Using title redaction (to `[Redacted]`) ensures telemetry continues to report valid total durations while obscuring identifying details.
3. **F40 (Dynamic Cycle)**: To adjust the ingestion cycle length dynamically, `background.js` must replace its static `setInterval` loop with a recursive `setTimeout` call. When fetch responses return the new timing configuration, the extension updates its local state and updates the scheduling timer.
4. **F45 (Multi-Window Tracker)**: Using `chrome.windows.getAll({populate: true})` returns all open Chrome windows and their active tabs. Sorting by window ID provides a stable sequence index, allowing the extension to extract the active tab details and focus state. Using the key `"window"` in JSON for the focused tab title keeps the request schema backward-compatible with the daemon's existing simple substring searches.
5. **F48 (Power Saver)**: The edge daemon can natively track battery capacity and status via standard Linux `/sys/class/power_supply` capacity and status endpoints. If the capacity falls below 20% and the state is `"Discharging"`, it updates the dynamic interval calculation, feeding 30000ms back to the Chrome extension.
6. **F50 (Global Hotkey)**: Chrome commands in Manifest V3 (`chrome.commands`) are suitable for triggering actions using global hotkeys. Because service worker lifetimes are restricted (~30 seconds), the extension cannot track the 1-hour timeout reliably. The C++ daemon should manage the pause timer state via `std::chrono::steady_clock` to guarantee reliable automatic resumption.

## 3. Caveats
- **OpenSSL Availability**: We assume OpenSSL (`libcrypto`) development packages are present on standard Linux host platforms for linking during the build process.
- **Sysfs Battery Mapping**: We assume the directory `/sys/class/power_supply/` is exposed inside the Crostini virtual container environment to read battery metrics. If it is absent on specific hardware (e.g. desktop devices without batteries), the daemon will default gracefully to normal unthrottled operations.

## 4. Conclusion
The proposed designs in `analysis.md` address all six requirements. They improve telemetry security through HMAC signature verification, enhance user privacy via local regex redaction, optimize power state operations dynamically, and implement robust global controls by using native C++ timers for hotkey tracking.

## 5. Verification Method
1. **Compilation Check**:
   Configure CMake and run `make` inside `edge-daemon/build/` to verify that CMake identifies the OpenSSL package and links it correctly.
2. **Unit & Integration Tests**:
   Run `ctest` to ensure no existing functionality is broken.
3. **Daemon Loopback Log Inspection**:
   Run `edge-daemon --secret test_secret` and inspect stdout logs to confirm signature verifications, battery capacities, and window tracker indexes are reported correctly.
4. **Extension Logs**:
   Open Chrome Extension Service Worker Inspector to verify the dynamic transition of timeout values (e.g., from 10000ms to 30000ms when simulated low-battery response is returned).
