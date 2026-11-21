#include "utils/MachineIdentity.h"
#include "utils/AppConfig.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <vector>

namespace hms_cpap {

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Identification.json: FlowGenerator.IdentificationProfiles.{Product,Software}.
MachineIdentity fromJson(const std::filesystem::path& path) {
    MachineIdentity id;
    try {
        std::ifstream f(path);
        const auto j = nlohmann::json::parse(f);
        const auto& profiles = j.at("FlowGenerator").at("IdentificationProfiles");
        auto text = [](const nlohmann::json& obj, const char* key) -> std::string {
            if (!obj.is_object() || !obj.contains(key) || !obj[key].is_string()) return "";
            return trim(obj[key].get<std::string>());
        };
        const auto product = profiles.value("Product", nlohmann::json::object());
        const auto software = profiles.value("Software", nlohmann::json::object());
        id.model = readableModel(text(product, "ProductName"));
        id.serial = text(product, "SerialNumber");
        id.firmware = text(software, "ApplicationIdentifier");
    } catch (const std::exception&) {
        return {};
    }
    id.manufacturer = "ResMed";
    return id;
}

/// Identification.tgt: lines of "#KEY value". #PNA is the product name, #SRN
/// the serial, #SID the software the machine runs.
MachineIdentity fromTgt(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return {};
    MachineIdentity id;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.size() < 5 || line[0] != '#') continue;
        const std::string key = line.substr(1, 3);
        const std::string value = trim(line.substr(4));
        if (key == "PNA")      id.model = readableModel(value);
        else if (key == "SRN") id.serial = value;
        else if (key == "SID") id.firmware = value;
    }
    if (id.empty()) return {};
    id.manufacturer = "ResMed";
    return id;
}

/// The serial a ResMed EDF carries in its header's recording field (bytes 88
/// to 167, "... SRN=23243570851 MID=..."), from the newest DATALOG night that
/// has an EDF. For a card with no identification file: a Sleep Bridge's card is
/// rebuilt from what the machine writes during therapy, and the machine writes
/// Identification.tgt only when the card is set up, so a tapper's card never
/// has one (hmscpap-tapper41, 2026-11-18: the CpapDash uploader held every file
/// of the night, "names no machine serial yet"). Serial only: the header names
/// no model.
MachineIdentity fromEdfHeader(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> nights;
    for (const auto& e : fs::directory_iterator(root / "DATALOG", ec)) {
        const auto name = e.path().filename().string();
        std::error_code dec;
        if (name.size() == 8 && std::all_of(name.begin(), name.end(), ::isdigit) && e.is_directory(dec))
            nights.push_back(e.path());
    }
    std::sort(nights.rbegin(), nights.rend());   // newest night first
    for (const auto& night : nights) {
        std::vector<fs::path> edfs;
        std::error_code nec;
        for (const auto& f : fs::directory_iterator(night, nec)) {
            std::string ext = f.path().extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".edf") edfs.push_back(f.path());
        }
        std::sort(edfs.begin(), edfs.end());
        for (const auto& edf : edfs) {
            std::ifstream in(edf, std::ios::binary);
            char header[256] = {};
            if (!in.read(header, sizeof(header))) continue;
            const std::string recording(header + 88, 80);
            const auto at = recording.find("SRN=");
            if (at == std::string::npos) continue;
            const auto end = recording.find(' ', at + 4);
            const std::string serial = trim(recording.substr(at + 4, end == std::string::npos ? std::string::npos : end - at - 4));
            if (serial.empty() || !std::all_of(serial.begin(), serial.end(), ::isalnum)) continue;
            MachineIdentity id;
            id.manufacturer = "ResMed";
            id.serial = serial;
            return id;
        }
    }
    return {};
}

std::string manufacturerName(DeviceManufacturer m) {
    switch (m) {
        case DeviceManufacturer::RESMED:     return "ResMed";
        case DeviceManufacturer::LOWENSTEIN: return "Löwenstein";
        case DeviceManufacturer::PHILIPS:    return "Philips";
        case DeviceManufacturer::BMC:        return "BMC";
        case DeviceManufacturer::SEFAM:      return "Sefam";
        case DeviceManufacturer::UNKNOWN:    break;
    }
    return "";
}

}  // namespace

