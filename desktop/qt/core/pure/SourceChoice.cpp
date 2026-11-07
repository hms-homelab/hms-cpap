#include "SourceChoice.h"

#include <cstdio>

namespace cpapdash::supervisor {

namespace {

/// The four octets of a dotted IPv4 address, or false.
bool octets(const std::string& ip, int out[4]) {
    char tail = 0;
    if (std::sscanf(ip.c_str(), "%d.%d.%d.%d%c", &out[0], &out[1], &out[2], &out[3], &tail) != 4)
        return false;
    for (int i = 0; i < 4; ++i)
        if (out[i] < 0 || out[i] > 255) return false;
    return true;
}

}  // namespace

void applySourceChoice(ConfigModel& model, SourceChoice choice, const SourceInputs& in) {
    switch (choice) {
        case SourceChoice::Local:
            model.set("source", "local");
            model.set("local_dir", in.local_dir);
            break;
        case SourceChoice::FlashAir:
            model.set("source", "flashair");
            model.set("flashair_url", in.flashair_url);
            break;
        case SourceChoice::Mule:
            model.set("source", "ezshare");
            model.set("ezshare_url", in.ezshare_url);
            break;
        case SourceChoice::SleepBridge:
            return;   // nothing, not even archive_dir
    }
    model.set("archive_dir", in.archive_dir);
}

SourceChoice initialSourceChoice(const ConfigModel& model) {
    const auto src = model.getString("source");
    if (src == "local")    return SourceChoice::Local;
    if (src == "flashair") return SourceChoice::FlashAir;
    return SourceChoice::SleepBridge;
}

int sleepBridgePort(const ConfigModel& model) {
    const int p = model.getInt("sleep_bridge.port", kSleepBridgeDefaultPort);
    return (p > 0 && p <= 65535) ? p : kSleepBridgeDefaultPort;
}

std::string lanAddress(const std::vector<std::string>& ipv4) {
    std::string fallback;
    for (const auto& ip : ipv4) {
        int o[4];
        if (!octets(ip, o)) continue;
        if (o[0] == 127 || o[0] == 0) continue;            // loopback, unspecified
        if (o[0] == 169 && o[1] == 254) continue;          // link-local: no DHCP
        const bool is_private = o[0] == 10 ||
                                (o[0] == 172 && o[1] >= 16 && o[1] <= 31) ||
                                (o[0] == 192 && o[1] == 168);
        if (is_private) return ip;
        if (fallback.empty()) fallback = ip;
    }
    return fallback;
}

std::string sleepBridgeNote(const std::string& address, int port) {
    std::string note = "Nothing to set up here: HmsCpap is already listening for your "
                       "Sleep Bridge.\n\n";
    note += address.empty()
        ? "On the bridge's own setup page, set it to stream to this computer's IP "
          "address, port " + std::to_string(port) + "."
        : "On the bridge's own setup page, set it to stream to " + address + ":" +
          std::to_string(port) + ".";
    note += "\n\nIts nights then land in the Sleep Bridge's own folder automatically.";
    return note;
}

}  // namespace cpapdash::supervisor
