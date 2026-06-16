#include <iostream>
#include <string>
#include <array>
#include <memory>
#include <thread>
#include <chrono>
#include <cmath>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <gio/gio.h>
#include "differential_privacy.h"



// DBus callbacks
void on_prepare_for_sleep(GDBusConnection* connection,
                          const gchar* sender_name,
                          const gchar* object_path,
                          const gchar* interface_name,
                          const gchar* signal_name,
                          GVariant* parameters,
                          gpointer user_data) {
    gboolean active;
    g_variant_get(parameters, "(b)", &active);
    g_dbus_paused = (active == TRUE);
    std::cout << "[DBus Listener] PrepareForSleep active: " << (active ? "true" : "false")
              << " -> g_dbus_paused: " << (g_dbus_paused ? "true" : "false") << std::endl;
}

void on_session_lock(GDBusConnection* connection,
                     const gchar* sender_name,
                     const gchar* object_path,
                     const gchar* interface_name,
                     const gchar* signal_name,
                     GVariant* parameters,
                     gpointer user_data) {
    g_dbus_paused = true;
    std::cout << "[DBus Listener] Session Lock -> g_dbus_paused: true" << std::endl;
}

void on_session_unlock(GDBusConnection* connection,
                       const gchar* sender_name,
                       const gchar* object_path,
                       const gchar* interface_name,
                       const gchar* signal_name,
                       GVariant* parameters,
                       gpointer user_data) {
    g_dbus_paused = false;
    std::cout << "[DBus Listener] Session Unlock -> g_dbus_paused: false" << std::endl;
}

void on_screensaver_active_changed(GDBusConnection* connection,
                                   const gchar* sender_name,
                                   const gchar* object_path,
                                   const gchar* interface_name,
                                   const gchar* signal_name,
                                   GVariant* parameters,
                                   gpointer user_data) {
    gboolean active;
    g_variant_get(parameters, "(b)", &active);
    g_dbus_paused = (active == TRUE);
    std::cout << "[DBus Listener] Screensaver ActiveChanged active: " << (active ? "true" : "false")
              << " -> g_dbus_paused: " << (g_dbus_paused ? "true" : "false") << std::endl;
}

void dbusListenerThread() {
    GMainContext* context = g_main_context_new();
    g_main_context_push_thread_default(context);

    GError* error = nullptr;
    GDBusConnection* system_conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!system_conn) {
        std::cerr << "[DBus Listener] Failed to connect to System Bus: " 
                  << (error ? error->message : "unknown error") << std::endl;
        if (error) g_error_free(error);
        error = nullptr;
    } else {
        std::cout << "[DBus Listener] Connected to System Bus." << std::endl;
        
        g_dbus_connection_signal_subscribe(
            system_conn,
            "org.freedesktop.login1",
            "org.freedesktop.login1.Manager",
            "PrepareForSleep",
            "/org/freedesktop/login1",
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_prepare_for_sleep,
            nullptr,
            nullptr
        );

        g_dbus_connection_signal_subscribe(
            system_conn,
            "org.freedesktop.login1",
            "org.freedesktop.login1.Session",
            "Lock",
            nullptr,
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_session_lock,
            nullptr,
            nullptr
        );

        g_dbus_connection_signal_subscribe(
            system_conn,
            "org.freedesktop.login1",
            "org.freedesktop.login1.Session",
            "Unlock",
            nullptr,
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_session_unlock,
            nullptr,
            nullptr
        );
    }

    GDBusConnection* session_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!session_conn) {
        std::cerr << "[DBus Listener] Failed to connect to Session Bus: "
                  << (error ? error->message : "unknown error") << std::endl;
        if (error) g_error_free(error);
    } else {
        std::cout << "[DBus Listener] Connected to Session Bus." << std::endl;

        g_dbus_connection_signal_subscribe(
            session_conn,
            nullptr,
            "org.freedesktop.ScreenSaver",
            "ActiveChanged",
            nullptr,
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_screensaver_active_changed,
            nullptr,
            nullptr
        );

        g_dbus_connection_signal_subscribe(
            session_conn,
            nullptr,
            "org.gnome.ScreenSaver",
            "ActiveChanged",
            nullptr,
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_screensaver_active_changed,
            nullptr,
            nullptr
        );
    }

    if (system_conn || session_conn) {
        GMainLoop* loop = g_main_loop_new(context, FALSE);
        std::cout << "[DBus Listener] Starting DBus listener loop..." << std::endl;
        g_main_loop_run(loop);
        g_main_loop_unref(loop);
    } else {
        std::cerr << "[DBus Listener] Both System and Session DBus connections failed. DBus listener thread exiting." << std::endl;
    }

    if (system_conn) g_object_unref(system_conn);
    if (session_conn) g_object_unref(session_conn);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
}

