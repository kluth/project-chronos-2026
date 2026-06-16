#include <gtk/gtk.h>
#include <gio/gio.h>
#include <iostream>
#include <string>

// Global pointers for GTK widgets and DBus connection
static GDBusConnection* g_session_bus = nullptr;
static GtkStatusIcon* g_status_icon = nullptr;
static GtkWidget* g_menu_status_item = nullptr;
static GtkWidget* g_menu_toggle_item = nullptr;
static bool g_is_paused = false;

// Helper to invoke method on daemon over D-Bus Session Bus
static bool call_daemon_method(const char* method_name, GVariant* parameters) {
    if (!g_session_bus) {
        std::cerr << "[Applet] Session bus not connected!" << std::endl;
        return false;
    }
    
    GError* error = nullptr;
    GVariant* result = g_dbus_connection_call_sync(
        g_session_bus,
        "org.chronos.EdgeDaemon",           // Bus Name
        "/org/chronos/EdgeDaemon",          // Object Path
        "org.chronos.EdgeDaemon",           // Interface Name
        method_name,
        parameters,
        nullptr,                            // Expected reply type
        G_DBUS_CALL_FLAGS_NONE,
        -1,                                 // Timeout
        nullptr,                            // Cancellable
        &error
    );

    if (error) {
        std::cerr << "[Applet] D-Bus call to " << method_name << " failed: " << error->message << std::endl;
        g_error_free(error);
        return false;
    }

    if (result) {
        g_variant_unref(result);
    }
    return true;
}

// Callback for Pause/Resume menu toggle action
static void on_toggle_tracking(GtkMenuItem* item, gpointer data) {
    if (g_is_paused) {
        if (call_daemon_method("Resume", nullptr)) {
            g_is_paused = false;
            gtk_menu_item_set_label(GTK_MENU_ITEM(g_menu_status_item), "Status: Active");
            gtk_menu_item_set_label(GTK_MENU_ITEM(g_menu_toggle_item), "Pause Tracking");
            
            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            gtk_status_icon_set_from_icon_name(g_status_icon, "media-record");
            #pragma GCC diagnostic pop
            
            std::cout << "[Applet] Tracking resumed successfully." << std::endl;
        }
    } else {
        if (call_daemon_method("Pause", nullptr)) {
            g_is_paused = true;
            gtk_menu_item_set_label(GTK_MENU_ITEM(g_menu_status_item), "Status: Paused");
            gtk_menu_item_set_label(GTK_MENU_ITEM(g_menu_toggle_item), "Resume Tracking");
            
            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            gtk_status_icon_set_from_icon_name(g_status_icon, "media-playback-pause");
            #pragma GCC diagnostic pop
            
            std::cout << "[Applet] Tracking paused successfully." << std::endl;
        }
    }
}

// Callback for Configure action - opens dialog with entries
static void on_configure(GtkMenuItem* item, gpointer data) {
    double current_epsilon = 1.0;
    double current_sensitivity = 0.1;
    double current_budget = 10.0;

    // Fetch current status/configuration from the daemon
    if (g_session_bus) {
        GError* error = nullptr;
        GVariant* result = g_dbus_connection_call_sync(
            g_session_bus,
            "org.chronos.EdgeDaemon",
            "/org/chronos/EdgeDaemon",
            "org.chronos.EdgeDaemon",
            "GetStatus",
            nullptr,
            G_VARIANT_TYPE("(bddd)"), // Returns: (paused, epsilon, sensitivity, budget)
            G_DBUS_CALL_FLAGS_NONE,
            -1, nullptr, &error
        );

        if (!error && result) {
            gboolean paused;
            g_variant_get(result, "(bddd)", &paused, &current_epsilon, &current_sensitivity, &current_budget);
            g_is_paused = paused;
            g_variant_unref(result);
        } else {
            if (error) {
                std::cerr << "[Applet] Failed to fetch daemon status: " << error->message << std::endl;
                g_error_free(error);
            }
            std::cout << "[Applet] Using default parameters due to daemon status query error." << std::endl;
        }
    }

    // Build the GTK configuration dialog
    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Configure Chronos Daemon",
        nullptr,
        GTK_DIALOG_MODAL,
        "Save", GTK_RESPONSE_ACCEPT,
        "Cancel", GTK_RESPONSE_REJECT,
        nullptr
    );

    GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    
    // Create Layout Grid
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 15);
    gtk_container_add(GTK_CONTAINER(content_area), grid);

    // Epsilon row
    GtkWidget* lbl_eps = gtk_label_new("Epsilon (Privacy Noise Level):");
    GtkWidget* ent_eps = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ent_eps), std::to_string(current_epsilon).c_str());
    gtk_grid_attach(GTK_GRID(grid), lbl_eps, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_eps, 1, 0, 1, 1);

    // Sensitivity row
    GtkWidget* lbl_sens = gtk_label_new("Sensitivity:");
    GtkWidget* ent_sens = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ent_sens), std::to_string(current_sensitivity).c_str());
    gtk_grid_attach(GTK_GRID(grid), lbl_sens, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_sens, 1, 1, 1, 1);

    // Budget row
    GtkWidget* lbl_budget = gtk_label_new("24h Cumulative Privacy Budget:");
    GtkWidget* ent_budget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ent_budget), std::to_string(current_budget).c_str());
    gtk_grid_attach(GTK_GRID(grid), lbl_budget, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_budget, 1, 2, 1, 1);

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        try {
            double eps = std::stod(gtk_entry_get_text(GTK_ENTRY(ent_eps)));
            double sens = std::stod(gtk_entry_get_text(GTK_ENTRY(ent_sens)));
            double budget = std::stod(gtk_entry_get_text(GTK_ENTRY(ent_budget)));

            if (call_daemon_method("Configure", g_variant_new("(ddd)", eps, sens, budget))) {
                std::cout << "[Applet] Dynamic configuration applied: Epsilon=" << eps 
                          << ", Sensitivity=" << sens << ", Budget=" << budget << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[Applet] Invalid numerical parameters input: " << e.what() << std::endl;
            
            GtkWidget* err_dialog = gtk_message_dialog_new(
                GTK_WINDOW(dialog),
                GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_CLOSE,
                "Error: Input values must be valid decimal numbers."
            );
            gtk_dialog_run(GTK_DIALOG(err_dialog));
            gtk_widget_destroy(err_dialog);
        }
    }

    gtk_widget_destroy(dialog);
}

