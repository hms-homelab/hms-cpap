#include "sleepbridge/BridgeSource.h"

#include <algorithm>
#include <cctype>

namespace hms_cpap::sleepbridge {

namespace {

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

}  // namespace

std::string formatForBrand(const std::string& brand) {
    std::string b = brand;
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (b.find("resmed") != std::string::npos) return "resmed";
    if (b == "bmc" || b.find("luna") != std::string::npos || b.find("react") != std::string::npos) return "bmc";
    return {};
}

std::optional<BridgeSource> bridgeSource(const Json::Value& status, const std::string& sender, std::string* why) {
    if (!status.isObject() || !status["bridges"].isArray()) {
        if (why) *why = "The Sleep Bridge receiver is off.";
        return std::nullopt;
    }
    for (const auto& b : status["bridges"]) {
        if (b.get("sender", "").asString() != sender) continue;
        BridgeSource s;
        s.local_dir = b.get("card_root", "").asString();
        if (s.local_dir.empty()) {
            if (why) *why = "This bridge has no folder yet.";
            return std::nullopt;
        }
        s.format = b.get("card_format", "").asString();
        if (s.format.empty() && b["learned"].isObject()) s.format = formatForBrand(b["learned"].get("brand", "").asString());
        if (s.format.empty() && status["learned_card"].isObject())
            s.format = formatForBrand(status["learned_card"].get("brand", "").asString());
        s.format_known = !s.format.empty();
        if (s.format.empty()) s.format = "resmed";
        return s;
    }
    if (why) *why = "No Sleep Bridge called " + sender + " has been heard here.";
    return std::nullopt;
}

std::string bridgeWatchFrame(const std::string& event, const std::string& sender, const Json::Value& status) {
    Json::Value v(Json::objectValue);
    v["t"]      = "sleepbridge";
    v["event"]  = event;
    v["sender"] = sender;
    v["status"] = status;
    return compact(v);
}

std::string streamWatchFrame(const std::string& event, const Json::Value& stream) {
    Json::Value v(Json::objectValue);
    v["t"]      = "cpapdash";
    v["event"]  = event;
    v["stream"] = stream;
    return compact(v);
}

}  // namespace hms_cpap::sleepbridge
