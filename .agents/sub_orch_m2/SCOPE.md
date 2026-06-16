# Scope: Milestone 2 - Edge Tracker Enhancements

## Architecture
Milestone 2 enhances the C++ `edge-daemon` and Chrome OS extension `chromeos-extension` to improve local buffering, process monitoring, privacy management, system event listening, and secure extension-daemon communication.

### Component Design
- **Local SQLite Offline Buffering**: A SQLite3 database handles telemetry storage when offline.
- **Process Scanner & GDPR Activity Tracker**: Reading `/proc` to check for active developer tools (IDEs, terminals) inside Crostini, and logging aggregate keystrokes and mouse distance metrics.
- **DBus System Listener**: Connects to System DBus (via `libdbus` or standard DBus protocol) to track sleep/lock/resume.
- **System Tray Applet**: A basic GTK applet or a console applet acting as controller.
- **Privacy Obfuscation & Budget Tracker**: Domain mapping and cumulative DP epsilon budget calculations.
- **Extension Features**: Battery level checker, idle query adjustments, regex privacy masking, GDPR-compliant keystroke and mouse distance tracking (aggregate counts per minute only), and signature-based IPC validation (HMAC-SHA256).

---

## Milestones

| # | Name | Scope / Features Covered | Dependencies | Status |
|---|------|--------------------------|--------------|--------|
| 1 | Sub-Milestone 2.1: Core Ingestion, Calibration, Network, & Obfuscation | F41 (Network State Checker), F46 (Domain Obfuscation Mapping), F49 (Calibrator Utility), F35 (SQLite offline buffer schema/DB) | None | DONE |
| 2 | Sub-Milestone 2.2: Privacy Budget & Native Process Scanning | F39 (Privacy Budget), F37 (/proc Process Scanner), F44 (GDPR Keystroke Aggregator), F47 (Shared Folder Snapshot backup) | SM1 | DONE |
| 3 | Sub-Milestone 2.3: DBus & Applet UI Integration | F36 (DBus Listener), F42 (System Tray Controller Applet) | SM1 | DONE |
| 4 | Sub-Milestone 2.4: Chrome Extension, Security, & Ingestion Controls | F38 (Privacy Regex Filters), F40 (Dynamic Interval), F43 (Encrypted IPC Bridge), F45 (GDPR Mouse Distance Tracker), F48 (Battery Power Saver), F50 (Global Pause Hotkey) | SM2, SM3 | DONE |

---

## Interface Contracts

### Extension-Daemon IPC Bridge (F43, F40)
- **Endpoint**: `POST http://localhost:8888/ingest`
- **Request Headers**:
  - `Content-Type: application/json`
  - `X-Signature: <hex HMAC-SHA256 signature of request body>`
  - `X-Timestamp: <timestamp in seconds>`
- **Request Body JSON**:
  ```json
  {
    "window": "Tab Title",
    "keystrokes_per_minute": 45,
    "mouse_pixels_per_minute": 1200,
    "batteryLevel": 0.85,
    "isPaused": false
  }
  ```
- **Response Headers**:
  - `Content-Type: application/json`
  - `Access-Control-Allow-Origin: *`
- **Response Body JSON**:
  ```json
  {
    "status": "ok",
    "interval": 10000,
    "isPaused": false
  }
  ```

### Calibration Command Line Flags (F49)
- `--epsilon <double>`: Set start epsilon (default `0.5`).
- `--sensitivity <double>`: Set sensitivity for DP Laplace noise (default `1.0`).
- `--budget <double>`: Set max 24h epsilon privacy budget (default `5.0`).
- `--secret <string>`: Set secret key for HMAC IPC verification.
