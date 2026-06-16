# Sub-Milestone 2.3 (DBus & Applet UI) Analysis Report

## Summary of Findings
1. **D-Bus System Event Listener (F36)**: Modern Linux system suspend, resume, and screen-lock events are exposed on the System D-Bus via `org.freedesktop.login1`. We can capture these by connecting to the System Bus and subscribing to:
   - `PrepareForSleep` signal on interface `org.freedesktop.login1.Manager` at object `/org/freedesktop/login1` (for suspend/resume).
   - `Lock` and `Unlock` signals on interface `org.freedesktop.login1.Session` on any session object path (for screen-lock).
2. **System Tray Daemon Controller Applet (F42)**: A lightweight, desktop-native tray controller can be implemented in C++ using `gtk+-3.0` (via `GtkStatusIcon`) and `gio-2.0` (for D-Bus Session Bus IPC). It communicates with `edge-daemon` over a custom D-Bus interface `org.chronos.EdgeDaemon` on the Session D-Bus to toggle tracking state, apply parameters dynamically, and trigger service restarts.
3. **Library Evaluation & Network Constraints**: Under `CODE_ONLY` offline mode, external packaging or downloads (like PyQt, PyGObject, or Qt dev headers) are unavailable. However, the host system contains pre-installed `libdbus-1-dev`, `libglib2.0-dev`, and `libgtk-3-dev`. Hence, a native C++ implementation utilizing GTK 3 and GLib/GIO is the most robust, self-contained solution.

---

## 1. Observations

### A. Host Development Libraries and Package Capabilities
Querying installed packages on the Debian-based Linux host environment:
* **Tool Command**: `dpkg -l | grep -E "libdbus-1-dev|libgtk-3-dev|libglib2.0-dev|python3-pyqt5|python3-gi|qtbase5-dev|qt5-default|libdbus"`
* **Output**:
```
ii  libdbus-1-3:arm64                    1.16.2-2                             arm64        simple interprocess messaging system (library)
ii  libdbus-1-dev:arm64                  1.16.2-2                             arm64        simple interprocess messaging system (development headers)
ii  libdbus-glib-1-2:arm64               0.114-1                              arm64        deprecated library for D-Bus IPC
ii  libglib2.0-dev:arm64                 2.84.4-3~deb13u3                     arm64        Development metapackage for the GLib family of libraries
ii  libglib2.0-dev-bin                   2.84.4-3~deb13u3                     arm64        Development utilities for the GLib library
ii  libgtk-3-dev:arm64                   3.24.49-3                            arm64        development files for the GTK library
```

* **Tool Command**: `pkg-config --list-all | grep -E "dbus|gtk"`
* **Output**:
```
dbus-1                         dbus - Free desktop message bus
gtk+-3.0                       GTK+ - GTK+ Graphical UI Library
```

* **Tool Command**: `pkg-config --modversion glib-2.0 dbus-1 gtk+-3.0`
* **Output**:
```
2.84.4
1.16.2
3.24.49
```

* **Python and Qt Packages**:
  Python 3.13.5 is installed, but `python3-gi` (PyGObject), `python3-dbus`, and `python3-pyqt5` / `python3-pyqt6` are **not** installed. Additionally, standard runtime Qt 5 modules are installed, but development headers and build tooling (such as `qtbase5-dev` or `qmake`) are missing.

### B. Verification of GTK status icon capability
We compiled a temporary GTK test executable referencing `GtkStatusIcon`:
* **Compile Command**: `g++ temp_gtk_test/test_gtk.cpp -o temp_gtk_test/test_gtk $(pkg-config --cflags --libs gtk+-3.0)`
* **Output**: Successfully compiled and linked.
* **Execution Output**:
```
GTK Initialized successfully!
GtkStatusIcon is supported and created!
```

### C. Active D-Bus Services on Host
* **System Bus Check**: Introspecting the `org.freedesktop.login1` service on the system bus reveals the presence of:
  - Signal: `PrepareForSleep` (signature: `b` for boolean, `true` = suspend, `false` = resume).
  - Method: `LockSession` / `UnlockSession` and session tracking.
* **Session Bus Session Object Check**: Introspecting the session object `/org/freedesktop/login1/session/_31` reveals interface `org.freedesktop.login1.Session` supporting:
  - Signals: `Lock` and `Unlock` (no arguments, indicating lock/unlock actions).

---

## 2. Logic Chain

1. **Language & Framework Selection**: 
   - Since Python GUI bindings (`PyQt5`, `gi` / PyGObject) are not installed, and the `CODE_ONLY` constraint prevents downloading packages online, Python is rejected for implementing the applet.
   - Since `qtbase5-dev` (Qt C++ development package) is missing and `libgtk-3-dev` is fully installed, writing the applet as a C++ program linking with `gtk+-3.0` is the only viable path.
   - The status icon test-compilation verified that GTK 3 is functional and `GtkStatusIcon` is fully supported on the host.

2. **D-Bus Listening Mechanism (F36)**:
   - `edge-daemon` requires listening to OS system events.
   - Connecting to the system bus and listening for signal `PrepareForSleep` on interface `org.freedesktop.login1.Manager` at path `/org/freedesktop/login1` will successfully notify the daemon of OS suspend/resume events.
   - Listening for `Lock` and `Unlock` signals on interface `org.freedesktop.login1.Session` across any session object path (using a wildcard path subscription) captures OS screen lock/unlock events.
   - A dedicated D-Bus listener thread running inside `edge-daemon` can update a global thread-safe flag `g_tracking_paused` upon receiving these signals, allowing `backgroundTelemetryThread` and the host socket ingest logic in `main.cpp` to pause.