// Callback for Restart Daemon action
static void on_restart_daemon(GtkMenuItem* item, gpointer data) {
    std::cout << "[Applet] Requesting systemd restart of edge-daemon.service..." << std::endl;
    int ret = system("systemctl --user restart edge-daemon.service");
    if (ret != 0) {
        std::cout << "[Applet] systemd failed. Attempting process-based recovery..." << std::endl;
        system("killall edge-daemon 2>/dev/null");
        // Start daemon in background
        system("./edge-daemon &");
    }
}

// Callback for quitting the system tray applet
static void on_quit_applet(GtkMenuItem* item, gpointer data) {
    std::cout << "[Applet] Shutting down controller applet." << std::endl;
    gtk_main_quit();
}

// Handler to open the popup menu on system tray interaction
static void on_status_icon_popup_menu(GtkStatusIcon* status_icon, guint button, guint activate_time, gpointer user_data) {
    GtkWidget* menu = GTK_WIDGET(user_data);
    
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    gtk_menu_popup_at_pointer(GTK_MENU(menu), nullptr);
    #pragma GCC diagnostic pop
}

int main(int argc, char* argv[]) {
    gtk_init(&argc, &argv);

    // Establish D-Bus Session Connection
    GError* error = nullptr;
    g_session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (error) {
        std::cerr << "[Applet] Warning: Failed to connect to Session Bus: " << error->message << std::endl;
        g_error_free(error);
    } else {
        std::cout << "[Applet] Connected to Session D-Bus." << std::endl;
    }

    // Build Right-Click System Tray Menu
    GtkWidget* menu = gtk_menu_new();

    // 1. Status display item (read-only label)
    g_menu_status_item = gtk_menu_item_new_with_label("Status: Active");
    gtk_widget_set_sensitive(g_menu_status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_menu_status_item);

    GtkWidget* sep1 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep1);

    // 2. Pause / Resume Toggle
    g_menu_toggle_item = gtk_menu_item_new_with_label("Pause Tracking");
    g_signal_connect(g_menu_toggle_item, "activate", G_CALLBACK(on_toggle_tracking), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_menu_toggle_item);

    // 3. Configure Parameters Dialog Entry
    GtkWidget* menu_config = gtk_menu_item_new_with_label("Configure...");
    g_signal_connect(menu_config, "activate", G_CALLBACK(on_configure), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_config);

    // 4. Daemon Restart Utility
    GtkWidget* menu_restart = gtk_menu_item_new_with_label("Restart Daemon");
    g_signal_connect(menu_restart, "activate", G_CALLBACK(on_restart_daemon), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_restart);

    GtkWidget* sep2 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep2);

    // 5. Exit Applet
    GtkWidget* menu_quit = gtk_menu_item_new_with_label("Quit Applet");
    g_signal_connect(menu_quit, "activate", G_CALLBACK(on_quit_applet), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_quit);

    gtk_widget_show_all(menu);

    // Initialize the Status Icon in system tray
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    g_status_icon = gtk_status_icon_new_from_icon_name("media-record"); // Active tracking icon (red dot)
    gtk_status_icon_set_tooltip_text(g_status_icon, "Chronos Local Tracker");
    g_signal_connect(g_status_icon, "popup-menu", G_CALLBACK(on_status_icon_popup_menu), menu);
    #pragma GCC diagnostic pop

    std::cout << "[Applet] System Tray Applet is running. Waiting for interactions..." << std::endl;

    // Run GTK loop
    gtk_main();

    // Cleanup
    if (g_session_bus) {
        g_object_unref(g_session_bus);
    }
    return 0;
}
