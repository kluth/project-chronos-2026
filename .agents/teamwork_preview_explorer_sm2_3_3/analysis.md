# Chronos DBus & Applet UI Implementation Analysis (F36 & F42)

## 1. Executive Summary
This report analyzes and designs the integration of a DBus System Event Listener (F36) and a System Tray Daemon Controller Applet (F42) into the `edge-daemon` subsystem of Project Chronos.
* **DBus Event Listener (F36)**: Captures OS suspend, resume, and screen-lock events using GDBus (GLib/GIO) in a background thread to toggle tracking.
* **System Tray Daemon Controller Applet (F42)**: A standalone GTK 3 desktop application that runs in the user session, communicating with the daemon via local HTTP IPC endpoints (`/status` and `/control`) to pause, resume, or configure parameters.
* **Library Evaluation**: Host scans confirm the presence of `libgtk-3-dev` and `libglib2.0-dev` (with GDBus/GIO), while Qt/PyQt dependencies are absent. Thus, a native C++ GTK 3 approach is recommended.

---

## 2. Host System Dependency Evaluation
A check of the developer environment yielded the following libraries and toolkits registered with `pkg-config` and `dpkg`:

| Library / Tool | Available Version | Status | Suitability / Recommendation |
|---|---|---|---|
| `dbus-1` | `1.16.2` | Installed | Low-level C DBus API. Complex and boilerplate-heavy. |
| `glib-2.0` | `2.84.4` | Installed | Core utilities. |
| `gio-2.0` / `gio-unix-2.0` | `2.84.4` | Installed | Contains **GDBus**, the modern, thread-safe DBus API. Highly recommended. |
| `gtk+-3.0` | `3.24.49` | Installed | Native Linux toolkit. Contains `GtkStatusIcon` for tray support. Highly recommended. |
| Python UI bindings (`gi`, `PyQt5`, `PyQt6`, `PySide2`, `PySide6`) | None | Not Installed | Excludes Python options (PyGObject/PyQt) due to package installation requirements. |
| Qt Dev Libraries (`qtbase5-dev`) | None | Not Installed | Excludes C++/Qt options unless packages are installed. |

### Conclusion on Tech Stack
* **Language**: C++17 (matching `edge-daemon` configuration).
* **DBus Integration**: GDBus (`gio-2.0`). It integrates natively with C++, manages asynchronous messages cleanly, and provides a simple registration/subscription API compared to the raw `libdbus-1` library.
* **Applet GUI**: GTK 3 (`gtk+-3.0`). We will implement `chronos-applet` as a lightweight C++ executable linking with GTK 3.

---

## 3. DBus System Event Listener (F36) Design
The listener will run as a separate background thread in `edge-daemon` to capture the following signals:

1. **System Suspend/Resume**:
   - **Bus**: System Bus
   - **Sender**: `org.freedesktop.login1`
   - **Interface**: `org.freedesktop.login1.Manager`
   - **Path**: `/org/freedesktop/login1`
   - **Signal**: `PrepareForSleep` (signature: `b` - boolean `active`, where `true` is suspend, `false` is resume).
2. **Session Lock/Unlock (Systemd Logind)**:
   - **Bus**: System Bus
   - **Sender**: `org.freedesktop.login1`
   - **Interface**: `org.freedesktop.login1.Session`
   - **Path**: Match globally (`nullptr`) to capture any active session path
   - **Signal**: `Lock` / `Unlock`
3. **Session Lock/Unlock (Desktop Screensaver)**:
   - **Bus**: Session Bus
   - **Interface**: `org.freedesktop.ScreenSaver` or `org.gnome.ScreenSaver`
   - **Path**: `/org/freedesktop/ScreenSaver` or `/org/gnome/ScreenSaver`
   - **Signal**: `ActiveChanged` (signature: `b` - boolean `locked`).

### State Management
Introduce a global atomic state variable in `differential_privacy.h` (and define it in `differential_privacy.cpp`):
```cpp
#include <atomic>
extern std::atomic<bool> g_tracking_paused;
```
Initialize `g_tracking_paused` to `false`.

