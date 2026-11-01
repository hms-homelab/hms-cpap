#include "services/As11Settings.h"

#include "clients/As11Stream.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace hms_cpap {

namespace {

// "+05:30" / "-07:00" as minutes east of UTC; false when it is neither.
bool offsetOf(const std::string& s, int* minutes) {
    if (s.size() != 6 || (s[0] != '+' && s[0] != '-') || s[3] != ':') return false;
    for (size_t i : {1u, 2u, 4u, 5u})
        if (s[i] < '0' || s[i] > '9') return false;
    const int m = (s[1] - '0') * 600 + (s[2] - '0') * 60 + (s[4] - '0') * 10 + (s[5] - '0');
    *minutes = s[0] == '-' ? -m : m;
    return true;
}

const json* find(const json& j, std::initializer_list<const char*> path) {
    const json* at = &j;
    for (const char* key : path) {
        if (!at->is_object()) return nullptr;
        auto it = at->find(key);
        if (it == at->end()) return nullptr;
        at = &*it;
    }
    return at;
}

std::optional<double> number(const json& j, std::initializer_list<const char*> path) {
    const json* v = find(j, path);
    if (v && v->is_number()) return v->get<double>();
    return std::nullopt;
}

std::optional<std::string> text(const json& j, std::initializer_list<const char*> path) {
    const json* v = find(j, path);
    if (v && v->is_string()) return v->get<std::string>();
    return std::nullopt;
}

// A switch as the STR holds it: 1 off, 2 on. Anything else says nothing.
std::optional<double> toggle(const std::optional<std::string>& s) {
    if (!s) return std::nullopt;
    if (*s == "Off" || *s == "No") return 1.0;
    if (*s == "On" || *s == "Yes") return 2.0;
    return std::nullopt;
}

// The CRC-32 a card keeps beside CurrentSettings.json, in CurrentSettings.crc:
// four bytes, little-endian (both of one card's .crc files check).
uint32_t crc32Of(const std::string& bytes) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : bytes) {
        c ^= ch;
        for (int b = 0; b < 8; b++) c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    return ~c;
}

