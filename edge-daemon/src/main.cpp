#include <iostream>
#include <string>
#include <array>
#include <memory>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include "differential_privacy.h"

// Helper to parse double safely
bool parseDouble(const std::string& str, double& val) {
    std::stringstream ss(str);
    return (ss >> val) && ss.eof();
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

        char buffer[2048] = {0};
        read(new_socket, buffer, 2048);
        std::string request(buffer);

        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"ok\"}";
        send(new_socket, response.c_str(), response.length(), 0);
        close(new_socket);

        std::string active_window = "";
        size_t pos = request.find("\"window\":\"");
        if (pos != std::string::npos) {
            size_t end_pos = request.find("\"", pos + 10);
            if (end_pos != std::string::npos) {
                active_window = request.substr(pos + 10, end_pos - (pos + 10));
            }
        }

        if (!active_window.empty()) {
            TelemetryEvent event = { active_window, 10.0 };
            
            // Clean/obfuscate title or URL before adding noise
            std::string cleaned_metric = obfuscateDomainOrCategory(event.metric_name);
            TelemetryEvent cleaned_event = { cleaned_metric, event.value };

            // Apply differential privacy
            TelemetryEvent safe_event = anonymizeEvent(cleaned_event, g_config.epsilon);
            
            std::cout << "\n--- ChromeOS Host Ingestion Cycle ---" << std::endl;
            std::cout << "Real Host Window: " << event.metric_name << " | Raw duration: " << event.value << "s" << std::endl;
            std::cout << "Cleaned/Obfuscated: " << safe_event.metric_name << std::endl;
            std::cout << "Transmitting:     " << safe_event.metric_name << " | Anonymized: " << safe_event.value << "s" << std::endl;
            
            if (isNetworkOnline()) {
                std::cout << "Network Status:   ONLINE" << std::endl;
                // Flush SQLite buffer FIFO
                flushBuffer(DB_PATH);
                // Attempt to send current event
                if (!runCurlSafe(safe_event.metric_name, safe_event.value)) {
                    std::cout << "Upload failed. Buffering event..." << std::endl;
                    bufferEvent(safe_event, DB_PATH);
                } else {
                    std::cout << "Uploaded successfully." << std::endl;
                }
            } else {
                std::cout << "Network Status:   OFFLINE | Buffering event..." << std::endl;
                bufferEvent(safe_event, DB_PATH);
            }
        }
    }
    
    return 0;
}