std::string readableModel(const std::string& product_name) {
    std::string out;
    const std::string in = trim(product_name);
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '_') {
            out += ' ';
            continue;
        }
        if (i > 0) {
            const unsigned char prev = static_cast<unsigned char>(in[i - 1]);
            const unsigned char cur = static_cast<unsigned char>(c);
            if ((std::isalpha(prev) && std::isdigit(cur)) ||
                (std::isdigit(prev) && std::isalpha(cur)))
                out += ' ';
        }
        out += c;
    }
    // One space where an underscore met a boundary, never two.
    std::string collapsed;
    for (char c : out) {
        if (c == ' ' && !collapsed.empty() && collapsed.back() == ' ') continue;
        collapsed += c;
    }
    return trim(collapsed);
}

MachineIdentity readIdentification(const std::string& card_root) {
    if (card_root.empty()) return {};
    const std::filesystem::path root(card_root);
    std::error_code ec;
    const auto json = root / "Identification.json";
    const auto tgt = root / "Identification.tgt";
    MachineIdentity from_json;
    if (std::filesystem::is_regular_file(json, ec)) from_json = fromJson(json);
    MachineIdentity from_tgt;
    if (std::filesystem::is_regular_file(tgt, ec)) from_tgt = fromTgt(tgt);
    if (!from_json.empty() && !from_tgt.empty() && !from_json.serial.empty() &&
        !from_tgt.serial.empty() && from_json.serial != from_tgt.serial) {
        // #94: one card, two machines. The machine writing the card now names
        // itself in every EDF header it writes.
        if (fromEdfHeader(root).serial == from_tgt.serial) return from_tgt;
        return from_json;
    }
    if (!from_json.empty()) return from_json;
    if (!from_tgt.empty()) return from_tgt;
    return fromEdfHeader(root);
}

MachineIdentity installIdentity(const std::string& local_dir, const std::string& archive_dir,
                                const std::string& data_dir) {
    std::string card_root = local_dir;
    if (card_root.empty()) card_root = archive_dir;
    if (card_root.empty() && !data_dir.empty())
        card_root = (std::filesystem::path(data_dir) / "cpap_data").string();
    auto identity = readIdentification(card_root);
    // No card to name the machine: an AirSense 11 read over its own Bluetooth
    // (directly or through a Mule & Miner) named itself in its own folder.
    // Without this a Bluetooth-only install announced "ResMed ?".
    if (identity.empty()) identity = readBluetoothIdentification(data_dir);
    return identity;
}

std::string machineName(const std::string& configured, const MachineIdentity& id) {
    const std::string name = trim(configured);
    if (!name.empty() && name != AppConfig::kDefaultDeviceName && name != AppConfig::kLegacyDeviceName)
        return name;
    if (!id.model.empty()) return id.manufacturer.empty() ? id.model : id.manufacturer + " " + id.model;
    return AppConfig::kDefaultDeviceName;
}

MachineIdentity readBluetoothIdentification(const std::string& data_dir) {
    if (data_dir.empty()) return {};
    std::error_code ec;
    std::filesystem::path newest;
    std::filesystem::file_time_type newest_time{};
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(data_dir) / "as11", ec)) {
        const auto file = entry.path() / "Identification.json";
        std::error_code fec;
        if (!entry.is_directory(fec) || !std::filesystem::is_regular_file(file, fec)) continue;
        const auto t = std::filesystem::last_write_time(file, fec);
        if (fec) continue;
        if (newest.empty() || t > newest_time) {
            newest = entry.path();
            newest_time = t;
        }
    }
    if (newest.empty()) return {};
    return readIdentification(newest.string());
}

MachineIdentity identityFromSession(const CPAPSession& session) {
    MachineIdentity id;
    id.manufacturer = manufacturerName(session.manufacturer);
    id.model = readableModel(session.product_name);
    id.serial = trim(session.serial_number);
    id.firmware = trim(session.firmware);
    return id;
}

}  // namespace hms_cpap
