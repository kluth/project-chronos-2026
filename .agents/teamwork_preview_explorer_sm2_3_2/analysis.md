# Technical Analysis: DBus & Applet UI (F36 & F42)

## 1. Executive Summary
This report analyzes the integration of a **DBus System Event Listener (F36)** and a **System Tray Daemon Controller Applet (F42)** for `edge-daemon` in Project Chronos.
Based on the evaluated host environment, we propose a native C/C++ architecture leveraging `libsystemd` (`sd-bus`) for D-Bus IPC/listening and `gtk+-3.0` for a lightweight tray application. This design provides a robust, zero-additional-dependency implementation that integrates directly into the existing C++ project structure.

---

## 2. Host Environment & Dependency Evaluation
We investigated the host system (`aarch64-linux-gnu`) using local package management and build tools.

### 2.1 Package/Library Availability (via `pkg-config` & `dpkg`)
- **System DBus Support (`libsystemd` / `sd-bus`)**: **Available** (`libsystemd` via pkg-config, header `/usr/include/systemd/sd-bus.h`).
- **Low-Level DBus Support (`libdbus-1`)**: **Available** (`dbus-1` via pkg-config, header `/usr/include/dbus-1.0/dbus/dbus.h`).
- **GTK Graphical UI Support (`libgtk-3-dev`)**: **Available** (`gtk+-3.0` via pkg-config).
- **Qt Graphical UI Support (`Qt5Widgets` / `Qt6Widgets`)**: **Not Available** (no pkg-config definitions or headers found under `/usr/include`).
- **Python Bindings (`PyQt5`, `PyQt6`, `PyGObject` / `gi`, `dbus`, `pydbus`)**: **Not Available** (imports fail in the host python3 environment).

### 2.2 IPC & Display Context
- **D-Bus Session Bus**: Address is exported (`DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus`).
- **D-Bus System Bus**: Socket exists and is world-accessible (`/var/run/dbus/system_bus_socket`).
- **GUI Display**: Display environments are active (`DISPLAY=:0` and `WAYLAND_DISPLAY=wayland-0`).

### 2.3 Choice of Technology
1. **Applet UI (F42)**: Due to the lack of Qt and Python GUI libraries, the applet must be written in **C using GTK+-3.0**. This allows compilation using the host's existing `libgtk-3-dev` libraries and produces a lightweight native binary (`edge-daemon-applet`).
2. **DBus Listener (F36)**: Since the daemon is written in C++17, system-level DBus interaction is best implemented via `libsystemd`'s `sd-bus` library. It is modern, declarative, lightweight, and pre-installed on the host.

---

## 3. DBus System Event Listener (F36) Design
The listener must capture OS suspend, resume, and screen-lock events to pause tracking.

### 3.1 Event Sources (systemd-logind)
- **Suspend/Resume**: The systemd-logind daemon exposes a signal `PrepareForSleep(active: b)` on interface `org.freedesktop.login1.Manager` at path `/org/freedesktop/login1` on the **System Bus**.
  - `active = true` indicates the system is entering sleep.
  - `active = false` indicates the system has resumed from sleep.
- **Lock/Unlock**: The interface `org.freedesktop.login1.Session` at session paths (e.g. `/org/freedesktop/login1/session/_31`) broadcasts `Lock` and `Unlock` signals.
  - Using a wildcard or matching without path filters lets us catch lock/unlock events for the current active user session.

### 3.2 Thread-Safe Pause Implementation in `edge-daemon`
We introduce:
1. `std::atomic<bool> g_tracking_paused{false}` to represent the tracking state.
2. `std::mutex g_config_mutex` to protect configuration reads and writes.
3. A background thread `dbusSystemEventListenerThread()` that listens to the system bus for logind signals:
   - Sets `g_tracking_paused = true` on `PrepareForSleep(true)` and `Lock`.
   - Sets `g_tracking_paused = false` on `PrepareForSleep(false)` and `Unlock`.
4. Modified hooks in `edge-daemon` loops:
   - In `backgroundTelemetryThread`: skips process scanning, resource telemetry, and automated backup checks if `g_tracking_paused` is true.
   - In `main()` (socket listener): returns `{"status":"paused"}` and discards incoming ingestion telemetry from the Chrome extension if `g_tracking_paused` is true.

---

## 4. System Tray Daemon Controller Applet (F42) Design
The System Tray Applet acts as a launcher and daemon controller.

### 4.1 Interface Architecture
The applet communicates with `edge-daemon` using a custom interface on the **Session Bus** (User Bus).
- **Service Name**: `org.chronos.EdgeDaemon`
- **Object Path**: `/org/chronos/EdgeDaemon`
- **Interface**: `org.chronos.EdgeDaemon.Controller`

