# Technical Analysis: Chrome Extension & Daemon Security Enhancements (SM2.4)

This analysis outlines the implementation design for enhancing the communication security, privacy features, and power efficiency of the Chronos ChromeOS Extension and Local C++ Daemon.

---

## 1. Encrypted Extension-Daemon IPC Bridge (F43)

### Security Model & Requirements
To prevent arbitrary local processes from sending spoofed telemetry or control payloads to the daemon, we implement an HMAC-SHA256 signature verification mechanism.
- The daemon receives a local configuration secret key via the CLI using the `--secret` parameter.
- The ChromeOS extension signs every request using HMAC-SHA256.
- The signed message includes a timestamp to prevent replay attacks.
- The daemon validates the timestamp (within a $\pm$ 60-second window) and verifies the signature using its shared secret.

### ChromeOS Extension Signed Headers Implementation
The Chrome extension generates the HMAC-SHA256 signature using the standard Web Crypto API (`crypto.subtle`), which is fully supported in Manifest V3 background service workers.

We compute the signature over a concatenated message: `timestamp + "." + method + "." + path + "." + body`.

```javascript
// Retrieve the shared secret configured in the extension settings
async function getSecretKey() {
  const result = await chrome.storage.local.get(["sharedSecret"]);
  return result.sharedSecret || "";
}

// Generate authentication headers
async function getSignedHeaders(method, path, body = "") {
  const timestamp = Math.floor(Date.now() / 1000).toString();
  const secretKey = await getSecretKey();
  
  // If no secret key is configured, return standard headers without signature
  if (!secretKey) {
    return { 'Content-Type': 'application/json' };
  }
  
  const message = `${timestamp}.${method}.${path}.${body}`;
  const encoder = new TextEncoder();
  const keyData = encoder.encode(secretKey);
  
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
    encoder.encode(message)
  );
  
  const signatureHex = Array.from(new Uint8Array(signatureBuffer))
    .map(b => b.toString(16).padStart(2, '0'))
    .join('');
    
  return {
    'Content-Type': 'application/json',
    'X-Chronos-Timestamp': timestamp,
    'X-Chronos-Signature': signatureHex
  };
}
```

### Daemon C++ Build Configuration Changes
To support HMAC-SHA256, the daemon will use OpenSSL's `libcrypto`. We modify `CMakeLists.txt` to find and link OpenSSL:

```cmake
# CMakeLists.txt modification
find_package(OpenSSL REQUIRED)

target_link_libraries(edge-daemon core_lib PkgConfig::GLIB OpenSSL::Crypto pthread)
target_link_libraries(edge-daemon-tests core_lib PkgConfig::GLIB OpenSSL::Crypto)
```

### Daemon C++ Verification Logic
In `edge-daemon/src/main.cpp`, we implement:
1. `compute_hmac_sha256` using OpenSSL EVP APIs.
2. `getHeaderValue` to parse HTTP headers.
3. Verification logic within the socket loop.

```cpp
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <iomanip>
#include <sstream>
#include <cmath>

std::string compute_hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    
    HMAC(EVP_sha256(), key.c_str(), key.length(), 
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(), 
         hash, &hash_len);
         
    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

std::string getHeaderValue(const std::string& request, const std::string& header_name) {
    size_t pos = request.find(header_name + ":");
    if (pos == std::string::npos) return "";
    size_t end_line = request.find("\n", pos);
    if (end_line == std::string::npos) end_line = request.length();
    std::string line = request.substr(pos + header_name.length() + 1, end_line - (pos + header_name.length() + 1));
    size_t first = line.find_first_not_of(" \r\n");
    size_t last = line.find_last_not_of(" \r\n");
    if (first == std::string::npos || last == std::string::npos) return "";
    return line.substr(first, last - first + 1);
}
```