void backgroundTelemetryThread(const std::string& db_path) {
    std::cout << "[Background Monitor] Starting native process scanner..." << std::endl;
    
    auto last_backup_time = std::chrono::steady_clock::now();
    const auto backup_interval = std::chrono::minutes(15);
    
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        // 1. Process Scanning (F37)
        if (!isTelemetryPaused()) {
            auto active_tools = scanNativeProcesses();
            for (const auto& tool : active_tools) {
                TelemetryEvent raw_event = { "dev_tool_" + tool, 10.0 };
                std::string cleaned_metric = obfuscateDomainOrCategory(raw_event.metric_name);
                TelemetryEvent cleaned_event = { cleaned_metric, raw_event.value };
                
                double cumulative = getCumulativeEpsilon24h(db_path);
                double current_epsilon;
                double config_budget;
                {
                    std::lock_guard<std::mutex> lock(g_config_mutex);
                    current_epsilon = g_config.epsilon;
                    config_budget = g_config.budget;
                }
                
                if (config_budget > 0.0 && cumulative >= config_budget) {
                    std::cout << "[Privacy Budget Check] Limit exceeded (" << cumulative << "/" << config_budget 
                              << "). Skipping dev tool log for " << cleaned_event.metric_name << std::endl;
                    continue;
                } else if (config_budget > 0.0 && cumulative >= 0.8 * config_budget) {
                    std::lock_guard<std::mutex> lock(g_config_mutex);
                    current_epsilon = calculateAdjustedEpsilon(g_config.epsilon, g_config.budget, cumulative);
                    std::cout << "[Privacy Budget Check] Approaching limit (" << cumulative << "/" << g_config.budget 
                              << "). Adjusting epsilon to " << current_epsilon << std::endl;
                }
                
                TelemetryEvent safe_event = anonymizeEvent(cleaned_event, current_epsilon);
                logPrivacyEpsilon(current_epsilon, db_path);
                
                if (!std::isfinite(safe_event.value)) {
                    std::cerr << "[Warning] safe_event.value is not finite (" << safe_event.value << ") for metric " << safe_event.metric_name << ". Clamping to 0.0." << std::endl;
                    safe_event.value = 0.0;
                }
                
                std::cout << "[Background Scanner] Found active process: " << tool << ". Buffering/transmitting..." << std::endl;
                if (isNetworkOnline()) {
                    flushBuffer(db_path);
                    if (!runCurlSafe(safe_event.metric_name, safe_event.value)) {
                        if (!bufferEvent(safe_event, db_path)) {
                            std::cerr << "[Warning] Failed to buffer event to database: " << safe_event.metric_name << std::endl;
                        }
                    }
                } else {
                    if (!bufferEvent(safe_event, db_path)) {
                        std::cerr << "[Warning] Failed to buffer event to database: " << safe_event.metric_name << std::endl;
                    }
                }
            }
        }
        
        // 3. Automated Backup Snapshot (F47)
        auto now_time = std::chrono::steady_clock::now();
        if (now_time - last_backup_time >= backup_interval) {
            dumpBackupToJson(db_path);
            last_backup_time = now_time;
        }
    }
}

