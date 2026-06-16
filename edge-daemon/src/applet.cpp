#include <gtk/gtk.h>
#include <iostream>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cctype>

struct DaemonStatus {
    bool online = false;
    bool paused = false;
    double epsilon = 0.5;
    double sensitivity = 1.0;
    double budget = 0.0;
    double cumulative_epsilon = 0.0;
};

// Global variables
GtkStatusIcon *status_icon = nullptr;
GtkWidget *status_item = nullptr;
GtkWidget *pause_item = nullptr;
GtkWidget *resume_item = nullptr;
GtkWidget *configure_item = nullptr;

bool parseDouble(const std::string& str, double& val) {
    std::stringstream ss(str);
    return (ss >> val) && ss.eof();
}

bool extractDouble(const std::string& json, const std::string& key, double& out_val) {
    size_t key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return false;
    
    size_t colon_pos = json.find(":", key_pos);
    if (colon_pos == std::string::npos) return false;
    
    size_t start = colon_pos + 1;
    while (start < json.size() && (std::isspace(json[start]) || json[start] == '"')) {
        start++;
    }
    
    size_t end = start;
    while (end < json.size() && (std::isdigit(json[end]) || json[end] == '.' || json[end] == '-' || json[end] == '+' || json[end] == 'e' || json[end] == 'E')) {
        end++;
    }
    
    if (end > start) {
        std::string val_str = json.substr(start, end - start);
        return parseDouble(val_str, out_val);
    }
    return false;
}

std::string send_http_request(const std::string& method, const std::string& path, const std::string& body = "") {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8888);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        close(sock);
        return "";
    }

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return "";
    }

    std::stringstream req;
    req << method << " " << path << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1:8888\r\n"
        << "Content-Length: " << body.length() << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Connection: close\r\n\r\n"
        << body;

    std::string req_str = req.str();
    send(sock, req_str.c_str(), req_str.length(), 0);

    std::string response = "";
    char buffer[2048];
    int n;
    while ((n = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[n] = '\0';
        response += buffer;
    }

    close(sock);
    return response;
}

DaemonStatus get_daemon_status() {
    DaemonStatus ds;
    std::string res = send_http_request("GET", "/status");
    if (res.empty()) {
        ds.online = false;
        return ds;
    }
    ds.online = true;
    ds.paused = (res.find("\"paused\":true") != std::string::npos);
    extractDouble(res, "epsilon", ds.epsilon);
    extractDouble(res, "sensitivity", ds.sensitivity);
    extractDouble(res, "budget", ds.budget);
    extractDouble(res, "cumulative_epsilon", ds.cumulative_epsilon);
    return ds;
}

void refresh_status() {
    DaemonStatus ds = get_daemon_status();
    if (!status_item || !pause_item || !resume_item || !configure_item || !status_icon) return;

    if (!ds.online) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), "Status: Offline");
        gtk_widget_set_sensitive(pause_item, FALSE);
        gtk_widget_set_sensitive(resume_item, FALSE);
        gtk_widget_set_sensitive(configure_item, FALSE);
        gtk_status_icon_set_tooltip_text(status_icon, "Chronos: Offline");
    } else {
        if (ds.paused) {
            gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), "Status: Paused");
            gtk_widget_set_sensitive(pause_item, FALSE);
            gtk_widget_set_sensitive(resume_item, TRUE);
            gtk_status_icon_set_tooltip_text(status_icon, "Chronos: Paused");
        } else {
            gtk_menu_item_set_label(GTK_MENU_ITEM(status_item), "Status: Running");
            gtk_widget_set_sensitive(pause_item, TRUE);
            gtk_widget_set_sensitive(resume_item, FALSE);
            std::stringstream ss;
            ss.precision(4);
            ss << "Chronos: Running\nEps: " << ds.epsilon << "\nBudget: " << ds.budget 
               << "\nUsed (24h): " << ds.cumulative_epsilon;
            gtk_status_icon_set_tooltip_text(status_icon, ss.str().c_str());
        }
        gtk_widget_set_sensitive(configure_item, TRUE);
    }
}

gboolean periodic_status_check(gpointer data) {
    refresh_status();
    return TRUE;
}

void on_pause_clicked(GtkMenuItem *menuitem, gpointer user_data) {
    send_http_request("POST", "/control", "{\"action\":\"pause\"}");
    refresh_status();
}

void on_resume_clicked(GtkMenuItem *menuitem, gpointer user_data) {
    send_http_request("POST", "/control", "{\"action\":\"resume\"}");
    refresh_status();
}

