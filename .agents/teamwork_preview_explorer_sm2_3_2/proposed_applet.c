#include <gtk/gtk.h>
#include <systemd/sd-bus.h>
#include <stdlib.h>

// Global pointer to the user D-Bus connection
static sd_bus *user_bus = NULL;

// Helper to call basic methods (Pause / Resume)
static void call_simple_method(const char *method_name) {
    if (!user_bus) {
        g_printerr("D-Bus not connected\n");
        return;
    }
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int r = sd_bus_call_method(user_bus,
                               "org.chronos.EdgeDaemon",
                               "/org/chronos/EdgeDaemon",
                               "org.chronos.EdgeDaemon.Controller",
                               method_name,
                               &error,
                               &m,
                               "");
    if (r < 0) {
        g_printerr("D-Bus method %s failed: %s\n", method_name, error.message);
    } else {
        char *response = NULL;
        sd_bus_message_read(m, "s", &response);
        g_print("Daemon response: %s\n", response ? response : "OK");
    }
    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
}

static void on_pause_clicked(GtkMenuItem *menuitem, gpointer user_data) {
    call_simple_method("Pause");
}

static void on_resume_clicked(GtkMenuItem *menuitem, gpointer user_data) {
    call_simple_method("Resume");
}

// Function to fetch status from the daemon
static gboolean get_daemon_status(gboolean *paused, double *epsilon, double *sensitivity, double *budget, double *cumulative) {
    if (!user_bus) return FALSE;
    
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int r = sd_bus_call_method(user_bus,
                               "org.chronos.EdgeDaemon",
                               "/org/chronos/EdgeDaemon",
                               "org.chronos.EdgeDaemon.Controller",
                               "GetStatus",
                               &error,
                               &m,
                               "");
    if (r < 0) {
        g_printerr("Failed to get daemon status: %s\n", error.message);
        sd_bus_error_free(&error);
        return FALSE;
    }
    
    int p_val = 0;
    r = sd_bus_message_read(m, "bdddd", &p_val, epsilon, sensitivity, budget, cumulative);
    if (r < 0) {
        g_printerr("Failed to parse status response: %s\n", strerror(-r));
        sd_bus_message_unref(m);
        return FALSE;
    }
    
    *paused = (p_val != 0);
    sd_bus_message_unref(m);
    return TRUE;
}

static void on_status_clicked(GtkMenuItem *menuitem, gpointer user_data) {
    gboolean paused = FALSE;
    double epsilon = 0.0, sensitivity = 0.0, budget = 0.0, cumulative = 0.0;
    
    if (get_daemon_status(&paused, &epsilon, &sensitivity, &budget, &cumulative)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_INFO,
                                                   GTK_BUTTONS_OK,
                                                   "Chronos Daemon Status:\n\n"
                                                   "Tracking State: %s\n"
                                                   "Epsilon (ε): %.4f\n"
                                                   "Sensitivity: %.4f\n"
                                                   "Privacy Budget: %.4f\n"
                                                   "24h Cumulative Budget Used: %.4f",
                                                   paused ? "PAUSED" : "ACTIVE",
                                                   epsilon, sensitivity, budget, cumulative);
        gtk_window_set_title(GTK_WINDOW(dialog), "Chronos Status");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    } else {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Could not connect to edge-daemon.\nIs it running?");
        gtk_window_set_title(GTK_WINDOW(dialog), "Connection Error");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
}