### GDBus Listener Implementation Draft
```cpp
#include <gio/gio.h>
#include <iostream>
#include <thread>
#include <atomic>
#include "differential_privacy.h"

// Global flag
std::atomic<bool> g_tracking_paused{false};

static void on_prepare_for_sleep(GDBusConnection* connection,
                                 const gchar* sender_name,
                                 const gchar* object_path,
                                 const gchar* interface_name,
                                 const gchar* signal_name,
                                 GVariant* parameters,
                                 gpointer user_data) {
    gboolean active;
    g_variant_get(parameters, "(b)", &active);
    if (active) {
        std::cout << "[DBus Listener] System preparing for sleep. Pausing tracking." << std::endl;
        g_tracking_paused = true;
    } else {
        std::cout << "[DBus Listener] System resumed from sleep. Resuming tracking." << std::endl;
        g_tracking_paused = false;
    }
}

static void on_session_lock(GDBusConnection* connection,
                            const gchar* sender_name,
                            const gchar* object_path,
                            const gchar* interface_name,
                            const gchar* signal_name,
                            GVariant* parameters,
                            gpointer user_data) {
    std::cout << "[DBus Listener] Session locked. Pausing tracking." << std::endl;
    g_tracking_paused = true;
}

static void on_session_unlock(GDBusConnection* connection,
                              const gchar* sender_name,
                              const gchar* object_path,
                              const gchar* interface_name,
                              const gchar* signal_name,
                              GVariant* parameters,
                              gpointer user_data) {
    std::cout << "[DBus Listener] Session unlocked. Resuming tracking." << std::endl;
    g_tracking_paused = false;
}

static void on_screensaver_active_changed(GDBusConnection* connection,
                                          const gchar* sender_name,
                                          const gchar* object_path,
                                          const gchar* interface_name,
                                          const gchar* signal_name,
                                          GVariant* parameters,
                                          gpointer user_data) {
    gboolean active;
    g_variant_get(parameters, "(b)", &active);
    if (active) {
        std::cout << "[DBus Listener] Screensaver locked. Pausing tracking." << std::endl;
        g_tracking_paused = true;
    } else {
        std::cout << "[DBus Listener] Screensaver unlocked. Resuming tracking." << std::endl;
        g_tracking_paused = false;
    }
}

void dbusListenerThread() {
    GMainContext* context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GMainLoop* loop = g_main_loop_new(context, FALSE);

    GError* error = nullptr;
    
    // Connect to System Bus (for Systemd PrepareForSleep and Session lock)
    GDBusConnection* system_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (system_conn) {
        g_dbus_connection_signal_subscribe(
            system_conn, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
            "PrepareForSleep", "/org/freedesktop/login1", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, on_prepare_for_sleep, nullptr, nullptr
        );
        g_dbus_connection_signal_subscribe(
            system_conn, "org.freedesktop.login1", "org.freedesktop.login1.Session",
            "Lock", nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_session_lock, nullptr, nullptr
        );
        g_dbus_connection_signal_subscribe(
            system_conn, "org.freedesktop.login1", "org.freedesktop.login1.Session",
            "Unlock", nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_session_unlock, nullptr, nullptr
        );
    } else {
        std::cerr << "[DBus Listener] System Bus connect failed: " << error->message << std::endl;
        g_clear_error(&error);
    }

    // Connect to Session Bus (for Desktop Screensaver ActiveChanged)
    GDBusConnection* session_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (session_conn) {
        g_dbus_connection_signal_subscribe(
            session_conn, nullptr, "org.freedesktop.ScreenSaver",
            "ActiveChanged", "/org/freedesktop/ScreenSaver", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, on_screensaver_active_changed, nullptr, nullptr
        );
        g_dbus_connection_signal_subscribe(
            session_conn, nullptr, "org.gnome.ScreenSaver",
            "ActiveChanged", "/org/gnome/ScreenSaver", nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, on_screensaver_active_changed, nullptr, nullptr
        );
    } else {
        std::cerr << "[DBus Listener] Session Bus connect failed: " << error->message << std::endl;
        g_clear_error(&error);
    }

    g_main_loop_run(loop);

    if (system_conn) g_object_unref(system_conn);
    if (session_conn) g_object_unref(session_conn);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
}
```