Within `main` inside the socket acceptance loop:
```cpp
// Within while(true) loop after request content is completely read:
if (!g_config.secret.empty()) {
    std::string rec_sig = getHeaderValue(request, "X-Chronos-Signature");
    std::string rec_ts = getHeaderValue(request, "X-Chronos-Timestamp");
    
    if (rec_sig.empty() || rec_ts.empty()) {
        std::string err_resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"error\",\"message\":\"Unauthorized: Missing signature headers\"}";
        send(new_socket, err_resp.c_str(), err_resp.length(), 0);
        close(new_socket);
        continue;
    }
    
    // Validate timestamp against replay attacks (60s window)
    try {
        long long req_ts = std::stoll(rec_ts);
        long long now_ts = std::time(nullptr);
        if (std::abs(now_ts - req_ts) > 60) {
            std::string err_resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"error\",\"message\":\"Unauthorized: Request expired\"}";
            send(new_socket, err_resp.c_str(), err_resp.length(), 0);
            close(new_socket);
            continue;
        }
    } catch (...) {
        std::string err_resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"error\",\"message\":\"Bad Request: Invalid timestamp format\"}";
        send(new_socket, err_resp.c_str(), err_resp.length(), 0);
        close(new_socket);
        continue;
    }
    
    // Extract exact body
    size_t body_pos = request.find("\r\n\r\n");
    std::string body = "";
    if (body_pos != std::string::npos) {
        body = request.substr(body_pos + 4);
    } else {
        body_pos = request.find("\n\n");
        if (body_pos != std::string::npos) {
            body = request.substr(body_pos + 2);
        }
    }
    
    std::string expected_msg = rec_ts + "." + method + "." + path + "." + body;
    std::string computed_sig = compute_hmac_sha256(g_config.secret, expected_msg);
    
    if (computed_sig != rec_sig) {
        std::string err_resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"error\",\"message\":\"Unauthorized: Invalid signature\"}";
        send(new_socket, err_resp.c_str(), err_resp.length(), 0);
        close(new_socket);
        continue;
    }
}
```

---

## 2. Privacy Masking Regex Filters (F38)

### Configuration & Data Flow
Privacy filters can be configured in the extension options page and saved in `chrome.storage.local` under the `"privacyFilters"` key as an array of string regex patterns.

### Client-side Regex Matching
We implement a verification helper in `background.js` to match tab titles or extracted domains against the configured regex filters.

```javascript
let privacyFilters = [];

// Initialize and listen for updates
chrome.storage.local.get(["privacyFilters"], (result) => {
  if (result.privacyFilters) privacyFilters = result.privacyFilters;
});
chrome.storage.onChanged.addListener((changes, area) => {
  if (area === "local" && changes.privacyFilters) {
    privacyFilters = changes.privacyFilters.newValue || [];
  }
});

function isPrivacyExcluded(title, url) {
  if (!privacyFilters || privacyFilters.length === 0) return false;
  
  let domain = "";
  try {
    if (url) {
      domain = new URL(url).hostname;
    }
  } catch (e) {}

  for (const pattern of privacyFilters) {
    try {
      const regex = new RegExp(pattern, "i");
      if (regex.test(title)) return true;
      if (domain && regex.test(domain)) return true;
    } catch (e) {
      console.error("[Chronos] Invalid regex filter pattern:", pattern, e);
    }
  }
  return false;
}
```

### Ingestion Strategy
Two approaches are analyzed for handling matching items:
1. **Drop Entire Ingestion Payload**: Complete exclusion of the active window tick, ensuring the daemon receives no record. *Con*: Creates gaps in active telemetry.
2. **Redact Sensitive Information (Recommended)**: Submit the tick but redact the domain/title payload to `[Redacted]` or `[Privacy Masked]`. *Pro*: Maintains correct total duration stats, keeping privacy budget and laplace noise metrics accurate while completely obfuscating the specific title.

We adopt the redaction approach, changing the window title to `[Redacted]` before posting.

---

## 3. Dynamic Ingestion Cycle Interval (F40)

To allow the daemon to control ingestion timing dynamically (e.g. based on low battery or budget saturation), we restructure the extension loop.

### Extension Implementation (Recursive `setTimeout`)
We replace the hardcoded `setInterval` with a recursive `setTimeout` pattern, updating `currentInterval` based on the daemon's responses.

