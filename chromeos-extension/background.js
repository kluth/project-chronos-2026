// background.js - Host OS Telemetry Sidecar
console.log("[Chronos] ChromeOS Sidecar initialized. Interaction detection active.");

setInterval(() => {
  // Query the system to see if the user has physically interacted (mouse/keyboard) in the last 15 seconds
  chrome.idle.queryState(15, (idleState) => {
    if (idleState !== "active") {
      console.log(`[Chronos] User is ${idleState}. Dropping telemetry to prevent false efficiency metrics.`);
      return; // Do not send data if the user is AFK/Idle
    }

    // If active, query the actual ChromeOS host for the active tab/window
    chrome.tabs.query({ active: true, currentWindow: true }, (tabs) => {
      if (tabs && tabs.length > 0) {
        const activeTitle = tabs[0].title || "Unknown ChromeOS App";
        
        console.log(`[Chronos] Active Interaction Detected. Window: ${activeTitle}`);

        // Stream the raw data to the local C++ Daemon via localhost loopback
        fetch('http://localhost:8888/ingest', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ window: activeTitle })
        }).catch(err => {
          // Silently fail if the C++ daemon inside Crostini is offline
        });
      }
    });
  });
}, 10000); // 10 second ingestion cycle
