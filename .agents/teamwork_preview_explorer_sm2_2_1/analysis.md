# Chronos Edge-Daemon Enhancements Analysis (Sub-Milestone 2.2)

## 1. Executive Summary
This analysis outlines the engineering design and implementation details for four key edge-daemon features in Sub-Milestone 2.2:
- **Local Privacy Budget Tracker (F39)**: Enforces differential privacy limits by tracking cumulative epsilon values consumed over a rolling 24-hour window, scaling down epsilon (increasing noise) or pausing ingestion when limits are reached.
- **Native Process Scanner `/proc` Monitor (F37)**: Continuously checks `/proc/<pid>/comm` in the Crostini container to detect active developer tools.
- **Device Resource Performance Telemetry (F44)**: Reads `/proc/stat` and `/proc/meminfo` to calculate local CPU and RAM utilization.
- **Automated Shared Folder Snapshots (F47)**: Periodically exports local SQLite telemetry events and privacy budget logs to the host-shared folder (`~/Downloads/chronos_backups`) in JSON format.

All four features are designed to run in a cohesive, low-resource, non-blocking background thread to preserve the responsiveness of the host ChromeOS socket loop. The proposed design has been verified compile-clean and test-clean using a temporary local compilation harness.

---

## 2. Local Privacy Budget Tracker (F39)
### Concept & Privacy Theory
Differential Privacy (DP) works by adding noise to query responses. For repeated queries or streams, the privacy leakage is cumulative. Under basic composition, the total privacy parameter $\epsilon_{total}$ is the sum of the individual epsilons used:
$$\epsilon_{total} = \sum_{i} \epsilon_i$$
To limit the maximum leakage in a 24-hour window, the daemon must log each event's consumed epsilon and restrict ingestion when approaching the budget.

### SQLite Database Schema
We introduce a new table `privacy_budget_log` in the SQLite database to store logged epsilons:
```sql
CREATE TABLE IF NOT EXISTS privacy_budget_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    epsilon REAL NOT NULL,
    timestamp INTEGER NOT NULL
);
```

### Budget Control Logic
1. **Window Query & Log Purge**:
   When evaluating cumulative budget consumption:
   - Clean up records older than 24 hours: `DELETE FROM privacy_budget_log WHERE timestamp < (now - 86400)`
   - Query total consumed epsilon: `SELECT SUM(epsilon) FROM privacy_budget_log WHERE timestamp >= (now - 86400)`
2. **Adjustment Policy**:
   Let the configured budget limit be `budget_limit` (configured via command-line arg `--budget` mapping to `g_config.budget`):
   - **Normal Zone** ($\text{cumulative} < 0.8 \times \text{budget\_limit}$): Use base $\epsilon$.
   - **Adjustment Zone** ($0.8 \times \text{budget\_limit} \le \text{cumulative} < \text{budget\_limit}$): Scale down $\epsilon$ linearly:
     $$\epsilon_{adj} = \epsilon_{base} \times \frac{\text{budget\_limit} - \text{cumulative}}{0.2 \times \text{budget\_limit}}$$
     To prevent divisions by zero or infinite noise bounds, we clamp $\epsilon_{adj} \ge 0.005$.
   - **Exceeded Zone** ($\text{cumulative} \ge \text{budget\_limit}$): Ingestion is paused.

---

## 3. Native Process Scanner `/proc` Monitor (F37)
### Crostini Process Discovery
To detect active developer tools inside Crostini without calling expensive external tools (like `ps`), we scan `/proc` directly.
- Numeric directories under `/proc` correspond to active PIDs.
- For each PID, the file `/proc/<pid>/comm` contains the name of the executable (truncated to 15 chars), which is lightweight and fast to read.
- We check the process names against a pre-defined set of developer tools:
  - IDEs: `code` (VSCode), `code-oss`
  - Terminals/Shells: `bash`, `zsh`, `fish`, `tmux`
  - Version Control & Runtimes: `git`, `docker`, `python`, `python3`, `node`
  - Compilers & Build Tools: `gcc`, `g++`, `make`, `cmake`

If any matching tools are found, a telemetry event prefix `dev_tool_<name>` is injected into the telemetry pipeline.

---

## 4. Device Resource Performance Telemetry (F44)
Telemetry is obtained by reading `/proc` system files at regular intervals.