### Integration in `edge-daemon` Main Loops
1. **Telemetry & Scanning Loop (`backgroundTelemetryThread` in `src/main.cpp`)**:
   We will insert a check for `g_tracking_paused` at the start of the loop cycle (line 30 onwards):
   ```cpp
   while (true) {
       std::this_thread::sleep_for(std::chrono::seconds(10));
       if (g_tracking_paused) {
           std::cout << "[Background Monitor] Tracking is paused. Skipping telemetry aggregation." << std::endl;
           continue;
       }
       // Process Scanning, Resource Telemetry and SQLite Buffer flushes...
   }
   ```
2. **ChromeOS Extension Ingestion Socket Handler (`main.cpp`)**:
   Within `main.cpp`'s socket polling loop (around line 261):
   ```cpp
   if (!active_window.empty()) {
       if (g_tracking_paused) {
           std::cout << "[Edge Daemon] Ingestion paused. Skipping window event: " << active_window << std::endl;
           continue;
       }
       TelemetryEvent event = { active_window, 10.0 };
       ...
   }
   ```

---

## 4. Applet-Daemon IPC Protocol & Interface
To control daemon state (pause, resume, configure) from the GTK Applet, the daemon's local port `8888` server can be expanded to support three endpoints:

1. **`POST /control`**: Used to send commands.
   - **Request payload**:
     - `{"action": "pause"}`
     - `{"action": "resume"}`
     - `{"action": "configure", "epsilon": 0.4, "budget": 12.0}`
   - **Response**: `{"status": "ok"}` or `{"status": "error", "message": "..."}`
2. **`GET /status`**: Returns current configurations.
   - **Response**:
     ```json
     {
       "status": "running", // or "paused"
       "epsilon": 0.5,
       "sensitivity": 1.0,
       "budget": 15.0,
       "cumulative_epsilon_24h": 1.2
     }
     ```
3. **`POST /ingest`** (or raw parsing as it currently does): The existing ingestion flow.

### Daemon Socket Server Upgrades
Modify the socket parsing loop in `main.cpp` to look for headers and endpoints:
```cpp
while (true) {
    int new_socket = accept(server_fd, nullptr, nullptr);
    if (new_socket < 0) continue;

    char buffer[2048] = {0};
    read(new_socket, buffer, 2048);
    std::string request(buffer);

    // Basic routing
    if (request.find("GET /status") != std::string::npos) {
        double cumulative = getCumulativeEpsilon24h(DB_PATH);
        std::string status_str = g_tracking_paused ? "paused" : "running";
        std::string json_res = "{\"status\":\"" + status_str + "\""
                             + ",\"epsilon\":" + std::to_string(g_config.epsilon)
                             + ",\"sensitivity\":" + std::to_string(g_config.sensitivity)
                             + ",\"budget\":" + std::to_string(g_config.budget)
                             + ",\"cumulative_epsilon_24h\":" + std::to_string(cumulative) + "}";
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: "
                             + std::to_string(json_res.length()) + "\r\n\r\n" + json_res;
        send(new_socket, response.c_str(), response.length(), 0);
        close(new_socket);
        continue;
    }
    
    if (request.find("POST /control") != std::string::npos) {
        std::string json_body = "";
        size_t body_pos = request.find("\r\n\r\n");
        if (body_pos != std::string::npos) {
            json_body = request.substr(body_pos + 4);
        }

        std::string res_msg = "{\"status\":\"ok\"}";
        if (json_body.find("\"action\":\"pause\"") != std::string::npos) {
            g_tracking_paused = true;
            std::cout << "[Control API] Pause command received." << std::endl;
        } else if (json_body.find("\"action\":\"resume\"") != std::string::npos) {
            g_tracking_paused = false;
            std::cout << "[Control API] Resume command received." << std::endl;
        } else if (json_body.find("\"action\":\"configure\"") != std::string::npos) {
            // Very simple JSON parsing for parameters
            double eps_val = g_config.epsilon;
            double bud_val = g_config.budget;
            
            size_t eps_pos = json_body.find("\"epsilon\":");
            if (eps_pos != std::string::npos) {
                eps_val = std::stod(json_body.substr(eps_pos + 10));
            }
            size_t bud_pos = json_body.find("\"budget\":");
            if (bud_pos != std::string::npos) {
                bud_val = std::stod(json_body.substr(bud_pos + 9));
            }

            g_config.epsilon = eps_val;
            g_config.budget = bud_val;
            std::cout << "[Control API] Reconfigured: Epsilon=" << eps_val << ", Budget=" << bud_val << std::endl;
        } else {
            res_msg = "{\"status\":\"error\",\"message\":\"invalid command\"}";
        }

        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: "
                             + std::to_string(res_msg.length()) + "\r\n\r\n" + res_msg;
        send(new_socket, response.c_str(), response.length(), 0);
        close(new_socket);
        continue;
    }

    // Default Ingestion Flow (existing code)
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"ok\"}";
    send(new_socket, response.c_str(), response.length(), 0);
    close(new_socket);
    
    // Ingest logic...
}
```

