#pragma once

#include <string>

namespace hms_cpap {

/**
 * WiFiSwitchClient - Manages USB WiFi dongle for ez Share burst connections
 *
 * Uses wpa_supplicant + dhcpcd directly (no NetworkManager).
 * Primary network is wired (eno1), so WiFi dongle is dedicated to ez Share.
 * dhcpcd.conf is pre-configured with metric 9999 + nogateway for the dongle,
 * so the default route always stays on eno1.
 */
class WiFiSwitchClient {
public:
    explicit WiFiSwitchClient(const std::string& interface);

    /**
     * Connect dongle to a WiFi network.
     * Starts a dedicated wpa_supplicant, waits for association, gets DHCP.
     */
    bool connect(const std::string& ssid, const std::string& password, int timeout_sec = 15);

    /**
     * Disconnect and tear down the dongle.
     * Releases DHCP, terminates wpa_supplicant, brings interface down.
     */
    bool disconnect();

    /** Check if a specific IP responds to ping. */
    bool isReachable(const std::string& ip, int timeout_sec = 3) const;

    /** Check if the dongle is currently associated to a network. */
    bool isConnected() const;

    /** Check if ez Share (192.168.4.1) is already reachable (e.g., via GL.iNet repeater). */
    bool isEzShareReachable() const;

private:
    std::string interface_;

    std::string execCommand(const std::string& command) const;
    bool execCommandStatus(const std::string& command) const;
};

} // namespace hms_cpap