### CPU Utilization (`/proc/stat`)
The first line of `/proc/stat` lists cumulative CPU times in different modes since boot:
$$\text{cpu} \quad \text{user} \quad \text{nice} \quad \text{system} \quad \text{idle} \quad \text{iowait} \quad \text{irq} \quad \text{softirq} \quad \text{steal} \quad \dots$$
1. Let $\text{IdleTime} = \text{idle} + \text{iowait}$
2. Let $\text{NonIdleTime} = \text{user} + \text{nice} + \text{system} + \text{irq} + \text{softirq} + \text{steal}$
3. Let $\text{TotalTime} = \text{IdleTime} + \text{NonIdleTime}$
4. We take two readings $t_1, t_2$ separated by the cycle time (10s):
   $$\text{Utilization \%} = \frac{\Delta \text{TotalTime} - \Delta \text{IdleTime}}{\Delta \text{TotalTime}} \times 100.0$$

### RAM Utilization (`/proc/meminfo`)
We parse `/proc/meminfo` to extract:
- `MemTotal:` Total usable RAM.
- `MemAvailable:` Amount of available RAM (standard in modern Linux kernels >= 3.14).
$$\text{RAM Utilization \%} = \frac{\text{MemTotal} - \text{MemAvailable}}{\text{MemTotal}} \times 100.0$$

---

## 5. Automated Shared Folder Snapshots (F47)
Exporting telemetry snapshots to `~/Downloads/chronos_backups/` provides local offline data portability.

### Directory Resolution & Creation
- The backup path is resolved dynamically by reading the `HOME` environment variable:
  $$\text{Path} = \text{HOME} + \text{/Downloads/chronos_backups}$$
- Using C++17 `<filesystem>`, we call `std::filesystem::create_directories()` to ensure the directory tree exists before writing.

### File Serialization
The backup exports two tables (`telemetry_events` and `privacy_budget_log`) in JSON format. The output is structured with a date-based ISO name (e.g. `chronos_backup_YYYYMMDD_HHMMSS.json`):
```json
{
  "timestamp": 1782367823,
  "buffered_telemetry": [
    {
      "id": 1,
      "metric_name": "vscode",
      "value": 10.0,
      "timestamp": 1782367800
    }
  ],
  "privacy_budget_log": [
    {
      "id": 1,
      "epsilon": 0.5,
      "timestamp": 1782367800
    }
  ]
}
```

---

## 6. Integration Architecture
To prevent CPU blocking on socket connections, a background daemon thread `backgroundTelemetryThread` is spawned at startup in `main.cpp` using `std::thread`.

### Pipeline Execution Flow (Every 10 seconds)
1. **Identify Tools**: Run `/proc` monitor. Iterate through PID folders.
2. **Collect Performance**: Read CPU state delta and calculate utilization. Parse `/proc/meminfo` to get memory utilization.
3. **Budget Check**: Query SQLite cumulative 24h budget. If within adjustment range, reduce $\epsilon$. If budget is exceeded, drop metrics.
4. **Anonymize**: Apply Laplace DP noise scale $b = \text{sensitivity} / \epsilon_{adj}$.
5. **Transmit/Buffer**: If network is online, attempt ingestion. Otherwise, buffer to `telemetry_buffer.db`.
6. **Snapshot Backup**: Every 15 minutes, serialize the SQLite database state to JSON under `~/Downloads/chronos_backups/`.

---

## 7. Verification and Testing
A full integration test suite was written in `tests/differential_privacy_test.cpp` to verify correctness:
- **`testPrivacyBudgetTracker`**: Logs epsilons, queries cumulative values, cleans old values, and checks adjusted epsilon calculation rules.
- **`testProcessScanner`**: Scans native `/proc` directories and verifies correct detection of running tools (e.g. `bash`, `python3`).
- **`testResourcePerformanceTelemetry`**: Simulates CPU ticks to verify the delta formula and parses memory.
- **`testSharedFolderSnapshots`**: Assures that JSON directories and schemas are created and saved correctly.

### Build and Test Command
To build and execute tests:
```bash
cd edge-daemon
rm -rf build && cmake -B build -S . && cmake --build build
./build/edge-daemon-tests
```

---

## 8. Reference Patch File
The complete diff implementation is located in the agent's working directory:
`/home/matthias/project/project-chronos/.agents/teamwork_preview_explorer_sm2_2_1/proposed_changes.patch`
This patch can be applied cleanly using the standard git command:
`git apply .agents/teamwork_preview_explorer_sm2_2_1/proposed_changes.patch`