---

## 5. System Tray Daemon Controller Applet (F42) Design
A standalone binary `chronos-applet` written in C++ using GTK 3.

### UI Architecture
1. **Tray Integration**: Creates a `GtkStatusIcon` (which handles tray integration natively in GTK 3).
2. **Context Menu**: Triggered on right-click, providing:
   - **Status**: Displaying "Status: Running" or "Status: Paused".
   - **Pause / Resume**: Toggle tracking.
   - **Configure...**: Launch Settings Dialog.
   - **Quit**: Exit the applet.
3. **Settings Dialog**:
   - Contains fields for **Epsilon** and **Privacy Budget**.
   - When "Apply" is clicked, it makes an HTTP request to `http://localhost:8888/control` reconfiguring the daemon.

### Applet Source Code Draft (`edge-daemon/src/applet.cpp`)
```cpp
#include <gtk/gtk.h>
#include <iostream>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static GtkStatusIcon *status_icon;
static GtkWidget *menu_status_item;
static GtkWidget *menu_toggle_item;

// Simple TCP POST sender helper
bool sendPostRequest(const std::string& path, const std::string& json_body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    std::string req = "POST " + path + " HTTP/1.1\r\n"
                    + "Host: localhost:8888\r\n"
                    + "Content-Type: application/json\r\n"
                    + "Content-Length: " + std::to_string(json_body.length()) + "\r\n"
                    + "Connection: close\r\n\r\n"
                    + json_body;

    send(sock, req.c_str(), req.length(), 0);
    close(sock);
    return true;
}

// Queries current status from daemon
std::string getDaemonStatus() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "Offline";
    }

    std::string req = "GET /status HTTP/1.1\r\nHost: localhost:8888\r\nConnection: close\r\n\r\n";
    send(sock, req.c_str(), req.length(), 0);

    char buf[1024];
    std::string resp = "";
    int len;
    while ((len = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[len] = '\0';
        resp += buf;
    }
    close(sock);

    size_t body_pos = resp.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
        return resp.substr(body_pos + 4);
    }
    return "";
}

// Dialog for configuration parameters
static void on_configure_clicked(GtkWidget *widget, gpointer data) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Chronos Configurations",
        nullptr,
        GTK_DIALOG_MODAL,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Apply", GTK_RESPONSE_ACCEPT,
        nullptr
    );
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 15);

    GtkWidget *lbl_eps = gtk_label_new("Epsilon (Privacy Noise):");
    GtkWidget *ent_eps = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ent_eps), "0.5");

    GtkWidget *lbl_bud = gtk_label_new("Privacy Budget (24h Limit):");
    GtkWidget *ent_bud = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ent_bud), "15.0");

    gtk_grid_attach(GTK_GRID(grid), lbl_eps, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_eps, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_bud, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_bud, 1, 1, 1, 1);

    // Pre-populate with current config
    std::string status_json = getDaemonStatus();
    if (!status_json.empty() && status_json != "Offline") {
        size_t eps_idx = status_json.find("\"epsilon\":");
        size_t bud_idx = status_json.find("\"budget\":");
        if (eps_idx != std::string::npos) {
            std::string cur_eps = status_json.substr(eps_idx + 10, status_json.find(",", eps_idx) - (eps_idx + 10));
            gtk_entry_set_text(GTK_ENTRY(ent_eps), cur_eps.c_str());
        }
        if (bud_idx != std::string::npos) {
            std::string cur_bud = status_json.substr(bud_idx + 9, status_json.find("}", bud_idx) - (bud_idx + 9));
            gtk_entry_set_text(GTK_ENTRY(ent_bud), cur_bud.c_str());
        }
    }

    gtk_container_add(GTK_CONTAINER(content_area), grid);
    gtk_widget_show_all(dialog);

    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    if (result == GTK_RESPONSE_ACCEPT) {
        std::string eps = gtk_entry_get_text(GTK_ENTRY(ent_eps));
        std::string bud = gtk_entry_get_text(GTK_ENTRY(ent_bud));
        
        std::string payload = "{\"action\":\"configure\",\"epsilon\":" + eps + ",\"budget\":" + bud + "}";
        sendPostRequest("/control", payload);
    }
    gtk_widget_destroy(dialog);
}

// Toggle Pause/Resume handler
static void on_toggle_clicked(GtkWidget *widget, gpointer data) {
    std::string status_json = getDaemonStatus();
    bool is_paused = (status_json.find("\"status\":\"paused\"") != std::string::npos);
    
    std::string payload = is_paused ? "{\"action\":\"resume\"}" : "{\"action\":\"pause\"}";
    if (sendPostRequest("/control", payload)) {
        std::cout << "[Applet UI] Dispatched action toggle." << std::endl;
    }
}

// Tray icon context menu populator
static void on_tray_popup(GtkStatusIcon *icon, guint button, guint activate_time, gpointer user_data) {
    GtkWidget *menu = gtk_menu_new();

    std::string status_json = getDaemonStatus();
    bool is_paused = (status_json.find("\"status\":\"paused\"") != std::string::npos);
    bool is_offline = (status_json == "Offline");

    std::string status_text = "Status: Running";
    if (is_offline) status_text = "Status: Offline (Daemon Stop)";
    else if (is_paused) status_text = "Status: Paused";

    menu_status_item = gtk_menu_item_new_with_label(status_text.c_str());
    gtk_widget_set_sensitive(menu_status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_status_item);

    // Separator
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    if (!is_offline) {
        menu_toggle_item = gtk_menu_item_new_with_label(is_paused ? "Resume Tracking" : "Pause Tracking");
        g_signal_connect(menu_toggle_item, "activate", G_CALLBACK(on_toggle_clicked), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_toggle_item);

        GtkWidget *config_item = gtk_menu_item_new_with_label("Configure Epsilon...");
        g_signal_connect(config_item, "activate", G_CALLBACK(on_configure_clicked), nullptr);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), config_item);
    }

    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit Applet");
    g_signal_connect(quit_item, "activate", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), nullptr);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    // Create system tray icon. Use a stock or specific icon name.
    status_icon = gtk_status_icon_new_from_icon_name("media-playback-pause");
    gtk_status_icon_set_tooltip_text(status_icon, "Chronos Daemon Controller");
    g_signal_connect(status_icon, "popup-menu", G_CALLBACK(on_tray_popup), nullptr);

    gtk_main();
    return 0;
}
```

