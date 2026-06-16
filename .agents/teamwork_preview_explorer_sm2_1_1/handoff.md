# Handoff Report — Explorer 1 (Sub-Milestone 2.1)

## 1. Observation
We examined the files and runtime dependencies in `/home/matthias/project/project-chronos/edge-daemon`.

*   **Observation 1 (Missing Headers):** A system-wide search for `sqlite3.h` returned no results in standard system directories. The only match is in Dart's pub cache:
    `find / -name sqlite3.h 2>/dev/null` -> `/home/matthias/.pub-cache/hosted/pub.dev/sqlite3-2.9.4/assets/sqlite3.h`
    We confirmed `libsqlite3.so.0.8.6` is located at `/usr/lib/aarch64-linux-gnu/libsqlite3.so.0.8.6`, but no developer headers exist in `/usr/include/`.
*   **Observation 2 (Shell Command Execution):** `edge-daemon/src/main.cpp:85-86` executes a curl command via the shell:
    ```cpp
    std::string curl_cmd = "curl -s -X POST -H \"Content-Type: application/json\" -d '{\"metric\":\"" + safe_event.metric_name + "\",\"val\":" + std::to_string(safe_event.value) + "}' https://europe-west3-project-chronos-2026.cloudfunctions.net/ingestTelemetry > /dev/null";
    exec(curl_cmd.c_str());
    ```
    This function `exec` is defined at line 14:
    ```cpp
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    ```
*   **Observation 3 (Differential Privacy Core):** The DP implementation is defined in `edge-daemon/src/differential_privacy.cpp`, where `anonymizeEvent` calls `generateLaplaceNoise(1.0, epsilon)` to add noise:
    ```cpp
    double noise = generateLaplaceNoise(1.0, epsilon);
    anonymized.value += noise;
    ```
*   **Observation 4 (CMake File):** `edge-daemon/CMakeLists.txt` does not reference any external SQLite3 or Curl package linking logic.

---

## 2. Logic Chain
1.  **SQLite3 Linker Dependency:** Since `sqlite3.h` is missing from system header directories (Observation 1), using a standard CMake `find_package(SQLite3 REQUIRED)` package finder will result in configuration errors at build time. To address this without system configuration privileges, the SQLite3 amalgamation (`sqlite3.c`/`sqlite3.h`) must be vendored statically inside the repository, or the build host must install `libsqlite3-dev` explicitly.
2.  **Telemetry Buffering and Offline Flow:** The active daemon parses requests synchronously. Spawning `popen(curl)` processes blocks execution. If the daemon is offline, curl runs will fail, causing latency or data loss. By adding a local SQLite3 table (`telemetry_buffer`), we can write telemetry safely to the database and use a background thread to asynchronously read, transmit, and delete entries, keeping the main extension connection handler thread non-blocking.
3.  **Network Checking:** Instead of triggering expensive shell processes to run `curl` only to discover the system is offline, we can check the host network state in C++ using `getifaddrs()` to detect active network interfaces, combined with a quick UDP non-blocking socket connect to `8.8.8.8:53` to verify routing table availability.
4.  **Domain Obfuscation:** The extension transmits the window title in the `window` field. This title may contain private URLs and detailed paths (Observation 2). To obfuscate, we can extract the base domain (e.g. `domain.com`) or map application names from the title, fallback to cryptographic hashing of unknown domains, and send only the safe, obfuscated metric to the backend.
5.  **Security Vulnerability:** In `main.cpp`, injecting a quote and shell operator in `safe_event.metric_name` (parsed directly from the incoming web request) allows execution of arbitrary shell command payloads via `popen()` (Observation 2). Resolving this requires migrating shell execution to direct C++ `libcurl` linking.

---

## 3. Caveats
*   **Internet Access checks:** A local UDP socket connectivity check to `8.8.8.8` verifies IP routing, but does not guarantee DNS resolution or that Firebase itself is reachable. Proper HTTP library checks must handle request timeouts and status codes (e.g. 503, 404) as fallback triggers for database buffering.
*   **Thread Safety:** SQLite3 connection handle usage inside a multithreaded daemon assumes `SQLITE_THREADSAFE=1` is active in the library. If not, mutexes or a single-threaded database access pattern must be enforced.

---

## 4. Conclusion
We recommend the following architectural implementations:
1.  **SQLite3:** Vendor the SQLite3 amalgamation (`sqlite3.c` & `sqlite3.h`) into `third_party/sqlite3/`, configure it as a static library target in `CMakeLists.txt`, and wrap database operations using custom C++ RAII smart pointers.
2.  **Crostini Network State Checker:** Implement a dual-level C++ checker using POSIX `getifaddrs` for interface availability and a non-blocking UDP socket route check.
3.  **Domain Obfuscation:** Clean and parse raw window titles or URLs to extract the base domain (incorporating ccSLD heuristics), and use `HMAC-SHA256` hashing for unclassifiable private paths.
4.  **Laplace Calibration Utility:** Create a standalone CLI binary `laplace-calibrate` to compute scales ($b = \Delta f / \epsilon$), variance ($\sigma^2 = 2 b^2$), and query capacity under budget restrictions, using `--secret` to securely seed the noise generator.
5.  **Security & Dependency Hardening:** Replace shell-based `popen(curl)` with native C++ `libcurl` linking in `CMakeLists.txt` via `find_package(CURL REQUIRED)` to eliminate shell injection vulnerabilities.

---

## 5. Verification Method
To verify the implementation of the recommended architecture:
1.  **Build System Verification:** Run CMake to compile edge-daemon. Compilation should succeed without requiring host `libsqlite3-dev` library setup when using the vendored amalgamation option.
    ```bash
    cmake -S . -B build
    cmake --build build
    ```
2.  **Interface Checker:** Run the test executable inside the Crostini container with the virtual interface disabled (e.g. `ip link set dev eth0 down`) and verify it reports offline and triggers database buffering.
3.  **Shell Injection Verification:** Inject a telemetry ping containing `test' ; rm -rf / #` and verify it is safely parsed as a literal string parameter without executing shell code.