```javascript
let currentInterval = 10000; // default 10 seconds
let telemetryTimeoutId = null;

async function checkAndStreamTelemetry() {
  chrome.idle.queryState(15, async (idleState) => {
    if (idleState !== "active") {
      console.log(`[Chronos] User is ${idleState}. Skipping loop.`);
      scheduleNextCycle();
      return;
    }

    // Window tracking logic...
    const payloadStr = JSON.stringify(telemetryPayload);
    try {
      const headers = await getSignedHeaders("POST", "/ingest", payloadStr);
      const response = await fetch('http://localhost:8888/ingest', {
        method: 'POST',
        headers: headers,
        body: payloadStr
      });
      
      if (response.ok) {
        const data = await response.json();
        // Dynamically adjust interval if returned by the daemon
        if (data.interval && data.interval !== currentInterval) {
          currentInterval = data.interval;
          console.log(`[Chronos] Cycle interval updated to ${currentInterval}ms`);
        }
      }
    } catch (err) {
      // Offline fallback
    }

    scheduleNextCycle();
  });
}

function scheduleNextCycle() {
  if (telemetryTimeoutId) clearTimeout(telemetryTimeoutId);
  telemetryTimeoutId = setTimeout(checkAndStreamTelemetry, currentInterval);
}
```

### Daemon C++ Interval Generation
The daemon calculates the interval based on status parameters and includes it in JSON responses:
```cpp
// Add interval to GET /status, POST /control, and POST /ingest responses
int interval_ms = getDynamicInterval();
std::stringstream ss;
ss << "HTTP/1.1 200 OK\r\n"
   << "Content-Type: application/json\r\n"
   << "Access-Control-Allow-Origin: *\r\n\r\n"
   << "{\"status\":\"ok\",\"interval\":" << interval_ms << "}";
```

---

## 4. Multi-Window Desktop Focus Tracker (F45)

### Chrome Extension Focus Tracker
Currently, the extension queries only the current window's active tab. To monitor multiple windows while identifying the currently focused window:

```javascript
chrome.windows.getAll({ populate: true, windowTypes: ['normal'] }, async (windows) => {
  if (!windows || windows.length === 0) {
    scheduleNextCycle();
    return;
  }
  
  // Sort windows by ID to ensure stable indexing
  windows.sort((a, b) => a.id - b.id);
  
  let windowsList = [];
  let focusedIndex = -1;
  let activeTitle = "";

  windows.forEach((win, index) => {
    const activeTab = win.tabs.find(t => t.active);
    let title = activeTab ? activeTab.title : "Unknown ChromeOS App";
    let url = activeTab ? activeTab.url : "";
    
    if (isPrivacyExcluded(title, url)) {
      title = "[Redacted]";
    }

    windowsList.push({
      title: title,
      focused: win.focused
    });

    if (win.focused) {
      focusedIndex = index;
      activeTitle = title;
    }
  });

  // Construct backward-compatible JSON schema
  const payloadObj = {
    window: activeTitle, // Maintains compatibility with existing "window" parser
    windows: windowsList,
    focused_index: focusedIndex,
    timestamp: Math.floor(Date.now() / 1000)
  };
});
```

### Daemon C++ Log Integration
The daemon parses `focused_index` from the incoming request payload using its existing parsing conventions:

```cpp
int focused_index = -1;
double temp_val;
if (extractDouble(request, "focused_index", temp_val)) {
    focused_index = static_cast<int>(temp_val);
}
std::cout << "[Edge Daemon] Focused Window Index: " << focused_index << std::endl;
```

---

## 5. Battery Status Power Saver (F48)

To preserve battery life on Chronos edge trackers, the C++ daemon reads native power supply metrics and instructs the extension to slow down ingestion.

### Native Battery Parsing on Linux
We read the state from `/sys/class/power_supply/`:

