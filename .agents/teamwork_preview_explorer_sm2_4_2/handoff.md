# Handoff Report: ChromeOS Extension & Security Feature Analysis (Sub-Milestone 2.4)

## 1. Observation
I directly observed the layout and contents of the following files:
*   `chromeos-extension/manifest.json` (lines 1-12):
    *   Uses Manifest V3 (`"manifest_version": 3`).
    *   Registers `background.js` as the background service worker (`"service_worker": "background.js"`).
*   `chromeos-extension/background.js` (lines 4-30):
    *   Uses a static `setInterval` of `10000` ms to query idle state and fetch active tabs.
    *   Sends telemetry via `fetch('http://localhost:8888/ingest', ...)` with body `{ window: activeTitle }`.
*   `edge-daemon/src/main.cpp` (lines 325-372, 427-647):
    *   Parses command-line parameters (including `--secret`).
    *   Hosts a HTTP listener on port 8888 with basic string parsing for routes `GET /status`, `POST /control`, and `POST /ingest`.
*   `edge-daemon/CMakeLists.txt` (lines 18-20):
    *   Builds the `edge-daemon` executable linking to `core_lib`, `PkgConfig::GLIB`, and `pthread`. It currently does not search for or link OpenSSL.

## 2. Logic Chain
1.  **F43 (Encrypted IPC)**: Because `background.js` runs inside a Manifest V3 Service Worker, standard Node cryptographic components are not available. However, the Web Crypto API (`crypto.subtle`) is accessible. Therefore, we can perform HMAC-SHA256 signing client-side. Since the daemon is built using CMake, linking OpenSSL's crypto library is standard on Linux and enables safe validation of signatures and timestamps. Constant-time string matching prevents timing side-channels.
2.  **F38 (Privacy Masking)**: Since the extension queries the tab title and URL, applying user-configured regex filters client-side avoids leaking restricted information over the loopback socket. Standard `std::regex` inside `main.cpp` provides a defense-in-depth safety net daemon-side.
3.  **F40 (Dynamic Interval)**: Because `setInterval` is fixed at initialization, replacing it with a recursive `setTimeout` schedule allows `background.js` to change its next execution delay based on `requested_interval_ms` received in the server's response.
4.  **F45 (Multi-Window Tracker)**: Since `chrome.windows.getAll({ populate: true })` returns all open Chrome windows and their active tabs, the extension can construct a structured payload identifying active tabs and tracking the focused window index.
5.  **F48 (Battery Power Saver)**: Since MV3 service workers block browser battery APIs, checking the battery `/sys/class/power_supply/` capacity natively in the daemon ensures high compatibility. The daemon then updates `g_config.ingestion_interval_ms`, which propagates to the client via `/ingest` responses.
6.  **F50 (Override Pause Hotkey)**: Because MV3 service workers are ephemeral and will terminate long-running JavaScript timers, timed telemetry pauses should be managed daemon-side (using lazy duration checks) and triggered by `chrome.commands` from the extension.

## 3. Caveats
*   The C++ daemon is currently assumed to be running in a Linux-compatible system (e.g. Crostini) where `/sys/class/power_supply/BAT0/capacity` (or `BAT1`) is available. If run in virtualized containers where sysfs is fully isolated from the host hardware state, battery monitoring may return `-1` (unsupported) and default to standard intervals.
*   The HMAC-SHA256 secret key needs to be configured in extension storage (e.g. via the Extension Admin policy or options panel) to match the daemon CLI parameter.

## 4. Conclusion
Integrating these six security and privacy features involves enhancing the client-side telemetry loop in `background.js` with Web Crypto signatures, recursive `setTimeout` schedules, and window enumeration. It concurrently requires linking OpenSSL in `CMakeLists.txt` and updating `main.cpp` to verify signatures, check sysfs battery capacity, perform lazy timed-pause evaluation, and output multi-window focus tracking metrics.

## 5. Verification Method
1.  **IPC Signature Verification**:
    *   Start daemon with `--secret TEST_SECRET`.
    *   Send requests from the extension (or via `curl` command mimicking the headers). Verify that unsigned or mismatched-signature requests return `401 Unauthorized` / `403 Forbidden`, while signed requests succeed.
2.  **Telemetry Cycle Test**:
    *   Verify the extension dynamically changes its loop timing when the daemon's responses change `requested_interval_ms`.
3.  **Battery Throttle Test**:
    *   Mock `/sys/class/power_supply/BAT0/capacity` with a value under `20` and status `Discharging`. Check that the daemon responds with `requested_interval_ms: 30000` and output displays throttling.
4.  **Unit Tests**:
    *   Add validation tests inside `tests/differential_privacy_test.cpp` verifying the performance and accuracy of `computeHmacSha256` and regex filtering.