---

## 6. Proposed CMakeLists.txt Updates
To build both features, the `edge-daemon/CMakeLists.txt` file needs to check for `glib-2.0`, `gio-2.0`, and `gtk+-3.0` using `pkg-config` and define two build targets:
1. `edge-daemon` (which includes DBus Listener threaded routine linked against GLib/GIO).
2. `chronos-applet` (which builds the GTK System Tray Applet).

### Proposed `CMakeLists.txt`
```cmake
cmake_minimum_required(VERSION 3.10)
project(EdgeDaemon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Find Package Dependencies
find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)

# Check for GLib, GIO, and GTK 3
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

include_directories(src)

# Core library with differential privacy and utilities
add_library(core_lib src/differential_privacy.cpp)
target_link_libraries(core_lib PUBLIC SQLite::SQLite3)

# 1. Edge Daemon target (includes main loop and DBus Listener)
# Make sure to compile src/main.cpp with GLib/GIO headers
add_executable(edge-daemon src/main.cpp)
target_link_libraries(edge-daemon 
    core_lib 
    PkgConfig::GLIB 
    PkgConfig::GIO 
    pthread
)

# 2. System Tray Applet target
add_executable(chronos-applet src/applet.cpp)
target_link_libraries(chronos-applet 
    PkgConfig::GTK3 
    PkgConfig::GLIB 
    PkgConfig::GIO
)

# Unit Testing suite
enable_testing()
add_executable(edge-daemon-tests tests/differential_privacy_test.cpp)
target_link_libraries(edge-daemon-tests core_lib)
add_test(NAME PropertyBasedDPTests COMMAND edge-daemon-tests)
```

