## 2026-06-16T11:12:27Z
You are the Worker for Sub-Milestone 2.1 (Local Buffering, Network Check, Obfuscation, and Calibration).
Your working directory is `/home/matthias/project/project-chronos/.agents/worker_sm2_1/`.

Task Details:
1. Verify if `libsqlite3-dev` is installed. If not, install it using `sudo apt-get update && sudo apt-get install -y libsqlite3-dev`. If that fails, see if you can run the daemon build or install it another way.
2. Implement the following features in `edge-daemon/src/main.cpp`, `edge-daemon/src/differential_privacy.cpp`, and `edge-daemon/src/differential_privacy.h`:
   - **F49: Laplace Calibration Utility**: Add command-line parsing for `--epsilon`, `--sensitivity`, `--budget`, `--secret`, and `--calibrate`. If `--calibrate` is provided, print DP noise bounds (variance, scale, confidence intervals) and exit. Save values in global/main configuration.
   - **F41: Crostini Network State Checker**: Implement a robust two-stage network checker in C++: first, use `getifaddrs` to check for active, non-loopback network interfaces. Second, perform a fast non-blocking TCP socket connection check (e.g. to IP `8.8.8.8` on port `53` with a 500ms timeout) to verify internet routing.
   - **F46: Local Domain Obfuscation Mapping**: Clean incoming titles/URLs to base domains or category names before applying noise. Implement URL suffix/prefix parsing and fallback categories (e.g., `vscode`, `terminal`, `web-browser`, `generic-activity`).
   - **F35: Local SQLite Offline Buffering**: Set up a local SQLite3 database to buffer anonymized telemetry events when offline. On ingestion, if offline or upload fails, store the noisy event in the SQLite DB. On reconnection (when a new request comes in and network is online), upload buffered events in bulk (FIFO) and delete them from the DB.
3. Safe Shell execution: To prevent shell injection vulnerabilities, replace the unsafe `popen(curl_cmd)` shell call in `exec` with a safe process execution using `fork()` and `execvp()` with an argument array, or link `libcurl` if available.
4. Update `CMakeLists.txt` to find and link `SQLite3` (using `find_package(SQLite3 REQUIRED)`).
5. Build and verify the daemon and tests. Run the existing tests. Add tests in `tests/differential_privacy_test.cpp` or a new test file to verify features 35, 41, 46, and 49.
6. Commit your changes incrementally using git as features are completed.
7. Write your handoff report to your working directory as handoff.md detailing what you implemented, the compilation and test run outputs, and git commit details. Message the parent with the path to your report.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A Forensic Auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.
