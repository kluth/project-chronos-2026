# Handoff Report — Reviewer Sub-Milestone 2.4

## 1. Observation
- **Test Build Command & Execution**:
  - Ran `make && ./edge-daemon-tests` in `/home/matthias/project/project-chronos/edge-daemon/build`.
  - **Tool command output**:
    ```
    Running Differential Privacy Tests...
    [PASS] Laplace Noise Distribution Property Test (Epsilon = 0.5)
    [PASS] Telemetry Anonymization Test
    [PASS] Local Domain Obfuscation Mapping Test
    [PASS] Local SQLite Offline Buffering Test
    Network interface check result: Active interfaces found
    [PASS] Crostini Network State Checker Test (verification complete)
    [PASS] Local Privacy Budget Tracker Test
    Process scanner run complete. Found 1 active target tools.
      - bash
    [PASS] Native Process Scanner /proc Monitor Test
    CPU stats reading: Success
    RAM stats reading: Success
    [PASS] Device Resource Performance Telemetry Test
    [Backup] SQLite tables backed up to /home/matthias/Downloads/chronos_backups/chronos_backup_20260616_134348.json
    [PASS] Automated Shared Folder Snapshots Test
    [PASS] Tracking Paused Atomic Flag Test
    [PASS] Battery Power Saver Test
    [Security IPC] Verification failed: missing signature or timestamp.
    [Security IPC] Verification failed: replay attack window exceeded.
    [Security IPC] Verification failed: replay attack window exceeded.
    [Security IPC] Verification failed: signature mismatch.
    [PASS] Signature Verification Test
    [Daemon] Override pause has expired. Resuming tracking.
    [PASS] Override Pause Test
    All tests passed successfully.
    ```
- **Code Inspection - Concurrency**:
  - `edge-daemon/src/differential_privacy.cpp` (lines 42-43): `std::atomic<bool> g_override_paused(false);` and `std::chrono::steady_clock::time_point g_override_paused_until;` are defined globally.
  - `edge-daemon/src/differential_privacy.cpp` (lines 879-887):
    ```cpp
    if (g_override_paused) {
        if (std::chrono::steady_clock::now() >= g_override_paused_until) {
            g_override_paused = false;
            std::cout << "[Daemon] Override pause has expired. Resuming tracking." << std::endl;
            return false;
        }
        return true;
    }
    ```
    This reads `g_override_paused_until` in the background thread (called by `backgroundTelemetryThread` via `isTelemetryPaused()`).
  - `edge-daemon/src/main.cpp` (lines 568-573):
    ```cpp
    } else if (action == "pause_override") {
        double duration = 3600.0;
        extractDouble(body, "duration", duration);
        g_override_paused = true;
        g_override_paused_until = std::chrono::steady_clock::now() + std::chrono::seconds(static_cast<int>(duration));
        success = true;
    ```
    This writes to `g_override_paused_until` in the main thread without synchronization.
- **Code Inspection - Configuration Data Race**:
  - `edge-daemon/src/main.cpp` (lines 574-587): writes to `g_config.epsilon`, `g_config.budget`, and `g_config.sensitivity` inside the main thread without synchronization.
  - `edge-daemon/src/main.cpp` (lines 220-222, 260-264, 292-295, 669-671): reads from `g_config` properties in the background thread.
- **Code Inspection - Filesystem Directory Iterator**:
  - `edge-daemon/src/differential_privacy.cpp` (lines 853-855):
    ```cpp
    int min_capacity = -1;
    for (const auto& entry : std::filesystem::directory_iterator(base_dir)) {
    ```
    This does not catch exceptions that can be thrown by `directory_iterator`.
- **Code Inspection - Socket Parsing**:
  - `edge-daemon/src/main.cpp` (lines 610-621):
    ```cpp
    size_t body_pos = request.find("\r\n\r\n");
    std::string body;
    if (body_pos != std::string::npos) {
        body = request.substr(body_pos + 4);
    ...
    ```
    This parses the request body directly without truncating it to the specified `content_length`.