#### D-Bus Interface Methods Exposed by `edge-daemon`:
| Method Name | Input Signatures | Output Signatures | Description |
|-------------|------------------|-------------------|-------------|
| `Pause`     | (none)           | `s`               | Pauses all daemon tracking activities |
| `Resume`    | (none)           | `s`               | Resumes daemon tracking activities |
| `Configure` | `ddd` (eps, sens, budget) | `s`       | Reconfigures daemon parameters thread-safely |
| `GetStatus` | (none)           | `bdddd` (paused, eps, sens, budget, cumulative) | Returns tracking status and current statistics |

### 4.2 GUI Layout & Interaction (GTK-3)
The applet is structured as a resident background process displaying a `GtkStatusIcon`. Right-clicking the icon displays a menu:
1. **Status Info...**: Opens an info dialog displaying:
   - Daemon state (Active or Paused)
   - Current privacy parameters (Epsilon, Sensitivity, Budget)
   - Cumulative 24h privacy budget used (retrieved via `GetStatus` which calls `getCumulativeEpsilon24h` on SQLite database).
2. **Pause Tracking**: Invokes `Pause()` D-Bus method.
3. **Resume Tracking**: Invokes `Resume()` D-Bus method.
4. **Configure...**: Opens a modal settings dialog with entry fields for Epsilon, Sensitivity, and Privacy Budget. Clicking "Save" invokes the `Configure(epsilon, sensitivity, budget)` method.
5. **Quit Applet**: Terminates the tray GUI loop.

---

## 5. CMakeLists.txt and Compilation/Run Blueprint

### 5.1 Updated CMakeLists.txt Configuration
To support compiling the daemon with DBus libraries and compiling the new tray applet, the CMake build configuration is updated using `PkgConfig` to resolve dependencies:
```cmake
cmake_minimum_required(VERSION 3.10)
project(EdgeDaemon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Find Packages
find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)

# Resolve DBus and GTK-3 using pkg-config
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

include_directories(src)

# Core library (DP utilities)
add_library(core_lib src/differential_privacy.cpp)
target_link_libraries(core_lib PUBLIC SQLite::SQLite3)

# Core Edge Daemon (linking with SQLite and systemd's sd-bus)
add_executable(edge-daemon src/main.cpp)
target_link_libraries(edge-daemon core_lib PkgConfig::SYSTEMD)

# System Tray Applet (linking with GTK-3 and systemd's sd-bus)
add_executable(edge-daemon-applet src/applet.c)
target_link_libraries(edge-daemon-applet PkgConfig::GTK3 PkgConfig::SYSTEMD)

enable_testing()
add_executable(edge-daemon-tests tests/differential_privacy_test.cpp)
target_link_libraries(edge-daemon-tests core_lib)
add_test(NAME PropertyBasedDPTests COMMAND edge-daemon-tests)
```

### 5.2 Build & Run Commands
1. **Install Build Dependencies**:
   ```bash
   sudo apt-get update
   sudo apt-get install -y libsystemd-dev libgtk-3-dev pkg-config
   ```
2. **Build**:
   ```bash
   cd edge-daemon
   mkdir -p build
   cd build
   cmake ..
   make
   ```
3. **Run Daemon**:
   ```bash
   ./edge-daemon --epsilon 0.5 --sensitivity 1.0 --budget 10.0
   ```
4. **Run Applet**:
   ```bash
   ./edge-daemon-applet &
   ```

---

## 6. Synthesis Section

### 6.1 Consensus
There is clear alignment that native C/C++ development is the only viable route on the current host. Both the system event listener and the tray controller can run as concurrent background threads in the daemon, exposing and consuming D-Bus interfaces respectively.

### 6.2 Resolved Conflicts
- **C vs. Python for Applet**: Python's PyQt/PyGObject bindings are missing on the host, making a native C-based GTK-3 application the optimal choice to avoid complex Python environment issues.
- **sd-bus vs. libdbus**: We chose `libsystemd`'s `sd-bus` over raw `libdbus` as it provides a clean, modern, and macro-driven declarative API for defining D-Bus objects (via vtables) and is pre-installed on the host.

### 6.3 Dissenting Views
- **Desktop Screensaver Interface**: An alternative considered was listening to desktop-specific interfaces like `org.gnome.ScreenSaver` or `org.freedesktop.ScreenSaver` for lock events. However, because `edge-daemon` runs within the Crostini container, relying on VM-level systemd-logind (`org.freedesktop.login1`) ensures high robustness regardless of the desktop environment running inside or outside the container.

### 6.4 Gaps
- **Crostini Container Boundary**: Although systemd-logind receives sleep signals passed by the hypervisor during host system suspend, locking the parent ChromeOS host screen might not directly trigger a lock signal inside the Crostini VM session. The applet and daemon should support manual toggle as a fallback.