```cpp
#include <filesystem>
#include <fstream>

int getBatteryLevel() {
    try {
        std::string path = "/sys/class/power_supply";
        if (std::filesystem::exists(path)) {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                std::string name = entry.path().filename().string();
                if (name.rfind("BAT", 0) == 0) {
                    std::ifstream file(entry.path() / "capacity");
                    int cap = -1;
                    if (file >> cap) return cap;
                }
            }
        }
    } catch (...) {}
    return -1;
}

bool isBatteryDischarging() {
    try {
        std::string path = "/sys/class/power_supply";
        if (std::filesystem::exists(path)) {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                std::string name = entry.path().filename().string();
                if (name.rfind("BAT", 0) == 0) {
                    std::ifstream file(entry.path() / "status");
                    std::string status;
                    if (file >> status) {
                        // Clean status string
                        size_t first = status.find_first_not_of(" \r\n");
                        size_t last = status.find_last_not_of(" \r\n");
                        if (first != std::string::npos && last != std::string::npos) {
                            status = status.substr(first, last - first + 1);
                        }
                        return status == "Discharging";
                    }
                }
            }
        }
    } catch (...) {}
    return false;
}
```

### Ingestion Throttling Logic
When the battery is discharging and drops below 20%, we increase the cycle time from 10 seconds to 30 seconds:

```cpp
int getDynamicInterval() {
    int cap = getBatteryLevel();
    if (cap >= 0 && cap < 20 && isBatteryDischarging()) {
        return 30000; // 30s throttled cycle
    }
    return 10000; // 10s default
}
```

---

## 6. Global Override Pause Hotkey (F50)

Allows the user to completely disable active tracking for 1 hour via a global ChromeOS shortcut.

### Extension Hotkey Registration
We bind a global command shortcut in `/chromeos-extension/manifest.json`:

```json
{
  "manifest_version": 3,
  "name": "Chronos ChromeOS Edge Tracker",
  "permissions": ["tabs", "idle", "storage"],
  "commands": {
    "toggle-pause-tracking": {
      "suggested_key": {
        "default": "Ctrl+Shift+Y",
        "mac": "Command+Shift+Y"
      },
      "description": "Pause tracking for 1 hour",
      "global": true
    }
  }
}
```

### Extension Command Listener
We capture the key command in `background.js` and call the daemon control endpoint:

```javascript
chrome.commands.onCommand.addListener(async (command) => {
  if (command === "toggle-pause-tracking") {
    console.log("[Chronos] Hotkey triggered: Pausing tracking for 1 hour");
    try {
      const payload = JSON.stringify({ action: "pause", duration_ms: 3600000 });
      const headers = await getSignedHeaders("POST", "/control", payload);
      
      await fetch('http://localhost:8888/control', {
        method: 'POST',
        headers: headers,
        body: payload
      });
    } catch (err) {
      console.error("[Chronos] Failed to send pause request:", err);
    }
  }
});
```

### Daemon Timer Handling
In `main.cpp`, we store a steady-state time point for automatic resumption, rendering the ephemeral background worker's potential shutdown harmless:

```cpp
#include <chrono>

std::chrono::steady_clock::time_point g_pause_until = std::chrono::steady_clock::now();

bool isTrackingPaused() {
    if (g_tracking_paused) {
        if (std::chrono::steady_clock::now() < g_pause_until) {
            return true;
        } else {
            // Auto-resume after timer expiration
            g_tracking_paused = false;
        }
    }
    return false;
}
```

We update the control logic to support duration:
```cpp
if (action == "pause") {
    g_tracking_paused = true;
    double duration_ms = 0.0;
    if (extractDouble(body, "duration_ms", duration_ms) && duration_ms > 0.0) {
        g_pause_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(duration_ms));
    } else {
        g_pause_until = std::chrono::steady_clock::time_point::max();
    }
    success = true;
} else if (action == "resume") {
    g_tracking_paused = false;
    g_pause_until = std::chrono::steady_clock::now();
    success = true;
}
```

We replace raw usage of `g_tracking_paused` inside the server request handlers and background threads (`backgroundTelemetryThread`) with `isTrackingPaused()`.