- **Code Inspection - JSON Extracting**:
  - `edge-daemon/src/main.cpp` (lines 653-658):
    ```cpp
    size_t pos = request.find("\"window\":\"");
    if (pos != std::string::npos) {
        size_t end_pos = request.find("\"", pos + 10);
        if (end_pos != std::string::npos) {
            active_window = request.substr(pos + 10, end_pos - (pos + 10));
        }
    }
    ```
    This extracts the window string assuming it does not contain escaped quotes.

## 2. Logic Chain
- **Data Races lead to Undefined Behavior**:
  - `g_override_paused_until` is written by the main thread during `/control` POST processing and read by the background thread during periodic ingestion cycles. Since `steady_clock::time_point` is not atomic and lacks mutex protection, this is a concurrent read-write data race.
  - Similarly, `g_config.epsilon`, `g_config.budget`, and `g_config.sensitivity` are modified by the main thread and read by the background thread concurrently, creating a data race on `double` values. Under standard C++ semantics, these data races produce undefined behavior (UB), which could manifest as torn reads, compiler-reordering bugs, or crashes.
- **FS Exceptions lead to Crash**:
  - If `/sys/class/power_supply` is not accessible, is deleted, or throws due to permissions (e.g. in hardened virtualization containers), `std::filesystem::directory_iterator` raises `std::filesystem_error`. The absence of `try-catch` blocks or an `error_code` parameter means the daemon will crash.
- **Socket Body Parsing Leaks**:
  - Extracting the body via `request.substr(body_pos + 4)` captures any trailing characters read from the socket beyond the designated JSON payload. Since `verifySignature` includes `body` in the HMAC calculation, trailing garbage causes verification to fail.
- **Escaped Quote JSON Parsing Gap**:
  - Manual substring extraction of `window` fails when titles include nested escaped quotes (`\"`). Finding the first quote after `pos + 10` prematurely cuts off the string (e.g., `Chronos \"Developer\" Tools` resolves to `Chronos \`), resulting in corrupted telemetry metric names.

## 3. Caveats
- Verified build and tests in a standard Linux environment. If running on specialized ChromeOS VM containers where `/sys/class/power_supply` has non-standard permissions, the exception risk is elevated.
- Assumed standard Chrome extension messaging environment where `fetch` sends body payloads close to `content_length`, but TCP socket behaviors can cause fragmented packets where trailing data could appear.

## 4. Conclusion
- **Verdict**: **REQUEST_CHANGES** (due to concurrency data races, crash risks, and parsing bugs).
- **Quality Review Verdict**: REQUEST_CHANGES
  - *Major Finding 1 (Concurrency)*: Unsynchronized thread access to `g_override_paused_until`.
  - *Major Finding 2 (Concurrency)*: Unsynchronized thread access to `g_config` configurations.
  - *Major Finding 3 (Robustness)*: Uncaught exception risk in directory iteration.
  - *Minor Finding 4 (Robustness)*: Vulnerable HTTP request body extraction (lack of truncation to `content_length`).
  - *Minor Finding 5 (Robustness)*: Escaped quote truncation in custom JSON parsing.
  - *Minor Finding 6 (Logic)*: Battery Saver returns first discharging battery rather than minimum capacity.
- **Adversarial Review Summary**:
  - *Overall Risk*: **HIGH** (Concurrency UB and crash risks are dangerous in low-level system daemons).
  - *Mitigations proposed*: Use `std::atomic` or `std::mutex` for shared configs/timers, utilize `std::error_code` for filesystem iteration, truncate body using `content_length`, and loop over all batteries to determine the lowest capacity.

## 5. Verification Method
- **Run the C++ daemon tests**:
  ```bash
  cd /home/matthias/project/project-chronos/edge-daemon/build
  make && ./edge-daemon-tests
  ```
- **Inspect Files**:
  - `edge-daemon/src/main.cpp`
  - `edge-daemon/src/differential_privacy.cpp`
