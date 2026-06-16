# Handoff Report - Edge Daemon Enhancements (SM2.1)

## 1. Observation
*   **Missing Development Headers for SQLite3**: Running `ls -la /usr/include/sqlite3.h` returned `ls: cannot access '/usr/include/sqlite3.h': No such file or directory`. Running `dpkg -l | grep libsqlite3-dev` returned no packages. The system has `libsqlite3-0` runtime (`3.46.1-7+deb13u1` for arm64) but lacks compiling dependencies.
*   **Linker Setup**: `edge-daemon/CMakeLists.txt` lines 11-12:
    ```cmake
    add_executable(edge-daemon src/main.cpp)
    target_link_libraries(edge-daemon core_lib)
    ```
*   **Synchronous Curl Posting**: `edge-daemon/src/main.cpp` lines 85-86:
    ```cpp
    std::string curl_cmd = "curl -s -X POST -H \"Content-Type: application/json\" -d '{\"metric\":\"" + safe_event.metric_name + "\",\"val\":" + std::to_string(safe_event.value) + "}' https://europe-west3-project-chronos-2026.cloudfunctions.net/ingestTelemetry > /dev/null";
    exec(curl_cmd.c_str());
    ```
*   **Baseline Tests**: Running `./edge-daemon-tests` executes and passes:
    ```
    Running Differential Privacy Tests...
    [PASS] Laplace Noise Distribution Property Test (Epsilon = 0.5)
    [PASS] Telemetry Anonymization Test
    All tests passed successfully.
    ```

## 2. Logic Chain
1.  **SQLite3 Linking Requirement**: Adding `#include <sqlite3.h>` directly into `main.cpp` or a sub-module will fail to compile immediately due to the absence of `sqlite3.h` in standard paths (confirmed by **Observation 1**). Therefore, we must either install `libsqlite3-dev` or vendor `sqlite3.c`/`sqlite3.h` directly (Option B in analysis).
2.  **Decoupling buffering from main loop**: The main socket loop in `main.cpp` should not block. Spawning `curl` synchronously (confirmed by **Observation 3**) blocks the daemon's main thread, especially when offline or when timeouts occur. Thus, we must write telemetry to the SQLite DB on receipt, and have a background thread drain the database to Firebase asynchronously when connectivity is confirmed.
3.  **VM Network Bridge Isolation**: The Crostini container network interface `eth0` stays UP and RUNNING even if the ChromeOS host loses external connectivity. Simply checking `getifaddrs` is insufficient. Thus, we require a non-blocking TCP socket check to an external DNS IP (e.g. `8.8.8.8`) on port 53 to confirm internet route.
4.  **Zero-Leakage Privacy Policy**: Window titles can contain query parameters or doc names. Applying DP noise to raw titles is not private because the string itself leaks. We must obfuscate URLs to base domains and map window titles to generic platforms/categories using a suffix rules engine, falling back to a generic `unclassified_activity` tag before applying Laplace noise.

## 3. Caveats
-   **Security of Seed**: Seeding the Laplace noise with a secret generates deterministic noise. If the secret is compromised, an attacker can subtract the noise to find the exact telemetry values. Seeding must only be used in CLI calibration/debugging or in specific deterministic DP applications.
-   **Public Suffix List**: The domain parser uses a rule-based heuristic to check common multi-part suffixes (like `.co.uk`, `.com.br`). It does not use the full standard Public Suffix List, which would increase code size and overhead.

## 4. Conclusion
We have produced a detailed, executable-ready design specification in `analysis.md` for implementing:
1.  `SQLiteBuffer` class with WAL mode enabled to thread-safely insert anonymized telemetry events.
2.  An asynchronous database-draining queue thread.
3.  A two-tiered network check (local interface check + 500ms non-blocking TCP socket connect check).
4.  A `DomainObfuscator` utility that handles both URL host parsing and window title rule-matching.
5.  A command line `calibrate-laplace` utility implementing `getopt_long` and deterministic seeded PRNG noise output.

## 5. Verification Method
-   **Compile verification**: Check if CMake configuration is correct. Once dependencies are added, the project compiles successfully with `make` or `cmake --build .`.
-   **Testing verification**: The current unit tests (`./edge-daemon-tests`) can be expanded to test domain obfuscation mappings and the SQLite buffer writing/draining capabilities.
