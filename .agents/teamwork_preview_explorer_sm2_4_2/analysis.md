# Technical Analysis Report: ChromeOS Extension & Daemon Telemetry Enhancements

This report provides a detailed analysis of how to implement the six requested security, privacy, and telemetry features in `chromeos-extension/background.js` and `edge-daemon/src/main.cpp`.

---

## 1. Encrypted Extension-Daemon IPC Bridge (F43)

### Objective
Secure the loopback IPC channel between the ChromeOS extension and the Crostini C++ daemon using HMAC-SHA256 signatures derived from a configuration secret key (configured via `--secret` parameter in the daemon).

### Design & Architecture
- **Key Storage**: The daemon receives the secret key from the command line argument `--secret` (stored in `g_config.secret`). The extension stores the corresponding secret key in its local settings or chrome storage (`chrome.storage.local`).
- **Signature Algorithm**: HMAC-SHA256.
- **Replay Attack Protection**: Every request includes a timestamp header `X-Chronos-Timestamp`. The signature is computed over the string `timestamp + "." + request_body`. The daemon enforces that the timestamp is within $\pm 300$ seconds (5 minutes) of the local system clock.
- **Constant-Time Verification**: The daemon compares the received signature and calculated signature using a constant-time comparison helper to prevent timing side-channels.

### Extension implementation (`chromeos-extension/background.js`)
Since MV3 Service Workers lack the Node `crypto` library, we use the standard browser Web Crypto API.

```javascript
// Sign payload using HMAC-SHA256 (Web Crypto API)
async function signPayload(payload, secret) {
  const encoder = new TextEncoder();
  const keyData = encoder.encode(secret);
  const messageData = encoder.encode(payload);

  const cryptoKey = await crypto.subtle.importKey(
    "raw",
    keyData,
    { name: "HMAC", hash: { name: "SHA-256" } },
    false,
    ["sign"]
  );

  const signatureBuffer = await crypto.subtle.sign(
    "HMAC",
    cryptoKey,
    messageData
  );

  const hashArray = Array.from(new Uint8Array(signatureBuffer));
  return hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
}
```

### Daemon Build Modification (`edge-daemon/CMakeLists.txt`)
Modify the build script to link OpenSSL:
```cmake
find_package(OpenSSL REQUIRED)
# ...
target_link_libraries(edge-daemon core_lib PkgConfig::GLIB OpenSSL::Crypto pthread)
```

### Daemon Implementation (`edge-daemon/src/main.cpp`)
Add HMAC calculation, constant-time verification, and header checking.

```cpp
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <iomanip>

// Helper to compute HMAC-SHA256
std::string computeHmacSha256(const std::string& secret, const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    HMAC(EVP_sha256(), secret.c_str(), secret.length(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         hash, &hash_len);
    
    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

// Constant-time string comparison
bool constantTimeCompare(const std::string& a, const std::string& b) {
    if (a.length() != b.length()) return false;
    unsigned char result = 0;
    for (size_t i = 0; i < a.length(); ++i) {
        result |= (a[i] ^ b[i]);
    }
    return result == 0;
}

// Timestamp validation
bool validateTimestamp(const std::string& ts_str) {
    try {
        unsigned long long req_ts = std::stoull(ts_str);
        unsigned long long current_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        long long diff = static_cast<long long>(current_ts) - static_cast<long long>(req_ts);
        return std::abs(diff) < 300000; // 5 minutes tolerance
    } catch (...) {
        return false;
    }
}
```

In the HTTP Server connection loop (`edge-daemon/src/main.cpp` line 427+), parse the request and validate if `g_config.secret` is active:
```cpp
// Check signatures if a secret is configured
if (!g_config.secret.empty()) {
    std::string signature = getHeaderValue(request, "X-Chronos-Signature");
    std::string timestamp = getHeaderValue(request, "X-Chronos-Timestamp");
    
    if (signature.empty() || timestamp.empty() || !validateTimestamp(timestamp)) {
        std::string err_resp = "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n\r\n";
        send(new_socket, err_resp.c_str(), err_resp.length(), 0);
        close(new_socket);
        continue;
    }
    
    std::string body = getRequestBody(request);
    std::string expected = computeHmacSha256(g_config.secret, timestamp + "." + body);
    if (!constantTimeCompare(signature, expected)) {
        std::string err_resp = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n";
        send(new_socket, err_resp.c_str(), err_resp.length(), 0);
        close(new_socket);
        continue;
    }
}
```

