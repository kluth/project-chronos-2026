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
#include "differential_privacy.h"

// Helper to execute OS-level command to push to Firebase
std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

int main() {
    std::cout << "[Edge Daemon] Initializing Chronos Local Tracker (Crostini Escaper Mode)..." << std::endl;
    std::cout << "[Edge Daemon] Enforcing epsilon-Differential Privacy." << std::endl;
    
    double epsilon = 0.5; // High privacy regime

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

        // Send a simple CORS-friendly HTTP Response back to the Chrome Extension
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n\r\n{\"status\":\"ok\"}";
        send(new_socket, response.c_str(), response.length(), 0);
        close(new_socket);

        // Fast JSON extraction: {"window":"Active Tab Title"}
        std::string active_window = "";
        size_t pos = request.find("\"window\":\"");
        if (pos != std::string::npos) {
            size_t end_pos = request.find("\"", pos + 10);
            if (end_pos != std::string::npos) {
                active_window = request.substr(pos + 10, end_pos - (pos + 10));
            }
        }

        if (!active_window.empty()) {
            // We count 1 active ping = 10 seconds of usage on this window
            TelemetryEvent event = { active_window, 10.0 };
            
            // Apply differential privacy BEFORE it ever leaves the device
            TelemetryEvent safe_event = anonymizeEvent(event, epsilon);
            
            std::cout << "\n--- ChromeOS Host Ingestion Cycle ---" << std::endl;
            std::cout << "Real Host Window: " << event.metric_name << " | Raw duration: " << event.value << "s" << std::endl;
            std::cout << "Transmitting:     " << safe_event.metric_name << " | Anonymized: " << safe_event.value << "s" << std::endl;
            
            // Transmit the safe data via curl to the DSGVO-compliant Firebase Backend
            std::string curl_cmd = "curl -s -X POST -H \"Content-Type: application/json\" -d '{\"metric\":\"" + safe_event.metric_name + "\",\"val\":" + std::to_string(safe_event.value) + "}' https://europe-west3-project-chronos-2026.cloudfunctions.net/ingestTelemetry > /dev/null";
            exec(curl_cmd.c_str());
        }
    }
    
    return 0;
}
