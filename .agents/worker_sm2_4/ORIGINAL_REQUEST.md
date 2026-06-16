## 2026-06-16T11:39:04Z

You are a worker tasked with implementing Sub-Milestone 2.4 (Chrome Extension, Security, & Ingestion Controls) for Project Chronos.

Working directory: /home/matthias/project/project-chronos/.agents/worker_sm2_4/

Scope of changes:
You need to implement the following features:
- F38 (Privacy Regex Filters): Filter out active titles and URLs matched against dynamic patterns from local storage before fetching.
- F40 (Dynamic Interval): Replace setInterval with a recursive setTimeout loop adjusting next check based on `requested_interval` in response.
- F43 (Encrypted IPC Bridge): Implement HMAC-SHA256 signature verification with timestamp replay check (60-second window) on the C++ Daemon, and signature generation on the extension using crypto.subtle. Add OpenSSL to CMakeLists.txt.
- F45 (Multi-Window Focus): Scan all windows and compile focus state list, extracting the focused window's active tab.
- F48 (Battery Power Saver): Daemon checks power supply status under `/sys/class/power_supply` and throttles request interval to 30 seconds if battery < 20% and discharging.
- F50 (Global Override Pause Hotkey): Add keyboard shortcut "Ctrl+Shift+H" (or default "Ctrl+Shift+U") to pause daemon window tracking for 1 hour.

Reference patch and proposed files from Explorer 10 (sm2_4_1):
- Patch: /home/matthias/project/project-chronos/.agents/teamwork_preview_explorer_sm2_4_1/proposed_changes.patch
- Background JS: /home/matthias/project/project-chronos/.agents/teamwork_preview_explorer_sm2_4_1/proposed_background.js
- Manifest: /home/matthias/project/project-chronos/.agents/teamwork_preview_explorer_sm2_4_1/proposed_manifest.json

Verification commands and criteria:
- Compile the C++ Daemon: inside `edge-daemon/build/`, run `cmake .. && make`
- Run the test suite: `./edge-daemon-tests`
- Ensure all tests pass.

Git commit guidance:
- Commit incrementally as features are implemented.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A Forensic Auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Please report your completion back via send_message to the parent conversation ID, and write a detailed handoff.md in your working directory.
