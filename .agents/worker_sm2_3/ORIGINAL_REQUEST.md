## 2026-06-16T11:26:22Z

You are the Worker for Sub-Milestone 2.3.
Your working directory is `/home/matthias/project/project-chronos/.agents/worker_sm2_3/`.

Task Details:
1. Implement **F36: DBus System Event Listener**:
   - Create a background thread in `edge-daemon` using `gio-2.0` (GDBus/GIO) or `dbus-1` to subscribe to System and Session DBus signals.
   - Listen to System Bus `PrepareForSleep` signal from `org.freedesktop.login1.Manager` interface. When `active` is true (suspending), set a global atomic `g_tracking_paused = true`. When false (resuming), set `g_tracking_paused = false`.
   - Listen to System Bus `Lock`/`Unlock` signals from `org.freedesktop.login1.Session`. Set `g_tracking_paused` accordingly.
   - Listen to Session Bus `ActiveChanged` signal from `org.freedesktop.ScreenSaver` or `org.gnome.ScreenSaver` interface. Set `g_tracking_paused` accordingly.
   - In `edge-daemon`'s main ingestion loop and background telemetry cycle, skip/ignore telemetry points when `g_tracking_paused` is true.
2. Implement **F42: System Tray Daemon Controller Applet**:
   - Implement `/status` (GET) and `/control` (POST) HTTP endpoints on the daemon's local port 8888 socket server.
   - `/status` returns JSON containing the current running/paused state, epsilon, sensitivity, budget, and cumulative 24h epsilon.
   - `/control` parses action commands (`pause`, `resume`, `configure`) and updates the configuration variables and `g_tracking_paused` state.
   - Write a standalone GTK 3 system tray applet `src/applet.cpp` (compiled as executable `chronos-applet`).
   - The applet must display a tray icon using `GtkStatusIcon`. Right-clicking it displays a popup menu showing current status (Running/Paused/Offline), and options to Pause/Resume tracking, Configure (launches a GTK dialog to update Epsilon and Budget parameters via local POST to `/control`), and Quit the applet.
3. Update `edge-daemon/CMakeLists.txt`:
   - Find packages: `find_package(SQLite3 REQUIRED)` and use PkgConfig to find `glib-2.0`, `gio-2.0`, and `gtk+-3.0`.
   - Link `edge-daemon` with glib-2.0, gio-2.0, sqlite3, and pthread.
   - Build `chronos-applet` target from `src/applet.cpp` linked with gtk+-3.0, glib-2.0, and gio-2.0.
4. Verify compiling and testing: Build targets `edge-daemon`, `chronos-applet`, and `edge-daemon-tests`. Run tests to verify the new endpoints and DBus listener integration.
5. Commit changes incrementally using git as features are verified.
6. Write your handoff report to your working directory as handoff.md containing command logs, build and test outputs, and git commit details. Message the parent with the path to your report.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A Forensic Auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.
