#include "sleepbridge/BridgeSource.h"

#include "services/SefamIngestion.h"   // SefamIngestion::holdsCard

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace hms_cpap::sleepbridge {

namespace fs = std::filesystem;

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

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
    // SDD-080 D1: a DreamStation's card.
    if (b.find("philips") != std::string::npos || b.find("respironics") != std::string::npos ||
        b.find("dreamstation") != std::string::npos)
        return "philips";
    // A Sefam card learned on a computer (the upload page's name for it).
    if (b.find("sefam") != std::string::npos) return "sefam";
    return {};
}

bool holdsSefamCard(const std::string& dir) {
    // A ResMed card is not walked: the bridge status asks this on every
    // answer, and a DATALOG holds hundreds of night folders.
    std::error_code ec;
    if (dir.empty() || fs::exists(fs::path(dir) / "DATALOG", ec) ||
        fs::exists(fs::path(dir) / "STR.edf", ec))
        return false;
    return SefamIngestion::holdsCard(dir);
}

bool holdsPhilipsCard(const std::string& dir) {
    // P-SERIES/<machine>/ with its identity file, any case, as the Philips
    // card reader finds one (PhilipsCardIngestion::findCardDir).
    std::error_code ec;
    for (fs::directory_iterator top(dir, ec), end; !ec && top != end; top.increment(ec)) {
        if (!top->is_directory(ec) || upper(top->path().filename().string()) != "P-SERIES") continue;
        for (fs::directory_iterator m(top->path(), ec); !ec && m != end; m.increment(ec)) {
            if (!m->is_directory(ec)) continue;
            for (fs::directory_iterator f(m->path(), ec); !ec && f != end; f.increment(ec)) {
                const auto n = upper(f->path().filename().string());
                if (n == "PROP.BIN" || n == "PROP.TXT") return true;
            }
        }
    }
    return false;
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
        // SDD-080 D1: the rebuild names ResMed and Luna cards only; a
        // DreamStation 2's folder holds P-SERIES/, read as a Philips card
        // (through OSCAR when encrypted), never as the ResMed default.
        if (s.format.empty() && holdsPhilipsCard(s.local_dir)) s.format = "philips";
        if (s.format.empty() && holdsSefamCard(s.local_dir)) s.format = "sefam";
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
