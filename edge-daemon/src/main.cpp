#include <iostream>
#include <string>
#include <array>
#include <memory>
#include <thread>
#include <chrono>
#include "differential_privacy.h"

// Helper to execute OS-level command to get active window
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
    std::cout << "[Edge Daemon] Initializing Chronos Local Tracker..." << std::endl;
    std::cout << "[Edge Daemon] Enforcing epsilon-Differential Privacy." << std::endl;
    
    double epsilon = 0.5; // High privacy regime

    // Tracking loop
    while (true) {
        // Query actual X11 active window using xdotool
        // Requires xdotool installed on the user's Linux machine
        std::string active_window;
        try {
            active_window = exec("xdotool getactivewindow getwindowname 2>/dev/null");
            if (active_window.empty()) active_window = "Unknown/Idle\n";
        } catch (...) {
            active_window = "Error fetching window\n";
        }

        // Strip newline
        if (!active_window.empty() && active_window.back() == '\n') {
            active_window.pop_back();
        }

        // We count 1 active ping = 10 seconds of usage on this window
        TelemetryEvent event = { active_window, 10.0 };
        
        // Apply differential privacy BEFORE it ever leaves the device
        TelemetryEvent safe_event = anonymizeEvent(event, epsilon);
        
        std::cout << "\n--- Ingestion Cycle ---" << std::endl;
        std::cout << "Real Window (Local Only): " << event.metric_name << " | Raw duration: " << event.value << "s" << std::endl;
        std::cout << "Transmitting (Noised):    " << safe_event.metric_name << " | Anonymized duration: " << safe_event.value << "s" << std::endl;
        
        // Example of transmitting the safe data via curl to Firebase Backend
        // std::string curl_cmd = "curl -s -X POST -H \"Content-Type: application/json\" -d '{\"metric\":\"" + safe_event.metric_name + "\",\"val\":" + std::to_string(safe_event.value) + "}' https://project-chronos.cloudfunctions.net/ingest > /dev/null";
        // exec(curl_cmd.c_str());

        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
    
    return 0;
}
