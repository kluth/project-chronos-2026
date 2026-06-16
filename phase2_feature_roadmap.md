# Chronos Phase 2 Feature Roadmap

This document outlines the 50 distinct features proposed for Phase 2 of Project Chronos. These features span the Angular dashboard client, the Firebase CRDT backend functions, and the C++/ChromeOS edge-daemon telemetry tracking system.

---

## 1. Angular Dashboard (17 Features)

1. **Interactive Canvas Drag-and-Drop Editor (`dashboard.component.ts`)**: Let users click and drag private anchors on the canvas to update their start and end boundaries directly in Firestore.
2. **Offline-First Synchronization Indicator (`dashboard.component.ts`)**: Add a connection state status bar indicating whether the Angular client is currently offline and showing the count of locally cached operations pending Firestore synchronization.
3. **Historical Time Allocation Reports Panel (`app.routes.ts` / new component)**: Add a dedicated routes view displaying cumulative metrics of total private hours protected vs. work tasks deleted over customizable timeframes.
4. **Preemption Notification Toast Alerts (`dashboard.component.ts`)**: Integrate a toast notification system triggered when a Firestore subscription detects a `WorkTask` has been deleted or preempted by a conflicting `PrivateLifeAnchor`.
5. **Interactive Privacy Regime Slider (`dashboard.component.ts`)**: A settings slider in the header allowing the user to dynamically adjust their differential privacy epsilon configuration, saving changes to a user settings document in Firestore.
6. **Telemetry Density Custom Canvas Chart (`dashboard.component.ts`)**: Build a canvas-rendered line chart showing the frequency of ChromeOS tab telemetry over the last 24 hours, displaying aggregated, anonymized noise-corrected values.
7. **Export Schedule to iCalendar (.ics) and CSV (`dashboard.component.ts`)**: Introduce buttons to export the currently active, conflict-resolved calendar schedule to standard CSV or iCal formats.
8. **Jira Integration Configurator UI (`dashboard.component.ts`)**: Create a settings panel in the frontend where users can save their Jira Cloud configurations, target projects, and OAuth scopes to allow background syncing.
9. **Private Anchor Location Autocomplete & Geocoding (`dashboard.component.ts`)**: Embed a text input that uses Google Places API to assign latitude/longitude coordinates to a private life anchor, enabling spatial boundary rules.
10. **Custom Life-Anchor Category Painter (`dashboard.component.ts`)**: Support user-defined anchor categories (e.g., "Family", "Health", "Social", "Exercise") with distinct canvas background colors and icons.
11. **Grid Snap-to-Interval Setting (`dashboard.component.ts`)**: Introduce a user preference to snap anchor drag-and-drop actions to intervals of 15, 30, or 60 minutes.
12. **Multi-Device Live Session Indicator (`dashboard.component.ts` / `auth.service.ts`)**: Display status bubbles in the dashboard header indicating other active devices (e.g., "Edge Daemon Active on Chromebook 1") using Firestore presence tracking.
13. **Local Storage Backup & Recovery Helper (`dashboard.component.ts`)**: Maintain temporary state in local storage for new draft anchors so that progress is preserved in case of sudden network failure or tab closure before sync.
14. **Custom Timeline Start/End Hour Config (`dashboard.component.ts`)**: Provide input controls to adjust the visible timeline grid starting hour (default 9:00 AM) and ending hour (default 6:00 PM) on the canvas.
15. **Timeline Zoom and Pan Navigation (`dashboard.component.ts`)**: Implement mouse wheel zoom and horizontal panning controls on the canvas wrapper to ease inspection of dense scheduling blocks.
16. **Task Category Filters (`dashboard.component.ts`)**: Include checkbox filters in the dashboard header allowing users to toggle rendering of "Jira Tickets", "Local Work Tasks", or "Anonymized Activity" on the timeline canvas.
17. **Cyberpunk Dark/Light Mode Theme Toggle (`app.css` / `dashboard.component.ts`)**: Implement a theme switcher that alters the dashboard styles and modifies the Canvas 2D fillStyle color palettes.

---

## 2. Firebase CRDT Backend (17 Features)

