## 2026-06-16T12:35:08Z
You are the Worker for GDPR Compliance metrics update (Milestone 2).
Your working directory is `/home/matthias/project/project-chronos/.agents/worker_gdpr_metrics/`.

Your task is to replace the old CPU/RAM Performance Telemetry (F44) and Multi-Window Focus Tracker (F45) with GDPR-compliant aggregate metrics:
1. **chromeos-extension (Keystroke & Mouse tracking)**:
   - Create `chromeos-extension/content.js`. In this script, add event listeners for:
     a. Keyboard `keydown` events: increment a keystroke counter (only counting, no key identities).
     b. Mouse `mousemove` events: calculate the Euclidean distance (pixels traveled) between successive mouse positions, and add to a mouse distance counter (no absolute coordinates).
     c. Periodically (e.g., every 5 seconds), send the accumulated counters to `background.js` via `chrome.runtime.sendMessage()` and reset the local counters.
   - Update `chromeos-extension/manifest.json` to register `content.js` under `content_scripts` matching `<all_urls>` with `all_frames: true`.
   - Update `chromeos-extension/background.js`:
     - Receive key/mouse messages from `content.js` and aggregate them.
     - Maintain a 60-second sliding window of these metrics to compute the per-minute totals (`keystrokes_per_minute` and `mouse_pixels_per_minute`).
     - In `performIngestionCycle`, send `keystrokes_per_minute` and `mouse_pixels_per_minute` in the JSON payload of the `POST /ingest` request.
     - Remove the old multi-window desktop focus tracking code (F45) from `background.js`.

2. **edge-daemon (C++ ingestion & storage)**:
   - In `edge-daemon/src/main.cpp`:
     - In the HTTP loop, parse `keystrokes_per_minute` and `mouse_pixels_per_minute` from the request JSON.
     - If present, process them as separate `TelemetryEvent`s, anonymize them with Laplace noise, and buffer/upload them.
     - Remove the CPU/RAM performance monitoring (F44) from `backgroundTelemetryThread` and any other places in `main.cpp`.
   - In `edge-daemon/src/differential_privacy.cpp` and `edge-daemon/src/differential_privacy.h`:
     - In `obfuscateDomainOrCategory`, ensure that `"keystrokes_per_minute"` and `"mouse_pixels_per_minute"` are passed through unchanged.
     - Remove the old functions `readCpuStats`, `calculateCpuUtilization`, and `getRamUtilization`.
   - In `edge-daemon/tests/differential_privacy_test.cpp`:
     - Replace `testResourcePerformanceTelemetry()` with a new test `testGDPRTelemetry()` that verifies the new keystroke/mouse metrics are parsed, passed through the obfuscator, anonymized, and stored correctly.

3. **Verify Build and Tests**:
   - Re-build the daemon, applet, and tests.
   - Run `./build/edge-daemon-tests` and verify all tests pass.
   - Commit the changes incrementally via Git.
   - Write a detailed handoff.md in your working directory and message the parent with your results.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A Forensic Auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.
