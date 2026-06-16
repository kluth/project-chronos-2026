# Handoff Report: Chrome Extension & Security Integration (Sub-Milestone 2.4)

## 1. Observation
* **Observed Files**:
  * `chromeos-extension/background.js`: Line 4-30 implements a fixed `setInterval` ingestion loop posting raw titles to the loopback server:
    ```javascript
    setInterval(() => {
      // Query the system to see if the user has physically interacted (mouse/keyboard) in the last 15 seconds
      chrome.idle.queryState(15, (idleState) => {
        ...
        // Stream the raw data to the local C++ Daemon via localhost loopback
        fetch('http://localhost:8888/ingest', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ window: activeTitle })
        })
    ```
  * `chromeos-extension/manifest.json`: Defines permissions `"tabs"` and `"idle"` with host permission `"http://localhost:8888/*"`. No `"storage"` permission or `"commands"` hotkeys are defined.
  * `edge-daemon/src/main.cpp`: Line 325-373 parses command-line flags. It supports parsing `--secret` but only stores it in `g_config.secret`. Line 427-647 implements a basic HTTP server listening on port 8888. It parses `/status` and `/control` paths, and accepts POST `/ingest` request bodies, extracting the active title with string index lookup without validating signatures.
  * `edge-daemon/CMakeLists.txt`: Line 7-11 finds dependencies:
    ```cmake
    find_package(SQLite3 REQUIRED)
    find_package(PkgConfig REQUIRED)
    
    pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)
    ```
    Currently lacks configuration to locate or compile with OpenSSL (`libcrypto`).

* **Observed Actions**:
  * Ran daemon baseline tests in `edge-daemon/build` using `make && ./edge-daemon-tests`. 
  * All 10 tests passed successfully, indicating a clean compile target.

## 2. Logic Chain
1. **F43 secure IPC integration** requires the extension and daemon to compute matching HMAC-SHA256 digests. 
   - *Observation*: The daemon CLI parses `--secret`, but lacks verification logic; the extension lacks access to storage permissions to configure a secret.
   - *Inference*: We must declare the `"storage"` permission in `manifest.json` so the extension can store the secret, calculate standard HMAC signatures in `background.js` using `crypto.subtle`, and link `OpenSSL::Crypto` in `CMakeLists.txt` to enable OpenSSL-based verification in `main.cpp`.
2. **F38 privacy regex filtration** must discard sensitive telemetry data.
   - *Observation*: Standard ingestion sends the active title straight to localhost.
   - *Inference*: Checking titles/URLs against dynamic regex patterns stored in `chrome.storage.local` prior to fetching localhost ensures privacy controls are strictly client-side.
3. **F40 dynamic interval adaptation** requires dynamic scheduler changes.
   - *Observation*: The extension currently utilizes `setInterval` for a hardcoded 10-second cadence.
   - *Inference*: Replacing `setInterval` with a recursive `setTimeout` loop enables updating the timer's next schedule length whenever a response JSON contains a new `requested_interval` value.
4. **F45 multi-window desktop focus tracker** requires scanning all open browser frames.
   - *Observation*: The extension currently uses `chrome.tabs.query({ active: true, currentWindow: true })`, tracking only the active frame of the current execution window.
   - *Inference*: Querying via `chrome.windows.getAll({ populate: true })` allows compiling active tabs across all browser windows and identifying which window index holds focus.
5. **F48 battery conservation** and **F50 override hotkey** require status throttling and manual pauses.
   - *Observation*: The daemon currently tracks DBus locks, but has no mechanism to determine battery limits or support duration-based temporary overrides.
   - *Inference*: Checking battery levels under `/sys/class/power_supply` enables the daemon to double the requested interval when battery is <20%. Registering a global keyboard shortcut in `manifest.json` and sending a `/control` pause action with a specific duration allows the daemon to automatically resume after 3600 seconds.

## 3. Caveats
* **Virtualization Boundaries**: On ChromeOS, if the C++ daemon executes inside Crostini, access to `/sys/class/power_supply` might be restricted or virtualized depending on Crostini system settings. In production, using a DBus fallback via `org.freedesktop.UPower` should be evaluated if sysfs is restricted.
* **Storage Invalidation**: If the user has not configured a storage secret key, signature verification is bypassed by default to enable permissive debug mode. In production, a secure default should be enforced.

## 4. Conclusion
Integrating secure IPC, multi-window logging, privacy masking, dynamic interval rates, battery throttles, and override hotkeys requires:
1. Declaring `"storage"` and `"commands"` in `manifest.json`.
2. Using recursive `setTimeout` and `crypto.subtle` inside `background.js`.
3. Updating `CMakeLists.txt` to compile against `OpenSSL::Crypto`.
4. Inserting header verification, sysfs power scanners, and duration-based check logic inside `main.cpp`.

## 5. Verification Method
* **Compilation**:
  - The modified daemon code must compile successfully by calling `cmake .. && make` inside `edge-daemon/build`.
* **Testing Harness**:
  - Run tests with `./edge-daemon-tests` to ensure core Differential Privacy functions remain unbroken.
* **Validation Conditions**:
  - Confirm requests without a valid `X-Signature` header return `HTTP 410` / `401` when `--secret` parameter is set.
  - Assert that ingestion request frequency shifts from $10\text{ seconds}$ to $30\text{ seconds}$ when the battery status is discharging below 20%.