static void on_configure_clicked(GtkMenuItem *menuitem, gpointer user_data) {
    gboolean paused = FALSE;
    double epsilon = 0.0, sensitivity = 0.0, budget = 0.0, cumulative = 0.0;
    
    if (!get_daemon_status(&paused, &epsilon, &sensitivity, &budget, &cumulative)) {
        GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   GTK_MESSAGE_ERROR,
                                                   GTK_BUTTONS_OK,
                                                   "Could not connect to edge-daemon to fetch current config.");
        gtk_window_set_title(GTK_WINDOW(dialog), "Connection Error");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Configure Parameters",
                                                   NULL,
                                                   GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   "_Cancel", GTK_RESPONSE_CANCEL,
                                                   "_Save", GTK_RESPONSE_ACCEPT,
                                                   NULL);
    
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 15);
    
    GtkWidget *lbl_eps = gtk_label_new("Epsilon (ε):");
    GtkWidget *ent_eps = gtk_entry_new();
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4f", epsilon);
    gtk_entry_set_text(GTK_ENTRY(ent_eps), buf);
    
    GtkWidget *lbl_sens = gtk_label_new("Sensitivity:");
    GtkWidget *ent_sens = gtk_entry_new();
    snprintf(buf, sizeof(buf), "%.4f", sensitivity);
    gtk_entry_set_text(GTK_ENTRY(ent_sens), buf);
    
    GtkWidget *lbl_bud = gtk_label_new("Privacy Budget:");
    GtkWidget *ent_bud = gtk_entry_new();
    snprintf(buf, sizeof(buf), "%.4f", budget);
    gtk_entry_set_text(GTK_ENTRY(ent_bud), buf);
    
    gtk_grid_attach(GTK_GRID(grid), lbl_eps, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_eps, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_sens, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_sens, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_bud, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_bud, 1, 2, 1, 1);
    
    gtk_container_add(GTK_CONTAINER(content_area), grid);
    gtk_widget_show_all(dialog);
    
    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    if (result == GTK_RESPONSE_ACCEPT) {
        double new_eps = atof(gtk_entry_get_text(GTK_ENTRY(ent_eps)));
        double new_sens = atof(gtk_entry_get_text(GTK_ENTRY(ent_sens)));
        double new_bud = atof(gtk_entry_get_text(GTK_ENTRY(ent_bud)));
        
        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message *m = NULL;
        int r = sd_bus_call_method(user_bus,
                                   "org.chronos.EdgeDaemon",
                                   "/org/chronos/EdgeDaemon",
                                   "org.chronos.EdgeDaemon.Controller",
                                   "Configure",
                                   &error,
                                   &m,
                                   "ddd",
                                   new_eps, new_sens, new_bud);
        if (r < 0) {
            g_printerr("Failed to save config: %s\n", error.message);
        } else {
            g_print("Daemon successfully reconfigured.\n");
        }
        sd_bus_error_free(&error);
        sd_bus_message_unref(m);
    }
    
    gtk_widget_destroy(dialog);
}

static void on_quit_clicked(GtkMenuItem *menuitem, gpointer user_data) {
    gtk_main_quit();
}

static void show_menu(GtkStatusIcon *status_icon, guint button, guint activate_time, gpointer user_data) {
    GtkWidget *menu = (GtkWidget *)user_data;
    // gtk_menu_popup is deprecated but standard for GtkStatusIcon
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, status_icon, button, activate_time);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    
    // Connect to user session DBus
    int r = sd_bus_open_user(&user_bus);
    if (r < 0) {
        g_printerr("Error: Could not connect to session DBus: %s\n", strerror(-r));
        return 1;
    }
    
    // Create status icon
    GtkStatusIcon *tray_icon = gtk_status_icon_new_from_icon_name("media-playback-start");
    gtk_status_icon_set_tooltip_text(tray_icon, "Chronos Daemon Controller");
    
    // Create menu
    GtkWidget *menu = gtk_menu_new();
    
    GtkWidget *item_status = gtk_menu_item_new_with_label("Status Info...");
    g_signal_connect(G_OBJECT(item_status), "activate", G_CALLBACK(on_status_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_status);
    
    GtkWidget *item_sep1 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_sep1);
    
    GtkWidget *item_pause = gtk_menu_item_new_with_label("Pause Tracking");
    g_signal_connect(G_OBJECT(item_pause), "activate", G_CALLBACK(on_pause_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_pause);
    
    GtkWidget *item_resume = gtk_menu_item_new_with_label("Resume Tracking");
    g_signal_connect(G_OBJECT(item_resume), "activate", G_CALLBACK(on_resume_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_resume);
    
    GtkWidget *item_configure = gtk_menu_item_new_with_label("Configure parameters...");
    g_signal_connect(G_OBJECT(item_configure), "activate", G_CALLBACK(on_configure_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_configure);
    
    GtkWidget *item_sep2 = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_sep2);
    
    GtkWidget *item_quit = gtk_menu_item_new_with_label("Quit Applet");
    g_signal_connect(G_OBJECT(item_quit), "activate", G_CALLBACK(on_quit_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_quit);
    
    gtk_widget_show_all(menu);
    
    g_signal_connect(G_OBJECT(tray_icon), "popup-menu", G_CALLBACK(show_menu), menu);
    
    gtk_main();
    
    if (user_bus) {
        sd_bus_close_unref(user_bus);
    }
    return 0;
}
