# Handoff Report — Explorer 2 Sub-Milestone 2.2

## 1. Observation
We observed the following definitions, implementation states, and structures in the `edge-daemon` codebase:
* **Configuration State (`/edge-daemon/src/differential_privacy.h` lines 10-15)**:
  ```cpp
  struct DPConfig {
      double epsilon = 0.5;
      double sensitivity = 1.0;
      double budget = 0.0;
      std::string secret = "";
  };
  ```
  `g_config` is defined globally, and CLI parameters for `--budget` are already parsed in `/edge-daemon/src/main.cpp` (lines 44-53).
* **Ingestion Flow (`/edge-daemon/src/main.cpp` lines 112-164)**:
  Incoming HTTP requests from the Chrome extension are parsed for active windows:
  ```cpp
  std::string active_window = "";
  size_t pos = request.find("\"window\":\"");
  // ... extracts window title ...
  TelemetryEvent safe_event = anonymizeEvent(cleaned_event, g_config.epsilon);
  ```
* **SQLite Buffering System (`/edge-daemon/src/differential_privacy.cpp` lines 288-315)**:
  `initDatabase(db_path)` sets up the SQLite database `telemetry_buffer.db` and defines the `telemetry_events` table structure.
* **Testing Suite (`/edge-daemon/tests/differential_privacy_test.cpp`)**:
  Provides tests for Laplace noise, obfuscation mapping, and SQLite buffering. Baseline tests compile and pass via CMake target `edge-daemon-tests`.

---

## 2. Logic Chain
1. **F39 (Privacy Budget)**: Summing `epsilon` values for telemetry events logged in the last 24h allows us to calculate local differential privacy leakage. If it is $>80\%$ of `g_config.budget`, we dynamically scale `epsilon` down, increasing Laplace noise ($b = \text{sensitivity}/\epsilon$). If it exceeds the budget, we pause ingestion by early-returning HTTP status `"paused"`.
2. **F37 (Process Scanner)**: By scanning `/proc/<pid>/exe` and `/proc/<pid>/cmdline` for target names (vscode, terminal, compiler/git), the daemon running inside Crostini can capture development tools operating in the Linux container.
3. **F44 (Resource Performance)**: Calculating the delta in CPU ticks between cycles from `/proc/stat` and parsing `MemTotal` and `MemAvailable` from `/proc/meminfo` yields accurate metrics for CPU and RAM consumption.
4. **F47 (Backup Snapshots)**: Fetching telemetry events from SQLite and formatting them to a JSON array using standard C++ outputs backups to `~/Downloads/chronos_backups/` periodically (e.g. every 5 minutes).

---

## 3. Caveats
* **Process Detection Namespace**: Process scanning is constrained to the Crostini namespace; host ChromeOS applications/Android apps are not visible in `/proc` and must be sent via host Chrome extension window titles.
* **First CPU Measurement**: The initial CPU measurement yields `0.0%` as it sets the baseline tick reference.

---

## 4. Conclusion
We have formulated a complete architectural plan and source code design for implementing F37, F39, F44, and F47 entirely within the edge-daemon module. This layout maintains the differential privacy contract and protects privacy boundaries persistently against container restarts.

---

## 5. Verification Method
To verify implementation once coded:
1. Run `make` and `./edge-daemon-tests` in `edge-daemon/` to run all unit tests.
2. Launch daemon with `--budget 0.5 --epsilon 0.1` and simulate telemetry inputs. Observe the logging details:
   - Cumulative epsilon is logged to `privacy_budget_log` table.
   - Epsilon is halved to `0.05` on the 5th request.
   - The 6th request receives a `{"status":"paused","reason":"privacy budget exceeded"}` HTTP response.
3. Verify backups write successfully formatted JSON structures to `~/Downloads/chronos_backups/`.
