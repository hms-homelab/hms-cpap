#include "BridgeSetup.h"

#include "SourceChoice.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace cpapdash::supervisor {

namespace {

bool usable(const std::string& ip, bool* is_private) {
    int o[4];
    char tail = 0;
    if (std::sscanf(ip.c_str(), "%d.%d.%d.%d%c", &o[0], &o[1], &o[2], &o[3], &tail) != 4) return false;
    for (int i = 0; i < 4; ++i)
        if (o[i] < 0 || o[i] > 255) return false;
    if (o[0] == 127 || o[0] == 0 || (o[0] == 169 && o[1] == 254)) return false;
    *is_private = o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168);
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

long long num(const nlohmann::json& o, const char* key) {
    if (!o.is_object() || !o.contains(key) || !o[key].is_number()) return 0;
    return o[key].get<long long>();
}

std::string str(const nlohmann::json& o, const char* key) {
    if (!o.is_object() || !o.contains(key) || !o[key].is_string()) return {};
    return o[key].get<std::string>();
}

}  // namespace

AddressChoice chooseBridgeAddress(const std::vector<std::string>& ipv4, const std::string& default_route_ip) {
    AddressChoice c;
    bool priv = false;
    const bool listed = std::find(ipv4.begin(), ipv4.end(), default_route_ip) != ipv4.end();
    if (listed && usable(default_route_ip, &priv)) c.primary = default_route_ip;
    else c.primary = lanAddress(ipv4);

    std::vector<std::string> pub;
    for (const auto& ip : ipv4) {
        if (ip == c.primary || !usable(ip, &priv)) continue;
        if (std::find(c.others.begin(), c.others.end(), ip) != c.others.end() ||
            std::find(pub.begin(), pub.end(), ip) != pub.end())
            continue;
        (priv ? c.others : pub).push_back(ip);
    }
    c.others.insert(c.others.end(), pub.begin(), pub.end());
    return c;
}

bool mentionPort(int port) { return port != kSleepBridgeDefaultPort; }

bool looksLikeCpapCard(const std::vector<std::string>& root_names) {
    bool str_edf = false, datalog = false, ident = false;
    for (const auto& n : root_names) {
        const std::string l = lower(n);
        if (l == "str.edf") str_edf = true;
        else if (l == "datalog") datalog = true;
        else if (l == "identification.tgt" || l == "identification.json") ident = true;
    }
    return (str_edf && datalog) || ident;
}

std::string arrivedBridge(const nlohmann::json& frame, long long since) {
    if (!frame.is_object() || str(frame, "t") != "sleepbridge") return {};
    const std::string event = str(frame, "event");
    if (event == "hello") return str(frame, "sender");
    if (event != "snapshot") return {};
    const auto& status = frame.contains("status") ? frame["status"] : nlohmann::json();
    if (!status.is_object() || !status.contains("bridges") || !status["bridges"].is_array()) return {};
    for (const auto& b : status["bridges"]) {
        const bool online = b.is_object() && b.contains("online") && b["online"].is_boolean() && b["online"].get<bool>();
        const long long started = num(b, "session_started_at");
        if (online && started > 0 && started >= since) return str(b, "sender");
    }
    return {};
}

nlohmann::json bridgeEntry(const nlohmann::json& status, const std::string& sender) {
    if (!status.is_object() || !status.contains("bridges") || !status["bridges"].is_array()) return nullptr;
    for (const auto& b : status["bridges"])
        if (str(b, "sender") == sender) return b;
    return nullptr;
}

CardSeen cardSeen(const nlohmann::json& status, const std::string& sender, long long stated_before) {
    const auto b = bridgeEntry(status, sender);
    if (b.is_null() || num(b, "card_stated_at") <= stated_before) return CardSeen::Waiting;
    if (b.contains("learned") && b["learned"].is_object() && str(b["learned"], "verdict") == "mismatch")
        return CardSeen::Mismatch;
    return CardSeen::Seen;
}

std::string formatToFix(const nlohmann::json& status, const std::string& sender, const std::string& format_set) {
    const std::string now = str(bridgeEntry(status, sender), "card_format");
    return !now.empty() && now != format_set ? now : std::string();
}

}  // namespace cpapdash::supervisor