---

## 2. Privacy Masking Regex Filters (F38)

### Objective
Allow the user to configure regex filters to exclude tab titles or domains before sending telemetry payloads to the local daemon.

### Extension Implementation (`chromeos-extension/background.js`)
Retrieve a list of exclusion filters from `chrome.storage.local`. Before sending the payload, check both the current tab's title and its domain (parsed from the tab URL) against each configured regex.

```javascript
async function getPrivacyFilters() {
  return new Promise((resolve) => {
    chrome.storage.local.get(["excludeFilters"], (result) => {
      resolve(result.excludeFilters || []);
    });
  });
}

async function shouldExcludeTab(tab, filters) {
  if (!filters || filters.length === 0) return false;
  
  const title = tab.title || "";
  const url = tab.url || "";
  let domain = "";
  try {
    if (url) {
      const urlObj = new URL(url);
      domain = urlObj.hostname;
    }
  } catch (e) {
    // Ignore URL parsing errors (e.g. chrome:// extensions)
  }

  for (const filter of filters) {
    try {
      const regex = new RegExp(filter, 'i'); // Case-insensitive matching
      if (regex.test(title) || (domain && regex.test(domain))) {
        return true;
      }
    } catch (e) {
      console.error("[Chronos] Invalid regex filter:", filter, e);
    }
  }
  return false;
}
```

If a filter matches, the loop drops the ingestion cycle without executing the `fetch()` post request.

### Daemon Implementation (`edge-daemon/src/main.cpp` - Defense-in-depth)
For robust security and privacy posture, the daemon implements similar checks on the received window title. We expand the `DPConfig` struct to store `privacy_filters` (configured via command-line `--filter <pattern>` options or a configuration parameter).

```cpp
#include <regex>

bool shouldExcludeWindow(const std::string& window_title, const std::vector<std::string>& filters) {
    for (const auto& pattern : filters) {
        try {
            std::regex re(pattern, std::regex_constants::icase);
            if (std::regex_search(window_title, re)) {
                return true;
            }
        } catch (const std::regex_error& e) {
            std::cerr << "[Regex Filter] Invalid pattern: " << pattern << std::endl;
        }
    }
    return false;
}
```

In `main.cpp` inside the ingestion path:
```cpp
if (shouldExcludeWindow(active_window, g_config.privacy_filters)) {
    std::cout << "[Edge Daemon] Ingestion dropped: matches privacy regex filter." << std::endl;
    // Respond OK but discard telemetry
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"excluded\"}";
    send(new_socket, response.c_str(), response.length(), 0);
    close(new_socket);
    continue;
}
```

---

## 3. Dynamic Ingestion Cycle Interval (F40)

### Objective
Allow the daemon to control the polling frequency dynamically. The daemon responds to ingestion payloads with a JSON field `requested_interval_ms` which the extension parses and applies to its next schedule.

### Extension Implementation (`chromeos-extension/background.js`)
Replace the static `setInterval` (line 4) with a recursive `setTimeout` pattern to dynamically adjust schedules.

```javascript
let currentIntervalMs = 10000; // Default 10s
let timeoutId = null;

function runIngestionCycle() {
  chrome.idle.queryState(15, (idleState) => {
    if (idleState !== "active") {
      scheduleNext(currentIntervalMs);
      return;
    }

    chrome.tabs.query({ active: true, currentWindow: true }, async (tabs) => {
      if (tabs && tabs.length > 0) {
        const activeTab = tabs[0];
        
        // Apply privacy filtering
        const filters = await getPrivacyFilters();
        if (await shouldExcludeTab(activeTab, filters)) {
          scheduleNext(currentIntervalMs);
          return;
        }
        
        // Build payload (see Multi-Window section below)
        const payload = JSON.stringify({ ... });
        const headers = { 'Content-Type': 'application/json' };
        
        // Sign if secret configured
        // ...
        
        fetch('http://localhost:8888/ingest', {
          method: 'POST',
          headers: headers,
          body: payload
        })
        .then(response => response.json())
        .then(data => {
          if (data && data.requested_interval_ms) {
            updateInterval(data.requested_interval_ms);
          }
        })
        .catch(err => {
          console.error("[Chronos] Ingestion request failed", err);
        })
        .finally(() => {
          scheduleNext(currentIntervalMs);
        });
      } else {
        scheduleNext(currentIntervalMs);
      }
    });
  });
}

function updateInterval(newIntervalMs) {
  if (newIntervalMs && newIntervalMs !== currentIntervalMs) {
    console.log(`[Chronos] Interval updated: ${currentIntervalMs}ms -> ${newIntervalMs}ms`);
    currentIntervalMs = newIntervalMs;
  }
}

function scheduleNext(delay) {
  if (timeoutId) clearTimeout(timeoutId);
  timeoutId = setTimeout(runIngestionCycle, delay);
}

// Kickstart cycle
scheduleNext(currentIntervalMs);
```