18. **Vector Clock Synchronization Schema (`src/crdt.ts`)**: Expand the standard interfaces to include logical vector clocks, tracking update counts from multiple client devices to support causal consistency.
19. **Dynamic Scheduling Policy Engine (`src/index.ts` / `src/crdt.ts`)**: Create a policy engine that parses user-specified JSON rules (e.g., "during 9am-5pm work tasks take priority, otherwise private wins") instead of a hardcoded rule.
20. **Replication Audit Logging System (`src/index.ts`)**: Write all preemption events, conflict resolutions, and active deletes to a dedicated `users/{userId}/audit_logs` subcollection for tracking.
21. **Scheduled Presence Heartbeat Sweeper (`src/index.ts`)**: Deploy a cron-triggered Firebase function that runs every 5 minutes to clear outdated active session heartbeats in Firestore.
22. **Outbound Webhooks Subscriptions (`src/index.ts`)**: Allow external enterprise systems to subscribe to schedule changes by dispatching HTTP POST webhooks when work tasks are preempted.
23. **MapReduce Aggregation Cloud Function (`src/index.ts`)**: Periodically aggregate raw time-series telemetry events in `users/{userId}/telemetry` into summarized daily usage metrics, cleaning up raw points.
24. **Differential Privacy Epsilon Validator (`src/index.ts`)**: A validation rule in Firestore or Cloud Functions ensuring incoming telemetry records have an appropriate variance level corresponding to their epsilon.
25. **Slack Preemption Alerts Webhook (`src/index.ts`)**: Send automated Slack notifications to the user's workspace when a work meeting is automatically deleted because it conflicts with a private anchor.
26. **Recurring Private Anchors Engine (`src/index.ts`)**: Expand backend schemas and triggers to evaluate and auto-populate recurring events (e.g., every weekday 15:00-16:00).
27. **Token-Based REST API Auth for Telemetry Ingestion (`src/index.ts`)**: Validate ingestion requests using a JWT or secure API key stored in user configuration documents before appending metrics.
28. **Soft-Delete and Recovery Subcollection (`src/index.ts`)**: Move preempted work tasks to `users/{userId}/archived_work_tasks` instead of permanent deletion, enabling users to restore or reschedule.
29. **Preemption Frequency Analytics Endpoints (`src/index.ts`)**: Expose an HTTPS REST endpoint calculating statistics about deleted work blocks to help users measure their work-life balance improvements.
30. **Jira Status Syncer Webhook (`src/index.ts`)**: When a Jira-based work task is deleted by a private anchor, call Jira REST APIs to transition the ticket status to "Blocked" or "Rescheduled".
31. **Strict Temporal Bounds Validation (`src/index.ts`)**: Implement strict schema checks on write triggers, ensuring that start timestamps are strictly less than end timestamps and bounds are positive.
32. **Geofenced Context Resolution (`src/index.ts`)**: Implement location checks so that private anchors only trigger preemption of work tasks if the user is verified to be away from the work office.
33. **Telemetry Ingestion Rate Limiter (`src/index.ts`)**: Add rate-limiting logic on the `ingestTelemetry` HTTPS endpoint using Firebase Realtime Database or Firestore counters to block high-frequency spam.
34. **Bulk Calendar (.ics) Import Endpoint (`src/index.ts`)**: An HTTPS upload parser endpoint that accepts `.ics` file streams and batches insertion of scheduling blocks.

---

## 3. C++/ChromeOS Edge Tracking (16 Features)

35. **Local SQLite Offline Buffering (`edge-daemon/src/main.cpp`)**: Set up a local SQLite database in `edge-daemon` to buffer telemetry events when internet connectivity is lost, uploading in bulk on reconnection.
36. **DBus System Event Listener (`edge-daemon/src/main.cpp`)**: Connect `edge-daemon` to the host system DBus interface to capture OS suspend, resume, and screen-lock events, immediately pausing tracking.
37. **Native Process Scanner `/proc` Monitor (`edge-daemon/src/main.cpp`)**: Read `/proc` directories inside Crostini to detect active IDEs, terminal shells, and local tools, adding them to active trackers.
38. **Privacy Masking Regex Filters (`chromeos-extension/background.js`)**: Add options configuration in the Chrome Extension to exclude matching domain patterns or tab title prefixes before sending JSON payloads.
39. **Local Privacy Budget Tracker (`edge-daemon/src/differential_privacy.cpp`)**: Monitor cumulative privacy leakage (sum of epsilon values) and automatically increase noise or pause ingestion if it exceeds a 24-hour limit.
40. **Dynamic Ingestion Cycle Interval (`chromeos-extension/background.js`)**: Let the daemon return a requested delay in its HTTP response (e.g. shorter when active, longer when idle), adjusting the extension's loop interval dynamically.
41. **Crostini Network State Checker (`edge-daemon/src/main.cpp`)**: Monitor active network state interfaces via sockets (`getifaddrs`) to prevent triggering external curl execution when disconnected.
42. **System Tray Daemon Controller Applet (`edge-daemon/src/main.cpp`)**: Implement a simple background tray launcher (using GTK/Qt) allowing users to pause, restart, or configure daemon parameters locally.
43. **Encrypted Extension-Daemon IPC Bridge (`edge-daemon/src/main.cpp` / `chromeos-extension/background.js`)**: Secure loopback POST requests using HMAC-SHA256 signatures with a local configuration secret key.
44. **GDPR-Compliant Keystroke Aggregator (`edge-daemon/src/main.cpp` / `chromeos-extension/background.js`)**: Track total keystrokes typed per minute inside active contexts using aggregate counters. Do not implement a raw keylogger or store keys, preserving GDPR compliance.
45. **GDPR-Compliant Mouse Distance Tracker (`edge-daemon/src/main.cpp` / `chromeos-extension/background.js`)**: Track the total distance traveled by the mouse in pixels per minute as an aggregate counter. Do not save X/Y coordinates, ensuring strict GDPR compliance.
46. **Local Domain Obfuscation Mapping (`edge-daemon/src/main.cpp`)**: Rewrite full URLs to base domains (e.g., mapping private documents to generic terms) at the edge before injecting differential privacy noise.
47. **Automated Shared Folder Snapshots (`edge-daemon/src/main.cpp`)**: Export backup schedule JSONs periodically to a host-shared directory (e.g. `~/Downloads/chronos_backups`) for local recovery.
48. **Battery Status Power Saver (`chromeos-extension/background.js`)**: Query the chrome extension power/battery API and throttle ingestion frequency when battery level drops below 20%.
49. **Command Line Laplace Calibration Utility (`edge-daemon/src/main.cpp`)**: Expose command line arguments (`--epsilon`, `--sensitivity`) to calibrate the C++ noise generation bounds on startup.
50. **Global Override Pause Hotkey (`chromeos-extension/background.js` / `edge-daemon/src/main.cpp`)**: Support a ChromeOS key binding that sends a pause trigger to the edge-daemon, pausing all local window tracking for 1 hour.