3. **IPC for Daemon and Applet (F42)**:
   - The system tray controller applet needs to query daemon status and trigger config updates.
   - Exposing a D-Bus IPC service `org.chronos.EdgeDaemon` on the Session D-Bus from `edge-daemon` allows the applet to cleanly invoke methods (`Pause`, `Resume`, `Configure`, `GetStatus`).
   - Using GIO's D-Bus API (`gio-2.0`) is highly recommended over raw `libdbus-1` as it is cleaner, safer, object-oriented, and integrates seamlessly with the GLib main context loops.

---

## 3. Caveats

* **Wayland & X11 Tray Support**: `GtkStatusIcon` relies on X11 tray protocols or System Tray extensions. In pure Wayland environments without XWayland or status notifier support, system trays can fail to render. However, the Crostini wrapper environment is running a compatible window manager layer where `GtkStatusIcon` successfully initialized in our test.
* **Session Lock under container**: In containerized or sandboxed systems, session lock signals from `systemd-logind` might not propagate inside the container unless the host D-Bus system socket is shared. Crostini propagates system DBus notifications standardly, but if not, an alternative falls back to monitoring the desktop session lock using specific user-session-level desktop saver interfaces.
* **GLib Linking**: Linking `edge-daemon` to GLib/GIO changes its runtime dependency profile. If a minimal static binary was desired, raw `libdbus` should be used instead of GLib (we have provided raw D-Bus examples as reference).

---

## 4. Conclusion

The proposed implementation architecture is clean, highly integrated, and compliant with all project constraints:
1. **D-Bus Event Listener Thread in Daemon**: Add `dbus_listener.cpp` using GIO to hook into the `system` D-Bus. It controls a global atomic `g_tracking_paused` flag.
2. **D-Bus IPC Service in Daemon**: Add `dbus_ipc.cpp` registering `/org/chronos/EdgeDaemon` on the `session` D-Bus. It exposes methods to read/write daemon parameters.
3. **GTK 3 Controller Applet**: A standalone executable `chronos-applet` (`applet.cpp`) providing status visibility, configuration, and daemon restart functions.
4. **CMake Integration**: Expand `CMakeLists.txt` using `PkgConfig` to locate and link `glib-2.0`, `gio-2.0`, `gtk+-3.0`, and `dbus-1`.

Detailed proposed files are generated in the working directory:
* [proposed_dbus_listener.cpp](./proposed_dbus_listener.cpp) — F36 System Suspend & Screen Lock listener.
* [proposed_dbus_ipc.cpp](./proposed_dbus_ipc.cpp) — Daemon D-Bus Session IPC Server.
* [proposed_applet.cpp](./proposed_applet.cpp) — F42 GTK 3 System Tray GUI applet.
* [proposed_CMakeLists.txt](./proposed_CMakeLists.txt) — CMake configuration.

---

## 5. Verification Method

To independently verify the compilation and runtime behavior:

### A. Compilation Check
Run the following compilation commands from `/home/matthias/project/project-chronos`:
```bash
# 1. Create a temporary source tree for testing CMake integration
mkdir -p build_test/src build_test/tests
cp edge-daemon/src/differential_privacy.cpp build_test/src/
cp edge-daemon/src/differential_privacy.h build_test/src/
cp edge-daemon/src/main.cpp build_test/src/
cp edge-daemon/tests/differential_privacy_test.cpp build_test/tests/

# Copy the proposed files to the test tree
cp .agents/teamwork_preview_explorer_sm2_3_1/proposed_dbus_listener.cpp build_test/src/dbus_listener.cpp
cp .agents/teamwork_preview_explorer_sm2_3_1/proposed_dbus_ipc.cpp build_test/src/dbus_ipc.cpp
cp .agents/teamwork_preview_explorer_sm2_3_1/proposed_applet.cpp build_test/src/applet.cpp
cp .agents/teamwork_preview_explorer_sm2_3_1/proposed_CMakeLists.txt build_test/CMakeLists.txt

# 2. Build using CMake
cd build_test
cmake .
make -j$(nproc)
```
*Invalidation conditions*: The build fails or throws undefined reference errors during linking.

### B. Functional Verification of D-Bus Signals
Run `dbus-monitor` to verify that signals are dispatched when suspending or locking:
* **System Bus Monitor (Suspend)**:
  `dbus-monitor --system "type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'"`
* **Session Bus Monitor (Lock)**:
  `dbus-monitor --system "type='signal',interface='org.freedesktop.login1.Session'"`

### C. Manual IPC Verification
Use `dbus-send` or `busctl` to mock applet interactions with the daemon:
* **Check Status**:
  `busctl --user call org.chronos.EdgeDaemon /org/chronos/EdgeDaemon org.chronos.EdgeDaemon GetStatus`
* **Pause Ingestion**:
  `busctl --user call org.chronos.EdgeDaemon /org/chronos/EdgeDaemon org.chronos.EdgeDaemon Pause`
* **Resume Ingestion**:
  `busctl --user call org.chronos.EdgeDaemon /org/chronos/EdgeDaemon org.chronos.EdgeDaemon Resume`
* **Configure Parameters**:
  `busctl --user call org.chronos.EdgeDaemon /org/chronos/EdgeDaemon org.chronos.EdgeDaemon Configure ddd 1.5 0.2 15.0`