### Daemon Implementation (`edge-daemon/src/main.cpp`)
Add an integer variable `ingestion_interval_ms` to `DPConfig` (defaults to `10000`). Include this parameter in JSON responses for `/ingest` and `/status`:

```cpp
// In HTTP status handler response payload construction
ss << "{"
   << "\"paused\":" << (g_tracking_paused ? "true" : "false") << ","
   << "\"requested_interval_ms\":" << g_config.ingestion_interval_ms
   << "}";
```

---

## 4. Multi-Window Desktop Focus Tracker (F45)

### Objective
Track multiple active Chrome windows, logging the title of the active tab in each window and identifying which window is currently focused.

### Extension Implementation (`chromeos-extension/background.js`)
Instead of `chrome.tabs.query({ active: true, currentWindow: true })`, use the `chrome.windows.getAll` API populated with tabs.

```javascript
chrome.windows.getAll({ populate: true }, (windows) => {
  const windowList = [];
  let focusedIndex = -1;

  windows.forEach((win, index) => {
    // Locate the active tab in this specific window
    const activeTab = win.tabs ? win.tabs.find(t => t.active) : null;
    const tabTitle = activeTab ? (activeTab.title || "Untitled Tab") : "No Active Tab";

    windowList.push({
      index: index,
      id: win.id,
      title: tabTitle,
      focused: win.focused
    });

    if (win.focused) {
      focusedIndex = index;
    }
  });

  const focusedTabTitle = focusedIndex !== -1 ? windowList[focusedIndex].title : "No Focused Window";

  // Payload format:
  // {
  //   "window": focusedTabTitle,
  //   "windows": windowList,
  //   "focused_index": focusedIndex
  // }
});
```

### Daemon Implementation (`edge-daemon/src/main.cpp`)
Parse the JSON payload to output the focused window index. Since GLib is linked, we can use GLib json-parser, or parse basic JSON variables using standard C++ logic:

```cpp
int focused_index = -1;
size_t f_pos = request.find("\"focused_index\":");
if (f_pos != std::string::npos) {
    size_t val_start = f_pos + 16;
    size_t end_pos = request.find_first_of(",}", val_start);
    if (end_pos != std::string::npos) {
        std::stringstream ss(request.substr(val_start, end_pos - val_start));
        ss >> focused_index;
    }
}

std::cout << "[Edge Daemon] Telemetry Received:" << std::endl;
std::cout << "  - Active Window: " << active_window << std::endl;
std::cout << "  - Focused Window Index: " << focused_index << std::endl;
```

---

## 5. Battery Status Power Saver (F48)

### Objective
Throttle the ingestion frequency (e.g. increase cycle interval to 30000ms) when the device's battery drops below 20%.

### Implementation Choice
Manifest V3 Service Workers do not expose browser APIs for system battery access (`navigator.getBattery()` is blocked in worker contexts).
To circumvent this, we implement low-battery detection **daemon-side**. In Linux/Crostini environments, system battery information is readable via `/sys/class/power_supply/`.

### Daemon Implementation (`edge-daemon/src/main.cpp`)
Create helpers to read Linux power supply information:

