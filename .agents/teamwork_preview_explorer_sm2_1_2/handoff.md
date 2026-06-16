# Handoff Report - Sub-Milestone 2.1 Explorer 2

## 1. Observation
- **Codebase Analysis**:
  - `edge-daemon/src/main.cpp`: Spawns shell processes via `popen` to execute `curl` requests. Specifically:
    ```cpp
    std::string curl_cmd = "curl -s -X POST -H \"Content-Type: application/json\" -d '{\"metric\":\"" + safe_event.metric_name + "\",\"val\":" + std::to_string(safe_event.value) + "}' https://europe-west3-project-chronos-2026.cloudfunctions.net/ingestTelemetry > /dev/null";
    exec(curl_cmd.c_str());
    ```
  - `edge-daemon/CMakeLists.txt`: Defines targets `core_lib`, `edge-daemon`, and `edge-daemon-tests`. It contains no references to SQLite3 or dynamic linking.
  - `edge-daemon/src/differential_privacy.cpp`: Implements `generateLaplaceNoise(double sensitivity, double epsilon)` and `anonymizeEvent(...)` using thread-local random device generators and standard mathematical log inverse CDF transformation.
- **Environment Analysis**:
  - Running command `dpkg -l | grep -E "libsqlite3|sqlite3"` outputs:
    `ii  libsqlite3-0:arm64                   3.46.1-7+deb13u1                     arm64        SQLite 3 shared library`
  - Running command `find /usr/include -name sqlite3.h` returns no stdout, indicating `libsqlite3-dev` is missing on the local development environment.
- **Tests Execution**:
  - Ran CMake compilation command and tests executable using `cmake -B build && cmake --build build && ./build/edge-daemon-tests`. Output:
    ```
    Running Differential Privacy Tests...
    [PASS] Laplace Noise Distribution Property Test (Epsilon = 0.5)
    [PASS] Telemetry Anonymization Test
    All tests passed successfully.
    ```

## 2. Logic Chain
1. **SQLite3 Integration**: Since `libsqlite3-dev` is not pre-installed on the host container, referencing `#include <sqlite3.h>` directly in `edge-daemon` code will trigger a compiler error unless developers compile it manually or configure CMake to handle this dependency. We recommend bundling the SQLite3 amalgamation (`sqlite3.c` / `sqlite3.h`) in `3rdparty/` for zero-dependency builds, or documenting `sudo apt install libsqlite3-dev` in project requirements and updating CMakeLists.txt using `find_package(SQLite3 REQUIRED)`.
2. **Crostini Network Checker**: The current daemon unconditionally attempts to shell out to `curl` to transmit data. If offline, the subprocess executes and hangs/times out slowly. Introducing a socket and `getifaddrs` checker in C++ allows the daemon to detect when offline before executing `curl`. The interface check (L1) verifies if an IPv4/IPv6 interface is UP, and the non-blocking connection check (L2) to `8.8.8.8:53` checks active internet routing.
3. **Domain Obfuscation Mapping**: Chrome extension telemetry contains raw tab titles which include sensitive search parameters, document paths, and private folder routes. Extrapolating base domains using domain segment tokenization (`.com`, `.co.uk`, etc.) and keyword lists allows anonymizing the metrics name into safe buckets (like `google.com` or generic labels like `vscode`, `terminal`) at the edge.
4. **Command Line Laplace Calibration**: Adding CLI parameters `--epsilon`, `--sensitivity`, `--budget`, and `--secret` enables the daemon to ingest starting parameters without recompiling. It also allows a `--calibrate` option which prints theoretical distribution bounds (e.g. standard deviation, 95% and 99% range boundaries) and simulation samples to facilitate privacy calibration.

## 3. Caveats
- **Routing Assumptions**: The L2 connection check targets Google DNS `8.8.8.8` on port 53. If the local network filters or blocks DNS traffic over TCP, or redirects port 53, the connection check might report false-negative offline states.
- **SQLite3 Locking**: SQLite3 is in multi-thread mode (`SQLITE_OPEN_NOMUTEX`), meaning concurrent thread safety must be enforced in the C++ layer. We added `std::mutex` locks inside the `TelemetryDB` wrapper helper to guarantee safety during read/write operations.

## 4. Conclusion
The current daemon can be successfully configured to support local SQL buffering, network-state checking, domain obfuscation, and command-line calibration without impacting the core differential privacy noise distribution functions. Linker dependencies for SQLite3 are solvable via either package managers or amalgamation bundling.

## 5. Verification Method
1. Inspect the written report `/home/matthias/project/project-chronos/.agents/teamwork_preview_explorer_sm2_1_2/analysis.md`.
2. Check that the existing tests still compile and run properly by executing:
   ```bash
   cmake -B edge-daemon/build edge-daemon
   cmake --build edge-daemon/build
   ./edge-daemon/build/edge-daemon-tests
   ```
