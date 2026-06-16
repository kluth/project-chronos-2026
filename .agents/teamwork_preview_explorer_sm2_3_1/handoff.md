# Handoff Report — Explorer 1 (SM2.3)

This handoff report summarizes the analysis for implementing the DBus System Event Listener (F36) and the System Tray Daemon Controller Applet (F42).

Detailed analysis and code templates are located in [analysis.md](./analysis.md).

## 1. Observation
- Pre-installed host libraries verified via `dpkg -l` and `pkg-config`:
  - `libdbus-1-dev` (v1.16.2) is installed.
  - `libglib2.0-dev` (v2.84.4) is installed.
  - `libgtk-3-dev` (v3.24.49) is installed.
  - `python3-gi`, `python3-dbus`, and PyQt are NOT installed.
  - Qt development headers (e.g. `qtbase5-dev`) are NOT installed.
- D-Bus signals for system events are verified:
  - `org.freedesktop.login1` on System Bus provides signal `PrepareForSleep` at `/org/freedesktop/login1`.
  - `org.freedesktop.login1.Session` on System Bus provides signals `Lock` and `Unlock` at session object paths (such as `/org/freedesktop/login1/session/_31`).
- GTK status icon capability test-compiled successfully with `g++` and initialized successfully, verifying `GtkStatusIcon` is fully supported on the environment.

## 2. Logic Chain
1. **No External Imports / Offline Restrictions**: Because we are operating in `CODE_ONLY` offline mode, we cannot install Python bindings (PyQt5, PyGObject) or Qt dev packages.
2. **C++ & GTK 3**: Therefore, both the DBus system listener and the tray applet must be implemented in C++ using the pre-installed system dev libraries (`libgtk-3-dev` and `libglib2.0-dev`).
3. **GIO D-Bus (GDBus)**: GIO's D-Bus client (`gio-2.0`) is selected for connecting to the D-Bus system bus and session bus because it handles signals and callbacks cleanly inside GLib threads.
4. **Applet-Daemon IPC**: Using a session-bus-based D-Bus interface `org.chronos.EdgeDaemon` simplifies communication, enabling the controller applet to dynamically query and configure the daemon.

## 3. Caveats
- `GtkStatusIcon` is deprecated in newer GTK 3 (and removed in GTK 4). However, it is fully supported by the installed library version (`3.24.49`) on the host.
- Screen locking signals are session-based. If systemd session boundaries differ in sandboxed profiles, monitoring session lock events may require resolving the exact active user session.

## 4. Conclusion
We propose a fully native, offline-compliant implementation using GTK 3 and GIO DBus:
- **`edge-daemon` extension**: Exposes an IPC object `org.chronos.EdgeDaemon` on the session bus (methods: `Pause`, `Resume`, `Configure`, `GetStatus`) and spawns a thread to listen for system suspend/lock signals from `org.freedesktop.login1`.
- **`chronos-applet` tool**: A GTK 3 tray icon applet that uses the session bus to control and configure the daemon.
- See detailed implementation files in the working directory:
  - D-Bus System Listener: [proposed_dbus_listener.cpp](./proposed_dbus_listener.cpp)
  - D-Bus IPC Server: [proposed_dbus_ipc.cpp](./proposed_dbus_ipc.cpp)
  - Tray Applet UI: [proposed_applet.cpp](./proposed_applet.cpp)
  - CMake Configuration: [proposed_CMakeLists.txt](./proposed_CMakeLists.txt)

## 5. Verification Method
Build and run testing using the following command structure:
```bash
# Set up test build workspace
mkdir -p build_test/src build_test/tests
cp edge-daemon/src/differential_privacy.cpp build_test/src/
cp edge-daemon/src/differential_privacy.h build_test/src/
cp edge-daemon/src/main.cpp build_test/src/
cp edge-daemon/tests/differential_privacy_test.cpp build_test/tests/
cp .agents/teamwork_preview_explorer_sm2_3_1/proposed_dbus_listener.cpp build_test/src/dbus_listener.cpp
cp .agents/teamwork_preview_explorer_sm2_3_1/proposed_dbus_ipc.cpp build_test/src/dbus_ipc.cpp
cp .agents/teamwork_preview_explorer_sm2_3_1/proposed_applet.cpp build_test/src/applet.cpp
cp .agents/teamwork_preview_explorer_sm2_3_1/proposed_CMakeLists.txt build_test/CMakeLists.txt

# Run CMake and build
cd build_test
cmake .
make -j$(nproc)
```
- Invalidation condition: Build errors out or missing headers are encountered.