// <settings path without .json>.crc for the file as it is now; false on failure.
bool writeCrcBeside(const fs::path& json_path) {
    std::ifstream in(json_path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    const uint32_t crc = crc32Of(ss.str());
    fs::path path = json_path;
    path.replace_extension(".crc");
    const fs::path tmp = path.string() + ".tmp";
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const char le[4] = {static_cast<char>(crc & 0xFF), static_cast<char>((crc >> 8) & 0xFF),
                            static_cast<char>((crc >> 16) & 0xFF), static_cast<char>((crc >> 24) & 0xFF)};
        out.write(le, 4);
        if (!out) { out.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

int64_t floorDiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

}  // namespace

int64_t As11Settings::firstDay() const {
    // The sleep day is noon to noon on the machine's clock: twelve hours back
    // from the local time lands on the day it started.
    const int64_t local_ms = applied_ms + static_cast<int64_t>(offset_min) * 60'000LL - 12 * 3'600'000LL;
    return floorDiv(local_ms, 86'400'000LL);
}

std::string settingsPath(const std::string& root) {
    return (fs::path(root) / "SETTINGS" / "CurrentSettings.json").string();
}

std::optional<As11Settings> parseSettings(const std::string& json_text) {
    const json top = json::parse(json_text, nullptr, false);
    if (top.is_discarded() || !top.is_object()) return std::nullopt;

    const json* profiles = find(top, {"FlowGenerator", "SettingProfiles"});
    if (!profiles) profiles = find(top, {"SettingProfiles"});
    if (!profiles || !profiles->is_object()) return std::nullopt;
    const json& p = *profiles;

    As11Settings s;
    auto put = [&](const char* label, std::optional<double> v) {
        if (v) s.values.emplace_back(label, *v);
    };

    if (auto when = text(p, {"Attributes", "AppliedDateTime"})) s.applied_ms = as11::isoToMs(*when);
    // With no (or an unreadable) AppliedDateTime there is no day to start from:
    // 0 would date them 1970 and put them on every past day. Skip them.
    if (s.applied_ms <= 0) return std::nullopt;
    if (auto tz = text(p, {"FeatureProfiles", "TimeZoneFeature", "TimeZoneOffset"})) {
        int m = 0;
        if (offsetOf(*tz, &m)) s.offset_min = m;
    }

    // ── the therapy mode: the one code seen on a card ──
    if (auto active = text(p, {"ActiveProfiles", "TherapyProfile"})) {
        if (const json* prof = find(p, {"TherapyProfiles", active->c_str()})) {
            if (auto mode = text(*prof, {"TherapyMode"}); mode && *mode == "AutoSet") put("Mode", 1.0);
        }
    }

    // ── the pressures, of all three profiles whichever is active ──
    put("S.C.StartPress", number(p, {"TherapyProfiles", "CpapProfile", "StartPressure"}));
    put("S.C.Press", number(p, {"TherapyProfiles", "CpapProfile", "SetPressure"}));
    put("S.A.StartPress", number(p, {"TherapyProfiles", "AutoSetProfile", "StartPressure"}));
    put("S.A.MaxPress", number(p, {"TherapyProfiles", "AutoSetProfile", "MaxPressure"}));
    put("S.A.MinPress", number(p, {"TherapyProfiles", "AutoSetProfile", "MinPressure"}));
    put("S.AFH.StartPress", number(p, {"TherapyProfiles", "AutoSetForHerProfile", "StartPressure"}));
    put("S.AFH.MaxPress", number(p, {"TherapyProfiles", "AutoSetForHerProfile", "MaxPressure"}));
    put("S.AFH.MinPress", number(p, {"TherapyProfiles", "AutoSetForHerProfile", "MinPressure"}));

    // ── the features ──
    put("S.AS.Comfort", toggle(text(p, {"FeatureProfiles", "ComfortFeature", "AutoSetComfort"})));
    put("S.RampEnable", toggle(text(p, {"FeatureProfiles", "AutoRampFeature", "RampEnable"})));
    put("S.RampTime", number(p, {"FeatureProfiles", "AutoRampFeature", "RampTime"}));
    put("S.EPR.ClinEnable", toggle(text(p, {"FeatureProfiles", "EprFeature", "EprEnablePatientAccess"})));
    put("S.EPR.EPREnable", toggle(text(p, {"FeatureProfiles", "EprFeature", "EprEnable"})));
    put("S.EPR.Level", number(p, {"FeatureProfiles", "EprFeature", "EprPressure"}));
    if (auto t = text(p, {"FeatureProfiles", "EprFeature", "EprType"}); t && *t == "FullTime")
        put("S.EPR.EPRType", 2.0);
    put("S.SmartStart", toggle(text(p, {"FeatureProfiles", "SmartStartStopFeature", "SmartStart"})));
    put("S.ABFilter", toggle(text(p, {"FeatureProfiles", "CircuitFeature", "AntiBacterialFilter"})));
    if (auto t = text(p, {"FeatureProfiles", "CircuitFeature", "MaskType"}); t && *t == "Pillows")
        put("S.Mask", 2.0);
    if (auto t = text(p, {"FeatureProfiles", "CircuitFeature", "TubeType"}); t && *t == "15mmNonHeated")
        put("S.Tube", 3.0);
    if (auto t = text(p, {"FeatureProfiles", "ClimateFeature", "ClimateControl"}); t && *t == "Auto")
        put("S.ClimateControl", 1.0);
    put("S.HumEnable", toggle(text(p, {"FeatureProfiles", "ClimateFeature", "HumidifierSettingEnable"})));
    put("S.HumLevel", number(p, {"FeatureProfiles", "ClimateFeature", "HumidifierLevel"}));
    put("S.TempEnable", toggle(text(p, {"FeatureProfiles", "ClimateFeature", "HeatedTubeSettingEnable"})));
    put("S.Temp", number(p, {"FeatureProfiles", "ClimateFeature", "HeatedTubeTemperature"}));

    if (s.values.empty()) return std::nullopt;
    return s;
}

std::optional<As11Settings> readSettings(const std::string& root) {
    std::ifstream in(settingsPath(root), std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return parseSettings(ss.str());
}

bool writeSettings(const std::string& root, const std::string& result_json) {
    const auto fresh = parseSettings(result_json);
    if (!fresh) return false;
    if (const auto have = readSettings(root); have && have->applied_ms >= fresh->applied_ms) {
        // The file there stays (a card's copy, say); one written before the
        // .crc existed gets its .crc, and a card's own is kept.
        std::error_code ec;
        fs::path crc = settingsPath(root);
        crc.replace_extension(".crc");
        if (!fs::exists(crc, ec)) writeCrcBeside(settingsPath(root));
        return false;
    }

    const fs::path path = settingsPath(root);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Laid out as a card's file: the object under "FlowGenerator", key order as sent.
    const std::string body = "{\"FlowGenerator\":" + result_json + "}";
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << body;
        if (!out) { out.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    writeCrcBeside(path);   // CurrentSettings.crc, as the card keeps it
    return true;
}

}  // namespace hms_cpap
