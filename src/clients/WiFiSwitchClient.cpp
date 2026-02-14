#include "clients/WiFiSwitchClient.h"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <array>
#include <memory>
#include <thread>
#include <chrono>

namespace hms_cpap {

static const char* WPA_CONF   = "/tmp/hms_cpap_wpa.conf";
static const char* WPA_PID    = "/tmp/hms_cpap_wpa.pid";
static const char* WPA_CTRL   = "/tmp/hms_cpap_wpa";   // control socket dir

WiFiSwitchClient::WiFiSwitchClient(const std::string& interface)
    : interface_(interface) {
}

std::string WiFiSwitchClient::execCommand(const std::string& command) const {
    std::array<char, 256> buffer;
    std::string result;

    std::unique_ptr<FILE, int(*)(FILE*)> pipe(
        popen(command.c_str(), "r"), pclose
    );
    if (!pipe) return "";

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

bool WiFiSwitchClient::execCommandStatus(const std::string& command) const {
    return system(command.c_str()) == 0;
}

bool WiFiSwitchClient::isEzShareReachable() const {
    // Check if 192.168.4.1 is already reachable (via GL.iNet repeater or direct connection)
    // This allows us to skip WiFi switching entirely if already connected
    std::string result = execCommand("ping -c 1 -W 1 192.168.4.1 2>&1 | grep '64 bytes'");
    return !result.empty();
}

bool WiFiSwitchClient::connect(const std::string& ssid,
                                const std::string& password,
                                int timeout_sec) {
    // Check if ez Share is already reachable (e.g., via GL.iNet repeater)
    if (isEzShareReachable()) {
        std::cout << "WiFi: ez Share (192.168.4.1) already reachable, skipping WiFi switching" << std::endl;
        return true;
    }

    std::cout << "WiFi: Connecting " << interface_ << " to " << ssid << std::endl;

    // Make sure any previous session is cleaned up
    disconnect();

    // 1. Write temporary wpa_supplicant config
    {
        std::ofstream conf(WPA_CONF);
        if (!conf.is_open()) {
            std::cerr << "WiFi: Cannot write " << WPA_CONF << std::endl;
            return false;
        }
        conf << "ctrl_interface=" << WPA_CTRL << "\n"
             << "network={\n"
             << "    ssid=\"" << ssid << "\"\n"
             << "    psk=\"" << password << "\"\n"
             << "}\n";
    }

    // 2. Bring interface up
    if (!execCommandStatus("ip link set " + interface_ + " up")) {
        std::cerr << "WiFi: Failed to bring up " << interface_ << std::endl;
        return false;
    }

    // 3. Start dedicated wpa_supplicant for this interface
    //    -B = background, -P = pid file, -C = ctrl socket dir
    std::string cmd = "wpa_supplicant"
                      " -i " + interface_ +
                      " -c " + WPA_CONF +
                      " -B -P " + WPA_PID +
                      " -D nl80211,wext"
                      " 2>/dev/null";

    if (!execCommandStatus(cmd)) {
        std::cerr << "WiFi: wpa_supplicant failed to start" << std::endl;
        return false;
    }

    // 4. Wait for WPA association
    for (int i = 0; i < timeout_sec; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::string status = execCommand(
            "wpa_cli -p " + std::string(WPA_CTRL) +
            " -i " + interface_ + " status 2>/dev/null"
        );

        if (status.find("wpa_state=COMPLETED") != std::string::npos) {
            std::cout << "WiFi: Associated with " << ssid << std::endl;

            // 5. Get IP via dhcpcd
            //    -G = do NOT touch default route (preserve eno1 gateway!)
            //    -K = don't send hostname
            //    ez Share DHCP takes ~3-4 seconds to lease IP
            execCommandStatus("dhcpcd -G -K " + interface_ + " 2>/dev/null");
            std::this_thread::sleep_for(std::chrono::seconds(5));

            // 5a. Add route to 192.168.4.0/24 (dhcpcd -G prevented automatic route)
            //     This allows reaching ez Share (192.168.4.1) via GL.iNet repeater
            execCommandStatus("ip route add 192.168.4.0/24 dev " + interface_ + " 2>/dev/null");

            // 6. Verify we got an IP (not link-local 169.254.x.x)
            std::string ip_check = execCommand("ip -4 addr show " + interface_ + " | grep 'inet ' | awk '{print $2}' | cut -d/ -f1");
            if (ip_check.empty() || ip_check.find("169.254") == 0) {
                std::cerr << "WiFi: Failed to get DHCP IP" << std::endl;
                disconnect();
                return false;
            }

            std::cout << "WiFi: Connected to " << ssid << " (IP: " << ip_check << ")" << std::endl;
            return true;
        }
    }

    std::cerr << "WiFi: Association timed out after " << timeout_sec << "s" << std::endl;
    disconnect();
    return false;
}

bool WiFiSwitchClient::disconnect() {
    std::cout << "WiFi: Disconnecting " << interface_ << std::endl;

    // Release DHCP lease (-x = exit/release, -G preserved from connection)
    execCommandStatus("dhcpcd -x " + interface_ + " 2>/dev/null");

    // Terminate our wpa_supplicant (uses our control socket, won't touch system one)
    execCommandStatus(
        "wpa_cli -p " + std::string(WPA_CTRL) +
        " -i " + interface_ + " terminate 2>/dev/null"
    );

    // Clean up pid file in case wpa_cli terminate didn't remove it
    std::remove(WPA_PID);

    // Remove route to 192.168.4.0/24 (if we added it)
    execCommandStatus("ip route del 192.168.4.0/24 dev " + interface_ + " 2>/dev/null");

    // Bring interface down
    execCommandStatus("ip link set " + interface_ + " down 2>/dev/null");

    std::cout << "WiFi: " << interface_ << " down" << std::endl;
    return true;
}

bool WiFiSwitchClient::isReachable(const std::string& ip, int timeout_sec) const {
    std::string cmd = "ping -c 1 -W " + std::to_string(timeout_sec) +
                      " -I " + interface_ + " " + ip + " >/dev/null 2>&1";
    return execCommandStatus(cmd);
}

bool WiFiSwitchClient::isConnected() const {
    std::string status = execCommand(
        "wpa_cli -p " + std::string(WPA_CTRL) +
        " -i " + interface_ + " status 2>/dev/null"
    );
    return status.find("wpa_state=COMPLETED") != std::string::npos;
}

} // namespace hms_cpap
