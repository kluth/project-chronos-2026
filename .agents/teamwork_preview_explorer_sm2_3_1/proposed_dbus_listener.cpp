#include <gio/gio.h>
#include <atomic>
#include <iostream>

// Thread-safe flag to control daemon tracking state
extern std::atomic<bool> g_tracking_paused;

// Signal handler for D-Bus notifications on the System Bus
static void on_system_dbus_signal(GDBusConnection* connection,
                                  const gchar* sender_name,
                                  const gchar* object_path,
                                  const gchar* interface_name,
                                  const gchar* signal_name,
                                  GVariant* parameters,
                                  gpointer user_data) {
    std::string sig(signal_name);
    std::string iface(interface_name);

    if (iface == "org.freedesktop.login1.Manager" && sig == "PrepareForSleep") {
        gboolean going_to_sleep;
        g_variant_get(parameters, "(b)", &going_to_sleep);
        if (going_to_sleep) {
            std::cout << "[DBus Listener] OS preparing to suspend. Pausing edge tracking." << std::endl;
            g_tracking_paused.store(true);
        } else {
            std::cout << "[DBus Listener] OS resumed from suspend. Resuming edge tracking." << std::endl;
            g_tracking_paused.store(false);
        }
    } 
    else if (iface == "org.freedesktop.login1.Session") {
        if (sig == "Lock") {
            std::cout << "[DBus Listener] OS Session locked. Pausing edge tracking." << std::endl;
            g_tracking_paused.store(true);
        } else if (sig == "Unlock") {
            std::cout << "[DBus Listener] OS Session unlocked. Resuming edge tracking." << std::endl;
            g_tracking_paused.store(false);
        }
    }
}

// Thread entry point for monitoring DBus system events
void dbusListenerThread() {
    GError* error = nullptr;
    
    // Connect to the System D-Bus
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (error) {
        std::cerr << "[DBus Listener] Failed to connect to system D-Bus: " << error->message << std::endl;
        g_error_free(error);
        return;
    }

    std::cout << "[DBus Listener] Connected to system D-Bus. Registering signal listeners..." << std::endl;

    // 1. Subscribe to system PrepareForSleep (Suspend/Resume) signal
    g_dbus_connection_signal_subscribe(
        conn,
        "org.freedesktop.login1",           // Sender service
        "org.freedesktop.login1.Manager",   // Interface name
        "PrepareForSleep",                  // Member name
        "/org/freedesktop/login1",          // Object path
        nullptr,                            // arg0
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_system_dbus_signal,
        nullptr, nullptr
    );

    // 2. Subscribe to Lock signal (Session lock) from any session path
    g_dbus_connection_signal_subscribe(
        conn,
        "org.freedesktop.login1",           // Sender service
        "org.freedesktop.login1.Session",   // Interface name
        "Lock",                             // Member name
        nullptr,                            // Object path (match all session objects)
        nullptr,                            // arg0
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_system_dbus_signal,
        nullptr, nullptr
    );

    // 3. Subscribe to Unlock signal (Session unlock) from any session path
    g_dbus_connection_signal_subscribe(
        conn,
        "org.freedesktop.login1",           // Sender service
        "org.freedesktop.login1.Session",   // Interface name
        "Unlock",                           // Member name
        nullptr,                            // Object path (match all session objects)
        nullptr,                            // arg0
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_system_dbus_signal,
        nullptr, nullptr
    );

    // Run the GLib context main loop to dispatch signals in this thread
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
    
    // Cleanup (normally unreachable)
    g_main_loop_unref(loop);
    g_object_unref(conn);
}
