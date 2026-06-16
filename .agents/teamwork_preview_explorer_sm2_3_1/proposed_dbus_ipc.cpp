#include <gio/gio.h>
#include <atomic>
#include <iostream>
#include <string>

// External configurations and status
extern std::atomic<bool> g_tracking_paused;

struct Config {
    double epsilon = 1.0;
    double sensitivity = 0.1;
    double budget = 10.0;
    std::string secret = "";
};
extern Config g_config;

// XML specification for DBus interface introspection
static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.chronos.EdgeDaemon'>"
  "    <method name='Pause'/>"
  "    <method name='Resume'/>"
  "    <method name='Configure'>"
  "      <arg type='d' name='epsilon' direction='in'/>"
  "      <arg type='d' name='sensitivity' direction='in'/>"
  "      <arg type='d' name='budget' direction='in'/>"
  "    </method>"
  "    <method name='GetStatus'>"
  "      <arg type='b' name='paused' direction='out'/>"
  "      <arg type='d' name='epsilon' direction='out'/>"
  "      <arg type='d' name='sensitivity' direction='out'/>"
  "      <arg type='d' name='budget' direction='out'/>"
  "    </method>"
  "  </interface>"
  "</node>";

// Handler for incoming method calls from the System Tray Applet
static void handle_method_call(GDBusConnection* connection,
                               const gchar* sender,
                               const gchar* object_path,
                               const gchar* interface_name,
                               const gchar* method_name,
                               GVariant* parameters,
                               GDBusMethodInvocation* invocation,
                               gpointer user_data) {
    std::string method(method_name);

    if (method == "Pause") {
        g_tracking_paused.store(true);
        std::cout << "[IPC Server] Received PAUSE command via D-Bus Session Bus." << std::endl;
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } 
    else if (method == "Resume") {
        g_tracking_paused.store(false);
        std::cout << "[IPC Server] Received RESUME command via D-Bus Session Bus." << std::endl;
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } 
    else if (method == "Configure") {
        double eps, sens, budget;
        g_variant_get(parameters, "(ddd)", &eps, &sens, &budget);
        
        g_config.epsilon = eps;
        g_config.sensitivity = sens;
        g_config.budget = budget;
        
        std::cout << "[IPC Server] Reconfigured: Epsilon=" << eps 
                  << ", Sensitivity=" << sens 
                  << ", Budget=" << budget << std::endl;
                  
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } 
    else if (method == "GetStatus") {
        gboolean paused = g_tracking_paused.load();
        GVariant* reply = g_variant_new("(bddd)", paused, g_config.epsilon, g_config.sensitivity, g_config.budget);
        g_dbus_method_invocation_return_value(invocation, reply);
    }
}

// Method table for D-Bus object registration
static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    nullptr, // get_property (not used)
    nullptr  // set_property (not used)
};

// Callback when D-Bus session bus is acquired and ready
static void on_bus_acquired(GDBusConnection* connection,
                            const gchar* name,
                            gpointer user_data) {
    GError* error = nullptr;
    GDBusNodeInfo* introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (error) {
        std::cerr << "[IPC Server] Introspection XML parser failed: " << error->message << std::endl;
        g_error_free(error);
        return;
    }

    g_dbus_connection_register_object(
        connection,
        "/org/chronos/EdgeDaemon",
        introspection_data->interfaces[0],
        &interface_vtable,
        nullptr, // user_data
        nullptr, // user_data_free_func
        &error
    );

    if (error) {
        std::cerr << "[IPC Server] Failed to register D-Bus Object: " << error->message << std::endl;
        g_error_free(error);
    } else {
        std::cout << "[IPC Server] Registered D-Bus Object at /org/chronos/EdgeDaemon" << std::endl;
    }
}

// Thread entry point for edge-daemon IPC server
void dbusIpcServerThread() {
    // Claim the well-known name org.chronos.EdgeDaemon on the Session Bus
    guint owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        "org.chronos.EdgeDaemon",
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired,
        nullptr, // on_name_acquired (optional)
        nullptr, // on_name_lost (optional)
        nullptr, // user_data
        nullptr  // user_data_free_func
    );

    // Main loop for dispatching IPC requests
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
    
    // Cleanup (normally unreachable)
    g_main_loop_unref(loop);
    g_bus_unown_name(owner_id);
}
