// background.js - Host OS Telemetry Sidecar
console.log("[Chronos] ChromeOS Sidecar initialized. Interaction detection active.");

let currentInterval = 10000; // Default 10 second ingestion cycle
let schedulerTimeoutId = null;

// Helper to get secret key from local storage
async function getSecret() {
  return new Promise((resolve) => {
    chrome.storage.local.get(["secret"], (result) => {
      resolve(result.secret || "");
    });
  });
}

// Helper to check privacy filters
async function shouldExclude(title, url) {
  return new Promise((resolve) => {
    chrome.storage.local.get(["privacyFilters"], (result) => {
      const filters = result.privacyFilters || [];
      for (const pattern of filters) {
        try {
          const regex = new RegExp(pattern, "i");
          if (regex.test(title) || regex.test(url)) {
            resolve(true);
            return;
          }
        } catch (e) {
          console.error(`[Chronos] Invalid regex filter: ${pattern}`, e);
        }
      }
      resolve(false);
    });
  });
}

// Generate HMAC-SHA256 signature for secure IPC
async function generateSignature(secret, method, path, timestamp, body) {
  const enc = new TextEncoder();
  const message = `${method.toUpperCase()}\n${path}\n${timestamp}\n${body}`;
  const key = await crypto.subtle.importKey(
    "raw",
    enc.encode(secret),
    { name: "HMAC", hash: { name: "SHA-256" } },
    false,
    ["sign"]
  );
  const signature = await crypto.subtle.sign("HMAC", key, enc.encode(message));
  return Array.from(new Uint8Array(signature))
    .map(b => b.toString(16).padStart(2, "0"))
    .join("");
}

function scheduleNextIngestion() {
  if (schedulerTimeoutId) {
    clearTimeout(schedulerTimeoutId);
  }
  schedulerTimeoutId = setTimeout(performIngestionCycle, currentInterval);
}

function performIngestionCycle() {
  // Query the system to see if the user has physically interacted (mouse/keyboard) in the last 15 seconds
  chrome.idle.queryState(15, (idleState) => {
    if (idleState !== "active") {
      console.log(`[Chronos] User is ${idleState}. Dropping telemetry to prevent false efficiency metrics.`);
      scheduleNextIngestion();
      return; // Do not send data if the user is AFK/Idle
    }

    // Query all Chrome windows to satisfy multi-window focus tracking (F45)
    chrome.windows.getAll({ populate: true }, async (windows) => {
      if (!windows || windows.length === 0) {
        scheduleNextIngestion();
        return;
      }

      let focusedWindowIndex = -1;
      const windowDataList = [];

      for (let i = 0; i < windows.length; i++) {
        const win = windows[i];
        if (win.focused) {
          focusedWindowIndex = i;
        }

        const activeTab = win.tabs ? win.tabs.find(t => t.active) : null;
        windowDataList.push({
          windowIndex: i,
          title: activeTab ? activeTab.title : "Unknown ChromeOS App",
          url: activeTab ? activeTab.url : "",
          focused: win.focused
        });
      }

      console.log(`[Chronos] Active windows tracked:`, windowDataList, `Focused Index: ${focusedWindowIndex}`);

      // Extract focused window tab info for primary logging
      const focusedTabInfo = windowDataList.find(w => w.focused) || windowDataList[0];
      const activeTitle = focusedTabInfo.title;
      const activeUrl = focusedTabInfo.url;

      // Privacy Masking Check (F38)
      const excluded = await shouldExclude(activeTitle, activeUrl);
      if (excluded) {
        console.log(`[Chronos] Privacy Exclusion Match for active tab: Title: "${activeTitle}" URL: "${activeUrl}". Dropping payload.`);
        scheduleNextIngestion();
        return;
      }

      // Stream the raw data to the local C++ Daemon via localhost loopback
      try {
        const path = "/ingest";
        const body = JSON.stringify({
          window: activeTitle,
          windows: windowDataList,
          focusedWindowIndex: focusedWindowIndex
        });

        const headers = { 'Content-Type': 'application/json' };
        
        // Retrieve and apply IPC signing (F43)
        const secret = await getSecret();
        if (secret) {
          const timestamp = Date.now().toString();
          const signature = await generateSignature(secret, "POST", path, timestamp, body);
          headers['X-Timestamp'] = timestamp;
          headers['X-Signature'] = signature;
        }

        const response = await fetch(`http://localhost:8888${path}`, {
          method: 'POST',
          headers: headers,
          body: body
        });

        if (response.ok) {
          const resJson = await response.json();
          if (resJson && resJson.requested_interval) {
            // Adjust ingestion cycle interval dynamically (F40)
            currentInterval = resJson.requested_interval;
            console.log(`[Chronos] Dynamic Ingestion Interval set to ${currentInterval}ms`);
          }
        }
      } catch (err) {
        // Silently fail if the C++ daemon inside Crostini is offline
        console.warn("[Chronos] Telemetry transmission failed.", err);
      }

      scheduleNextIngestion();
    });
  });
}

// Bind Global Override Pause Hotkey (F50)
chrome.commands.onCommand.addListener(async (command) => {
  if (command === "toggle-pause-override") {
    console.log("[Chronos] Pause Hotkey pressed. Pausing daemon tracking for 1 hour.");
    try {
      const path = "/control";
      const body = JSON.stringify({ action: "pause_override", duration: 3600 });
      const headers = { 'Content-Type': 'application/json' };

      const secret = await getSecret();
      if (secret) {
        const timestamp = Date.now().toString();
        const signature = await generateSignature(secret, "POST", path, timestamp, body);
        headers['X-Timestamp'] = timestamp;
        headers['X-Signature'] = signature;
      }

      const res = await fetch(`http://localhost:8888${path}`, {
        method: 'POST',
        headers: headers,
        body: body
      });

      if (res.ok) {
        console.log("[Chronos] 1-hour pause command successfully acknowledged by daemon.");
      }
    } catch (err) {
      console.error("[Chronos] Failed to send pause command to daemon.", err);
    }
  }
});

// Start the ingestion schedule
scheduleNextIngestion();