int main(int argc, char* argv[]) {
    bool calibrate = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--epsilon") {
            if (i + 1 < argc) {
                if (!parseDouble(argv[++i], g_config.epsilon)) {
                    std::cerr << "Error: invalid value for --epsilon" << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: --epsilon requires a value" << std::endl;
                return 1;
            }
        } else if (arg == "--sensitivity") {
            if (i + 1 < argc) {
                if (!parseDouble(argv[++i], g_config.sensitivity)) {
                    std::cerr << "Error: invalid value for --sensitivity" << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: --sensitivity requires a value" << std::endl;
                return 1;
            }
        } else if (arg == "--budget") {
            if (i + 1 < argc) {
                if (!parseDouble(argv[++i], g_config.budget)) {
                    std::cerr << "Error: invalid value for --budget" << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: --budget requires a value" << std::endl;
                return 1;
            }
        } else if (arg == "--secret") {
            if (i + 1 < argc) {
                g_config.secret = argv[++i];
            } else {
                std::cerr << "Error: --secret requires a value" << std::endl;
                return 1;
            }
        } else if (arg == "--calibrate") {
            calibrate = true;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            return 1;
        }
    }

    if (calibrate) {
        double scale = g_config.sensitivity / g_config.epsilon;
        double variance = 2.0 * scale * scale;
        double ci95 = scale * 2.99573227355;
        double ci99 = scale * 4.60517018599;

        std::cout << "DP Noise Bounds Calibration:\n";
        std::cout << "Epsilon: " << g_config.epsilon << "\n";
        std::cout << "Sensitivity: " << g_config.sensitivity << "\n";
        std::cout << "Scale: " << scale << "\n";
        std::cout << "Variance: " << variance << "\n";
        std::cout << "95% Confidence Interval: [-" << ci95 << ", " << ci95 << "]\n";
        std::cout << "99% Confidence Interval: [-" << ci99 << ", " << ci99 << "]\n";
        return 0;
    }

    std::cout << "[Edge Daemon] Initializing Chronos Local Tracker (Crostini Escaper Mode)..." << std::endl;
    std::cout << "[Edge Daemon] Enforcing epsilon-Differential Privacy (Epsilon=" << g_config.epsilon 
              << ", Sensitivity=" << g_config.sensitivity << ")." << std::endl;

    const std::string DB_PATH = "telemetry_buffer.db";
    if (!initDatabase(DB_PATH)) {
        std::cerr << "Failed to initialize SQLite buffer database." << std::endl;
        return 1;
    }
    if (!initPrivacyBudgetTable(DB_PATH)) {
        std::cerr << "Failed to initialize SQLite privacy budget log." << std::endl;
        return 1;
    }

    std::thread bg_thread(backgroundTelemetryThread, DB_PATH);
    bg_thread.detach();

    std::thread dbus_thread(dbusListenerThread);
    dbus_thread.detach();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8888);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed on port 8888" << std::endl;
        return 1;
    }
    
    listen(server_fd, 3);
    std::cout << "[Edge Daemon] Listening on 0.0.0.0:8888 for Host ChromeOS Telemetry..." << std::endl;

    while (true) {
        int new_socket = accept(server_fd, nullptr, nullptr);
        if (new_socket < 0) continue;

        std::string request = "";
        char buffer[1024];
        ssize_t bytes_read;
        size_t content_length = 0;
        size_t header_end = std::string::npos;

        while (true) {
            bytes_read = recv(new_socket, buffer, sizeof(buffer) - 1, 0);
            if (bytes_read <= 0) {
                break;
            }
            buffer[bytes_read] = '\0';
            request.append(buffer, bytes_read);

            if (header_end == std::string::npos) {
                header_end = request.find("\r\n\r\n");
                if (header_end == std::string::npos) {
                    header_end = request.find("\n\n");
                }
                
                if (header_end != std::string::npos) {
                    size_t cl_pos = request.find("Content-Length:");
                    if (cl_pos == std::string::npos) {
                        cl_pos = request.find("content-length:");
                    }
                    if (cl_pos != std::string::npos && cl_pos < header_end) {
                        size_t end_line = request.find("\n", cl_pos);
                        std::string cl_line = request.substr(cl_pos, end_line - cl_pos);
                        size_t colon = cl_line.find(":");
                        if (colon != std::string::npos) {
                            std::string cl_val = cl_line.substr(colon + 1);
                            size_t first = cl_val.find_first_not_of(" \r\n");
                            size_t last = cl_val.find_last_not_of(" \r\n");
                            if (first != std::string::npos && last != std::string::npos) {
                                std::stringstream cl_ss(cl_val.substr(first, last - first + 1));
                                cl_ss >> content_length;
                            }
                        }
                    }
                }
            }

            if (header_end != std::string::npos) {
                size_t body_start = header_end + (request.find("\r\n\r\n") != std::string::npos ? 4 : 2);
                if (request.size() - body_start >= content_length) {
                    break;
                }
            }
        }
        std::cout << "[Edge Daemon Server] Received request:\n" << request << std::endl;

        std::string method, path;
        std::istringstream iss(request);
        iss >> method >> path;
        std::cout << "[Edge Daemon Server] Method: '" << method << "', Path: '" << path << "'" << std::endl;

        if (method.empty() || path.empty()) {
            close(new_socket);
            continue;
        }

        if (method == "GET" && path == "/status") {
            if (!verifySignature(method, path, request, "")) {
                std::string err_resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"error\":\"unauthorized\"}";
                send(new_socket, err_resp.c_str(), err_resp.length(), 0);
                close(new_socket);
                continue;
            }
            double cumulative = getCumulativeEpsilon24h(DB_PATH);
            bool discharging = false;
            int battery_pct = getBatteryLevel(discharging);
            
            bool current_pause_state = isTelemetryPaused();
            double config_epsilon, config_sensitivity, config_budget;
            {
                std::lock_guard<std::mutex> lock(g_config_mutex);
                config_epsilon = g_config.epsilon;
                config_sensitivity = g_config.sensitivity;
                config_budget = g_config.budget;
            }
            
            std::stringstream ss;
            ss << "HTTP/1.1 200 OK\r\n"
               << "Content-Type: application/json\r\n"
               << "Access-Control-Allow-Origin: *\r\n\r\n"
               << "{"
               << "\"paused\":" << (current_pause_state ? "true" : "false") << ","
               << "\"epsilon\":" << config_epsilon << ","
               << "\"sensitivity\":" << config_sensitivity << ","
               << "\"budget\":" << config_budget << ","
               << "\"cumulative_epsilon\":" << cumulative << ","
               << "\"battery_level\":" << battery_pct << ","
               << "\"battery_discharging\":" << (discharging ? "true" : "false")
               << "}";
            std::string response = ss.str();
            send(new_socket, response.c_str(), response.length(), 0);
            close(new_socket);
            continue;
        } 
        else if (method == "POST" && path == "/control") {
            size_t body_pos = request.find("\r\n\r\n");
            std::string body;
            if (body_pos != std::string::npos) {
                body = request.substr(body_pos + 4);
            } else {
                body_pos = request.find("\n\n");
                if (body_pos != std::string::npos) {
                    body = request.substr(body_pos + 2);
                } else {
                    body = request;
                }
            }

            body = body.substr(0, content_length);

            if (!verifySignature(method, path, request, body)) {
                std::string err_resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"error\":\"unauthorized\"}";
                send(new_socket, err_resp.c_str(), err_resp.length(), 0);
                close(new_socket);
                continue;
            }

            std::string action = "";
            size_t act_pos = body.find("\"action\"");
            if (act_pos != std::string::npos) {
                size_t colon_pos = body.find(":", act_pos);
                if (colon_pos != std::string::npos) {
                    size_t start_quote = body.find("\"", colon_pos);
                    if (start_quote != std::string::npos) {
                        size_t end_quote = body.find("\"", start_quote + 1);
                        if (end_quote != std::string::npos) {
                            action = body.substr(start_quote + 1, end_quote - start_quote - 1);
                        }
                    }
                }
            }

            bool success = false;
            std::string err_msg = "";

            if (action == "pause") {
                g_tracking_paused = true;
                success = true;
            } else if (action == "resume") {
                g_tracking_paused = false;
                {
                    std::lock_guard<std::mutex> lock(g_config_mutex);
                    g_override_paused = false;
                }
                success = true;
            } else if (action == "pause_override") {
                double duration = 3600.0;
                extractDouble(body, "duration", duration);
                
                if (std::isnan(duration) || std::isinf(duration) || duration < 0.0 || duration > 31536000.0) {
                    duration = 3600.0;
                }
                
                {
                    std::lock_guard<std::mutex> lock(g_config_mutex);
                    g_override_paused = true;
                    g_override_paused_until = std::chrono::steady_clock::now() + std::chrono::seconds(static_cast<int>(duration));
                }
                success = true;
            } else if (action == "configure") {
                double new_epsilon;
                bool has_eps = extractDouble(body, "epsilon", new_epsilon);
                double new_budget;
                bool has_bud = extractDouble(body, "budget", new_budget);
                double new_sensitivity;
                bool has_sens = extractDouble(body, "sensitivity", new_sensitivity);
                
                {
                    std::lock_guard<std::mutex> lock(g_config_mutex);
                    if (has_eps) {
                        g_config.epsilon = new_epsilon;
                    }
                    if (has_bud) {
                        g_config.budget = new_budget;
                    }
                    if (has_sens) {
                        g_config.sensitivity = new_sensitivity;
                    }
                }
                success = true;
            } else {
                err_msg = "Invalid action: " + action;
            }

            std::stringstream ss;
            if (success) {
                ss << "HTTP/1.1 200 OK\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Access-Control-Allow-Origin: *\r\n\r\n"
                   << "{\"status\":\"ok\"}";
            } else {
                ss << "HTTP/1.1 400 Bad Request\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Access-Control-Allow-Origin: *\r\n\r\n"
                   << "{\"status\":\"error\",\"message\":\"" << err_msg << "\"}";
            }
            std::string response = ss.str();
            send(new_socket, response.c_str(), response.length(), 0);
            close(new_socket);
            continue;
        }

        size_t body_pos = request.find("\r\n\r\n");
        std::string body;
        if (body_pos != std::string::npos) {
            body = request.substr(body_pos + 4);
        } else {
            body_pos = request.find("\n\n");
            if (body_pos != std::string::npos) {
                body = request.substr(body_pos + 2);
            } else {
                body = request;
            }
        }

        body = body.substr(0, content_length);

        if (!verifySignature(method, path, request, body)) {
            std::string err_resp = "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"error\":\"unauthorized\"}";
            send(new_socket, err_resp.c_str(), err_resp.length(), 0);
            close(new_socket);
            continue;
        }

        // Determine Dynamic Ingestion Interval (F40) & Battery Power Saver Throttling (F48)
        bool discharging = false;
        int battery_pct = getBatteryLevel(discharging);
        int requested_interval = 10000; // default 10 seconds
        if (discharging && battery_pct >= 0 && battery_pct < 20) {
            requested_interval = 30000; // throttle frequency to 30 seconds
        }

        std::stringstream ss_resp;
        ss_resp << "HTTP/1.1 200 OK\r\n"
                << "Content-Type: application/json\r\n"
                << "Access-Control-Allow-Origin: *\r\n\r\n"
                << "{\"status\":\"ok\",\"requested_interval\":" << requested_interval << "}";
        std::string response = ss_resp.str();
        send(new_socket, response.c_str(), response.length(), 0);
        close(new_socket);

        if (isTelemetryPaused()) {
            std::cout << "[Edge Daemon] Ingestion paused. Ignoring telemetry point." << std::endl;
            continue;
        }

        double keystrokes_per_minute = 0.0;
        bool has_keystrokes = extractDouble(body, "keystrokes_per_minute", keystrokes_per_minute);

        double mouse_pixels_per_minute = 0.0;
        bool has_mouse = extractDouble(body, "mouse_pixels_per_minute", mouse_pixels_per_minute);

        auto process_event = [&](const std::string& name, double val) {
            TelemetryEvent event = { name, val };
            std::string cleaned_metric = obfuscateDomainOrCategory(event.metric_name);
            TelemetryEvent cleaned_event = { cleaned_metric, event.value };

            double cumulative = getCumulativeEpsilon24h(DB_PATH);
            double current_epsilon;
            double config_budget;
            {
                std::lock_guard<std::mutex> lock(g_config_mutex);
                current_epsilon = g_config.epsilon;
                config_budget = g_config.budget;
            }
            
            if (config_budget > 0.0 && cumulative >= config_budget) {
                std::cout << "[Privacy Budget Check] Limit exceeded (" << cumulative << "/" << config_budget 
                          << "). Ingestion paused for " << cleaned_event.metric_name << std::endl;
                return;
            } else if (config_budget > 0.0 && cumulative >= 0.8 * config_budget) {
                std::lock_guard<std::mutex> lock(g_config_mutex);
                current_epsilon = calculateAdjustedEpsilon(g_config.epsilon, g_config.budget, cumulative);
                std::cout << "[Privacy Budget Check] Approaching limit (" << cumulative << "/" << g_config.budget 
                          << "). Adjusting epsilon to " << current_epsilon << std::endl;
            }

            TelemetryEvent safe_event = anonymizeEvent(cleaned_event, current_epsilon);
            logPrivacyEpsilon(current_epsilon, DB_PATH);
            
            if (!std::isfinite(safe_event.value)) {
                std::cerr << "[Warning] safe_event.value is not finite (" << safe_event.value << ") for metric " << safe_event.metric_name << ". Clamping to 0.0." << std::endl;
                safe_event.value = 0.0;
            }
            
            std::cout << "\n--- ChromeOS Host Ingestion Cycle ---" << std::endl;
            std::cout << "Metric: " << event.metric_name << " | Raw value: " << event.value << std::endl;
            std::cout << "Cleaned/Obfuscated: " << safe_event.metric_name << std::endl;
            std::cout << "Transmitting:     " << safe_event.metric_name << " | Anonymized: " << safe_event.value << std::endl;
            
            if (isNetworkOnline()) {
                std::cout << "Network Status:   ONLINE" << std::endl;
                flushBuffer(DB_PATH);
                if (!runCurlSafe(safe_event.metric_name, safe_event.value)) {
                    std::cout << "Upload failed. Buffering event..." << std::endl;
                    if (!bufferEvent(safe_event, DB_PATH)) {
                        std::cerr << "[Warning] Failed to buffer event to database: " << safe_event.metric_name << std::endl;
                    }
                } else {
                    std::cout << "Uploaded successfully." << std::endl;
                }
            } else {
                std::cout << "Network Status:   OFFLINE | Buffering event..." << std::endl;
                if (!bufferEvent(safe_event, DB_PATH)) {
                    std::cerr << "[Warning] Failed to buffer event to database: " << safe_event.metric_name << std::endl;
                }
            }
        };

        if (has_keystrokes) {
            process_event("keystrokes_per_minute", keystrokes_per_minute);
        }
        if (has_mouse) {
            process_event("mouse_pixels_per_minute", mouse_pixels_per_minute);
        }
    }
    
    return 0;
}

