# Analysis Report: Chrome Extension & Security Integration (Sub-Milestone 2.4)

## Executive Summary
This report provides a read-only investigation and concrete technical blueprint for implementing security, privacy, and control features spanning the ChromeOS sidecar extension (`chromeos-extension/background.js`) and the local telemetry C++ daemon (`edge-daemon/src/main.cpp`). 

The primary goals are to:
1. **Secure localhost loopback IPC (F43)** against unauthorized or spoofed payloads using cryptographic HMAC-SHA256 signatures and replay-attack windows.
2. **Apply client-side privacy controls (F38)** to prevent sensitive tab titles/domains from ever leaving the browser boundary using user-defined regular expression filters.
3. **Optimize power consumption and synchronization (F40, F48)** via a dynamic interval adaptation protocol that throttles polling when the device is running on low battery (discharging below 20%).
4. **Introduce desktop awareness and manual overrides (F45, F50)** to log multi-window focus states and enable a global keyboard command to pause logging for exactly 1 hour.

Proposed modifications are detailed below and backed up by complete replacement assets (`proposed_background.js`, `proposed_manifest.json`) and a unified diff (`proposed_changes.patch`) written to the agent directory `/home/matthias/project/project-chronos/.agents/teamwork_preview_explorer_sm2_4_1/`.

---

## Technical Feasibility & Design Decisions

### F43: Encrypted Extension-Daemon IPC Bridge
* **Goal**: Establish integrity and authenticity of POST/GET requests originating from the Chrome Extension to the C++ daemon listening on `localhost:8888`.
* **Design Decision**: 
  * **Web Crypto API**: The MV3 background service worker uses the browser's native `crypto.subtle` API to import the raw string secret key and compute standard HMAC-SHA256 digests. This avoids bundling external JS libraries, satisfying strict Chrome Web Store policies.
  * **OpenSSL (C++)**: The daemon utilizes `libcrypto` (`openssl/hmac.h`) to compute the expected HMAC. The dependency is declared and linked in `CMakeLists.txt` via standard `find_package(OpenSSL REQUIRED)`.
  * **API Message Construction**: To prevent request alteration, the signature is computed over:
    `METHOD + "\n" + PATH + "\n" + X-Timestamp + "\n" + REQUEST_BODY`
  * **Replay Protection**: The Extension includes a milliseconds-since-epoch timestamp header (`X-Timestamp`). The Daemon rejects any request where the time delta relative to local clock time exceeds $60\text{ seconds}$ ($60000\text{ ms}$).
  * **Timing Attacks**: Signature comparisons in `main.cpp` are conducted in constant-time (avoiding standard `strcmp` or `operator==`) via a manual bitwise OR difference loop to prevent side-channel timing disclosures.

### F38: Privacy Masking Regex Filters
* **Goal**: Filter out telemetry payloads based on regex matches against the tab title or domain.
* **Design Decision**: 
  * Filtering is enforced **directly inside the Extension worker** prior to transmitting payload bytes across the localhost loopback. This ensures private browsing patterns, internal project titles, or specific workspaces never cross process boundaries.
  * Filters are retrieved from the extension’s `chrome.storage.local` memory (configured via an options interface).
  * Patterns are compiled into Javascript `RegExp` objects with the case-insensitive (`i`) modifier. If either the active tab title or active tab URL tests positive, the payload is skipped.

### F40: Dynamic Ingestion Cycle Interval
* **Goal**: The extension dynamically changes its telemetry polling interval based on values dictated by the daemon's runtime state.
* **Design Decision**:
  * Instead of `setInterval` which enforces a fixed cadence, the service worker utilizes a **recursive `setTimeout` scheduler**.
  * The response of every loopback request (such as POST `/ingest`) returns a JSON body containing `requested_interval` (in milliseconds).
  * The scheduler stores this interval in memory and schedules the next payload cycle using the adjusted duration.

### F45: Multi-Window Desktop Focus Tracker
* **Goal**: Track active tabs across multiple Chrome windows and identify the index of the focused window.
* **Design Decision**:
  * Query all windows using `chrome.windows.getAll({ populate: true })`. This returns all Window objects and populated active tab lists.
  * Construct a structured array of active tabs containing `windowIndex`, `title`, `url`, and `focused` state.
  * Locate the index of the window where `focused === true`. Log this focused index and pass the structured list of active tabs to the daemon inside the `windows` payload key, keeping the primary active tab title under the `window` key.

