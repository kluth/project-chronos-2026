# Handoff Report - Sub-Milestone 2.2 (Privacy Budget & Native Resource/Process Scanning)

## 1. Observation
- **Codebase compilation status**: Confirmed using `make` at `/home/matthias/project/project-chronos/edge-daemon/` which compiled targets `core_lib`, `edge-daemon`, and `edge-daemon-tests`.
- **Test status**: Executed `./edge-daemon-tests` showing all existing tests pass:
  ```
  Running Differential Privacy Tests...
  [PASS] Laplace Noise Distribution Property Test (Epsilon = 0.5)
  [PASS] Telemetry Anonymization Test
  [PASS] Local Domain Obfuscation Mapping Test
  [PASS] Local SQLite Offline Buffering Test
  Network interface check result: Active interfaces found
  [PASS] Crostini Network State Checker Test (verification complete)
  All tests passed successfully.
  ```
- **SQLite Database initialization**: Located at `edge-daemon/src/differential_privacy.cpp:288` where `initDatabase` initializes table `telemetry_events`.
- **Privacy Parameter Parsing**: Parsed from command-line arguments in `edge-daemon/src/main.cpp:44` to populate global variable `g_config.budget` (type `double`).
- **Anonymization entry point**: Located at `edge-daemon/src/differential_privacy.cpp:48` where `anonymizeEvent` takes raw events and applies Laplace noise.

## 2. Logic Chain
- **Serialization of database access**: Given that the main socket server thread receives events (loop starting at `edge-daemon/src/main.cpp:112`) and the new telemetry scanning operates on a periodic basis, a multi-threaded architecture is chosen. To prevent concurrent write conflicts (`SQLITE_BUSY`) or deadlocks from recursive functions, a `std::recursive_mutex` (`g_db_mutex`) is proposed to protect all SQL statement prepares and executions.
- **Privacy Budget Adjustment (F39)**: Under Laplace noise mechanism, scale is $b = sensitivity / epsilon$. Reducing $\epsilon$ increases Laplace noise. By querying `privacy_budget_log` table for cumulative epsilon over the last 24h (86,400s), we check if the limit is exceeded (then pause ingestion) or approached (>80% limit, then scale down epsilon linearly, enforcing a 10% lower floor to prevent division-by-zero or infinite noise).
- **Process Scanner `/proc` Monitor (F37)**: Under POSIX systems, `/proc` contains numerical directories representing active processes. Reading `/proc/<pid>/comm` exposes process names (e.g. `code` for VSCode, `bash` or `zsh` for terminals, `gcc` for compilers). Matching these yields active native state, which can be logged to SQLite as telemetry events and anonymized.
- **Resource Performance Telemetry (F44)**: `/proc/stat` tracks CPU ticks since boot. Reading the first line `cpu ` and computing ticks delta between periods (using static variables for previous state) gives current active CPU utilization. `/proc/meminfo` tracks memory statistics, where RAM utilization is computed as `100.0 * (MemTotal - MemAvailable) / MemTotal` (with fallback to `MemFree` if `MemAvailable` is absent).
- **Automated Snapshots (F47)**: Backup path `~/Downloads/chronos_backups/` is expanded via `getenv("HOME")` and created using C++17 `std::filesystem::create_directories`. Querying `telemetry_events` and `privacy_budget_log` and serializing results manually to JSON text avoids adding heavy third-party parsing libraries to `CMakeLists.txt`.

## 3. Caveats
- `/proc/meminfo` falls back to `MemFree` if `MemAvailable` is absent, which may slightly overestimate memory usage on older kernels (pre-3.14).
- The process scanner targets common shells and editor names (`code`, `bash`, etc.) which might not catch highly customized tool names or wrappers.
- Database path is hardcoded as `telemetry_buffer.db` consistent with the existing codebase setup.

## 4. Conclusion
We have formulated a complete, thread-safe C++17 implementation plan for the four features (F37, F39, F44, F47) that utilizes a detached background worker thread in `main.cpp` and serialized SQLite locks in `differential_privacy.cpp`. The complete design, integration details, and verification unit tests are documented in `analysis.md`.

## 5. Verification Method
- **Compilation check**: Compile the enhanced codebase using:
  ```bash
  cmake -B build
  cmake --build build
  ```
- **Test execution**: Run the test suite:
  ```bash
  ./build/edge-daemon-tests
  ```
  Ensure all new tests (`testPrivacyBudgetTracker`, `testProcessScanner`, `testResourcePerformanceTelemetry`, `testBackupDatabaseToJson`) output `[PASS]`.
- **Directory Inspection**: Check that snapshots are successfully created under the user directory:
  ```bash
  ls -l ~/Downloads/chronos_backups/
  cat ~/Downloads/chronos_backups/chronos_backup_*.json
  ```