```cpp
#include <fstream>

int getBatteryCapacity() {
    std::ifstream file("/sys/class/power_supply/BAT0/capacity");
    int capacity = -1;
    if (file >> capacity) return capacity;
    
    // Fallback path
    std::ifstream file_alt("/sys/class/power_supply/BAT1/capacity");
    if (file_alt >> capacity) return capacity;
    
    return -1;
}

std::string getBatteryStatus() {
    std::ifstream file("/sys/class/power_supply/BAT0/status");
    std::string status = "Unknown";
    if (file >> status) return status;
    return "Unknown";
}

void enforceBatteryPowerSaver() {
    int capacity = getBatteryCapacity();
    std::string status = getBatteryStatus();
    
    if (capacity >= 0 && capacity < 20 && status == "Discharging") {
        if (g_config.ingestion_interval_ms != 30000) {
            g_config.ingestion_interval_ms = 30000;
            std::cout << "[Power Saver] Battery low (" << capacity 
                      << "%). Throttling ingestion interval to 30000ms." << std::endl;
        }
    } else {
        if (g_config.ingestion_interval_ms == 30000) {
            g_config.ingestion_interval_ms = 10000; // Reset to default
            std::cout << "[Power Saver] Battery restored. Restoring interval to 10000ms." << std::endl;
        }
    }
}
```

Call `enforceBatteryPowerSaver()` inside the request loop before building the response. The updated interval will be sent as `requested_interval_ms` to the Chrome Extension, dynamically slowing down the extension's polling frequency.

---

## 6. Global Override Pause Hotkey (F50)

### Objective
Provide a ChromeOS keyboard shortcut that pauses window tracking telemetry for exactly 1 hour.

### Architecture Choice
MV3 Service Workers are ephemeral and will kill any active JavaScript `setTimeout()` when idle. Therefore, we store and enforce the pause expiration **daemon-side** using a lazy timestamp checker.

### Extension Configuration (`chromeos-extension/manifest.json`)
Declare the hotkey using `commands`:
```json
"commands": {
  "toggle-pause-one-hour": {
    "suggested_key": {
      "default": "Ctrl+Shift+Y"
    },
    "description": "Pause tracking for 1 hour"
  }
}
```

### Extension Logic (`chromeos-extension/background.js`)
Register the command listener. Upon trigger, perform a control request to pause the daemon:
```javascript
chrome.commands.onCommand.addListener((command) => {
  if (command === "toggle-pause-one-hour") {
    console.log("[Chronos] Pause hotkey detected.");
    
    // Call daemon control API with 1 hour duration
    fetch('http://localhost:8888/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: "pause", duration_seconds: 3600 })
    }).catch(err => console.error("Control API error", err));
  }
});
```

### Daemon Implementation (`edge-daemon/src/main.cpp`)
Track the timed pause expiration globally:
```cpp
std::atomic<bool> g_tracking_paused_by_timer(false);
std::chrono::steady_clock::time_point g_pause_expiry_time;

void checkPauseExpiry() {
    if (g_tracking_paused_by_timer && std::chrono::steady_clock::now() >= g_pause_expiry_time) {
        g_tracking_paused = false;
        g_tracking_paused_by_timer = false;
        std::cout << "[Timer] Pause period expired. Auto-resuming telemetry tracking." << std::endl;
    }
}
```

In the `/control` HTTP handler (line 510+), check if `duration_seconds` is passed in:
```cpp
if (action == "pause") {
    g_tracking_paused = true;
    
    int duration = 3600; // Default 1 hour
    extractInt(body, "duration_seconds", duration);
    
    g_pause_expiry_time = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
    g_tracking_paused_by_timer = true;
    success = true;
}
```

Invoke `checkPauseExpiry()` at the top of incoming request handlers and loops (e.g. at line 481, before processing routes).

---

## Synthesis & Integration Plan

1. **Replay-Safe Signature Verification**: Provides defense against request interception. 
2. **Double-Sided Privacy Filtration**: Client-side filtering saves network resources and prevents leakages; daemon-side filtering protects against misconfigured extensions.
3. **Synergy of Throttling Elements**: Low-battery thresholds trigger daemon-side interval calculation, which is parsed by the client's loop during dynamic interval updates.
4. **Timed Pauses**: The Hotkey command utilizes the daemon `/control` API to configure dynamic timer blocks, circumventing Manifest V3 worker lifecycles.