void on_configure_clicked(GtkMenuItem *menuitem, gpointer user_data) {
    DaemonStatus ds = get_daemon_status();
    if (!ds.online) return;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Chronos Configuration",
        nullptr,
        GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL,
        "OK", GTK_RESPONSE_OK,
        nullptr
    );

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content_area), 15);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_add(GTK_CONTAINER(content_area), grid);

    GtkWidget *lbl_epsilon = gtk_label_new("Epsilon (Privacy Loss):");
    gtk_widget_set_halign(lbl_epsilon, GTK_ALIGN_START);
    GtkWidget *ent_epsilon = gtk_entry_new();
    std::string eps_str = std::to_string(ds.epsilon);
    eps_str.erase(eps_str.find_last_not_of('0') + 1, std::string::npos);
    if (eps_str.back() == '.') eps_str.pop_back();
    gtk_entry_set_text(GTK_ENTRY(ent_epsilon), eps_str.c_str());
    gtk_grid_attach(GTK_GRID(grid), lbl_epsilon, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_epsilon, 1, 0, 1, 1);

    GtkWidget *lbl_budget = gtk_label_new("Privacy Budget (24h):");
    gtk_widget_set_halign(lbl_budget, GTK_ALIGN_START);
    GtkWidget *ent_budget = gtk_entry_new();
    std::string bud_str = std::to_string(ds.budget);
    bud_str.erase(bud_str.find_last_not_of('0') + 1, std::string::npos);
    if (bud_str.back() == '.') bud_str.pop_back();
    gtk_entry_set_text(GTK_ENTRY(ent_budget), bud_str.c_str());
    gtk_grid_attach(GTK_GRID(grid), lbl_budget, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ent_budget, 1, 1, 1, 1);

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_OK) {
        const gchar *eps_text = gtk_entry_get_text(GTK_ENTRY(ent_epsilon));
        const gchar *bud_text = gtk_entry_get_text(GTK_ENTRY(ent_budget));

        double eps = 0.5;
        double bud = 0.0;
        if (parseDouble(eps_text, eps) && parseDouble(bud_text, bud)) {
            std::stringstream ss;
            ss << "{"
               << "\"action\":\"configure\","
               << "\"epsilon\":" << eps << ","
               << "\"budget\":" << bud
               << "}";
            send_http_request("POST", "/control", ss.str());
        } else {
            GtkWidget *error_dialog = gtk_message_dialog_new(
                GTK_WINDOW(dialog),
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_OK,
                "Error: invalid numeric values entered."
            );
            gtk_dialog_run(GTK_DIALOG(error_dialog));
            gtk_widget_destroy(error_dialog);
        }
    }

    gtk_widget_destroy(dialog);
    refresh_status();
}

void on_quit_clicked(GtkMenuItem *menuitem, gpointer user_data) {
    gtk_main_quit();
}

void show_popup_menu(GtkStatusIcon *icon, guint button, guint activate_time, gpointer user_data) {
    GtkWidget *menu = GTK_WIDGET(user_data);
    refresh_status();
    gtk_menu_popup(GTK_MENU(menu), nullptr, nullptr, gtk_status_icon_position_menu, icon, button, activate_time);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    status_icon = gtk_status_icon_new_from_icon_name("preferences-system");
    gtk_status_icon_set_visible(status_icon, TRUE);

    GtkWidget *menu = gtk_menu_new();

    status_item = gtk_menu_item_new_with_label("Status: Unknown");
    gtk_widget_set_sensitive(status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), status_item);

    GtkWidget *separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);

    pause_item = gtk_menu_item_new_with_label("Pause Tracking");
    g_signal_connect(G_OBJECT(pause_item), "activate", G_CALLBACK(on_pause_clicked), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pause_item);

    resume_item = gtk_menu_item_new_with_label("Resume Tracking");
    g_signal_connect(G_OBJECT(resume_item), "activate", G_CALLBACK(on_resume_clicked), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), resume_item);

    configure_item = gtk_menu_item_new_with_label("Configure...");
    g_signal_connect(G_OBJECT(configure_item), "activate", G_CALLBACK(on_configure_clicked), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), configure_item);

    GtkWidget *quit_separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_separator);

    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(G_OBJECT(quit_item), "activate", G_CALLBACK(on_quit_clicked), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);

    g_signal_connect(G_OBJECT(status_icon), "popup-menu", G_CALLBACK(show_popup_menu), menu);

    refresh_status();
    g_timeout_add_seconds(5, periodic_status_check, nullptr);

    gtk_main();

    return 0;
}