### F48: Battery Status Power Saver
* **Goal**: Automatically reduce ingestion frequency when battery level falls below 20%.
* **Design Decision**:
  * **Daemon-Side Reading**: The C++ daemon queries the kernel battery capacity interface located at `/sys/class/power_supply/`.
  * The helper checks `/sys/class/power_supply/*/capacity` for charge percentage, and `/sys/class/power_supply/*/status` to verify if the state is `"Discharging"`. 
  * If the device is discharging and battery percentage drops below 20%, the daemon adjusts `requested_interval` to `30000` (30 seconds) instead of the base `10000` (10 seconds). The extension adjusts automatically upon receiving the next API response.

### F50: Global Override Pause Hotkey
* **Goal**: Enable a keyboard hotkey to pause daemon window tracking for exactly 1 hour.
* **Design Decision**:
  * Register a global command in `manifest.json` under `"commands"`. The ChromeOS specific hotkey is bound to `Ctrl+Shift+H` (with a cross-platform default of `Ctrl+Shift+U`).
  * In `background.js`, register `chrome.commands.onCommand.addListener` to detect trigger execution.
  * When triggered, make a Signed POST request to the daemon's `/control` endpoint passing: `{"action": "pause_override", "duration": 3600}`.
  * In the daemon, we maintain a dedicated `g_override_paused` atomic state and timestamp `g_override_paused_until`. On verification, `g_override_paused_until` is updated to `now + 3600 seconds`.
  * The unified checking helper `isTelemetryPaused()` is used to check both DBus sleep state (`g_tracking_paused`) and the timer-based override state (`g_override_paused`). If `now >= g_override_paused_until`, the override automatically resets to `false`.

---

## Architectural Data Flow
The sequence diagram below represents the interaction patterns among the various components:

```
[ ChromeOS Window Manager ]
        │ (User switch focus)
        ▼
┌──────────────────────────────────────────┐
│         Chrome Extension (MV3)           │
│  - Queries windows & active tabs (F45)    │
│  - Filters active title & URL (F38)      │
│  - Computes HMAC-SHA256 signature (F43)   │
└──────────────────┬───────────────────────┘
                   │ 
                   │ Signed HTTP POST (X-Signature, X-Timestamp)
                   ▼
┌──────────────────────────────────────────┐
│           Local C++ Daemon               │
│  - Validates HMAC / Replay Windows (F43)  │
│  - Queries battery capacity / status     │
│  - Computes throttle interval (F48, F40)  │
│  - Evaluates pause state (F50)           │
└──────────────────┬───────────────────────┘
                   │ 
                   │ HTTP 200 OK {"requested_interval": 30000}
                   ▼
┌──────────────────────────────────────────┐
│         Chrome Extension (MV3)           │
│  - Updates setTimeout duration (F40)     │
└──────────────────────────────────────────┘
```

---

## Summary of Code Deliverables

The files created in my working directory contain the complete, production-ready blueprint of the proposed changes:

1. **`proposed_manifest.json`**:
   * Registers `"storage"` permission required for HMAC secret key and regex filter configuration.
   * Registers `"toggle-pause-override"` hotkey command with native ChromeOS mapping (`Ctrl+Shift+H`).
2. **`proposed_background.js`**:
   * Migrates from rigid `setInterval` to a dynamic `setTimeout` scheduler.
   * Leverages Web Crypto API for HMAC-SHA256 signature construction on `/ingest` and `/control` endpoints.
   * Queries all Chrome windows via `chrome.windows.getAll` to track window focus indexes and populate active tabs list.
   * Inspects `chrome.storage.local` patterns for client-side regex exclusion matches before transmitting data.
3. **`proposed_changes.patch`**:
   * Unified diff patch to `CMakeLists.txt` adding `OpenSSL` libraries.
   * Modifies `edge-daemon/src/main.cpp` to introduce a secure signature verification engine, Linux `/sys/class/power_supply` reader, dual-state DBus and hotkey override evaluation, and JSON payload parser helpers.
