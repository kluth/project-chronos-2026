let keystrokeCount = 0;
let mouseDistance = 0;
let lastMouseX = null;
let lastMouseY = null;

window.addEventListener('keydown', () => {
  keystrokeCount++;
}, true);

window.addEventListener('mousemove', (event) => {
  if (lastMouseX !== null && lastMouseY !== null) {
    const dx = event.clientX - lastMouseX;
    const dy = event.clientY - lastMouseY;
    mouseDistance += Math.sqrt(dx * dx + dy * dy);
  }
  lastMouseX = event.clientX;
  lastMouseY = event.clientY;
}, true);

setInterval(() => {
  chrome.runtime.sendMessage({
    type: 'user_activity_metrics',
    keystrokes: keystrokeCount,
    mousePixels: mouseDistance
  }, () => {
    // Avoid unhandled runtime error if context/extension is reloaded
    if (chrome.runtime.lastError) {
      // Ignored
    }
  });
  keystrokeCount = 0;
  mouseDistance = 0;
}, 5000);