---

## 7. Verification & Mocking Guide

### Command Line Verification (Mocking DBus Signals)
To verify the daemon correctly pauses and resumes in response to DBus events without needing to actually put the host OS to sleep, you can emit mock DBus signals using `gdbus` or `dbus-send`.

1. **Test Suspend Signal**:
   ```bash
   gdbus emit --system --to-object /org/freedesktop/login1 \
     --signal org.freedesktop.login1.Manager.PrepareForSleep true
   ```
   *Expected outcome*: `edge-daemon` logs `[DBus Listener] System preparing for sleep. Pausing tracking.` and rejects/ignores telemetry inputs.

2. **Test Resume Signal**:
   ```bash
   gdbus emit --system --to-object /org/freedesktop/login1 \
     --signal org.freedesktop.login1.Manager.PrepareForSleep false
   ```
   *Expected outcome*: `edge-daemon` logs `[DBus Listener] System resumed from sleep. Resuming tracking.` and restarts telemetry ingestion.

3. **Test Session Screen Lock**:
   ```bash
   # Systemd Lock (matching path-independent subscription)
   gdbus emit --system --to-object /org/freedesktop/login1/session/self \
     --signal org.freedesktop.login1.Session.Lock
   ```
   *Expected outcome*: `edge-daemon` logs `[DBus Listener] Session locked. Pausing tracking.`

4. **Test Session Screen Unlock**:
   ```bash
   gdbus emit --system --to-object /org/freedesktop/login1/session/self \
     --signal org.freedesktop.login1.Session.Unlock
   ```
   *Expected outcome*: `edge-daemon` logs `[DBus Listener] Session unlocked. Resuming tracking.`

5. **Test Screensaver ActiveChanged (Session Bus)**:
   ```bash
   gdbus emit --session --to-object /org/freedesktop/ScreenSaver \
     --signal org.freedesktop.ScreenSaver.ActiveChanged true
   ```
   *Expected outcome*: `edge-daemon` logs `[DBus Listener] Screensaver locked. Pausing tracking.`

### Applet Controller Verification
1. Start `edge-daemon`.
2. Start `chronos-applet` in a GUI environment.
3. Right-click the applet tray icon:
   * Verify status reads "Status: Running".
   * Click "Pause Tracking". Right-click again and verify status changes to "Status: Paused". Check `edge-daemon` console output for command receipt confirmation.
   * Click "Configure Epsilon...". Set Epsilon to `1.2` and Budget to `20.0`. Click "Apply".
4. Query `/status` endpoint manually to confirm change:
   ```bash
   curl http://localhost:8888/status
   ```
   Verify response is `{"status":"paused","epsilon":1.200000,"sensitivity":1.000000,"budget":20.000000,...}`.
