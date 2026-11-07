#include "services/Ds2Store.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
namespace ds2 = cpapdash::ds2ble;

namespace hms_cpap {

namespace {
constexpr const char* kTag = "DS2 Bluetooth: ";

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// "0000077A" -> 0x77a; nullopt unless exactly eight hex digits.
std::optional<uint32_t> hexStem(const std::string& stem) {
    if (stem.size() != 8) return std::nullopt;
    for (char c : stem)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    return static_cast<uint32_t>(std::strtoul(stem.c_str(), nullptr, 16));
}
}  // namespace

Ds2Store::Ds2Store(std::string data_dir) : data_dir_(std::move(data_dir)) {}

std::string Ds2Store::root() const { return (fs::path(data_dir_) / "ds2").string(); }
std::string Ds2Store::jsonPath() const { return (fs::path(data_dir_) / "ds2.json").string(); }

std::string Ds2Store::safeSerial(const std::string& serial) {
    std::string s;
    for (char c : serial)
        if (std::isalnum(static_cast<unsigned char>(c))) s.push_back(c);
    return s.empty() ? "unknown" : s;
}

std::string Ds2Store::machineDir(const std::string& serial) const {
    return (fs::path(root()) / safeSerial(serial)).string();
}

Ds2Store::State Ds2Store::load() const {
    State st;
    std::ifstream in(jsonPath());
    if (!in) return st;
    Json::Value j;
    Json::CharReaderBuilder b;
    std::string errs;
    if (!Json::parseFromStream(b, in, &j, &errs) || !j.isObject()) {
        std::cerr << kTag << jsonPath() << " is not readable JSON (" << errs
                  << "); starting from no pairing and no bookmarks" << std::endl;
        return st;
    }
    if (j["paired"].isObject()) {
        Paired p;
        p.address = j["paired"]["address"].asString();
        p.name = j["paired"]["name"].asString();
        p.id = j["paired"]["id"].asString();
        p.paired_at = j["paired"]["paired_at"].asInt64();
        p.serial = j["paired"]["serial"].asString();
        st.paired = p;
    }
    const Json::Value& ms = j["machines"];
    if (ms.isObject()) {
        for (const auto& serial : ms.getMemberNames()) {
            const Json::Value& m = ms[serial];
            Machine mc;
            mc.serial = serial;
            mc.model = m["model"].asString();
            mc.firmware = m["firmware"].asString();
            mc.newest_session = m["newest_session"].asUInt();
            mc.last_read_at = m["last_read_at"].asInt64();
            mc.last_records = m["last_records"].asInt();
            mc.last_error = m["last_error"].asString();
            mc.last_attempt_at = m["last_attempt_at"].asInt64();
            const Json::Value& bms = m["bookmarks"];
            if (bms.isObject()) {
                for (const auto& k : bms.getMemberNames()) {
                    try {
                        const int stream = std::stoi(k);
                        auto bm = ds2::fromHex(bms[k].asString());
                        if (stream >= 0 && stream <= 255 && (bm.empty() || bm.size() == ds2::kBookmarkSize))
                            mc.bookmarks[static_cast<uint8_t>(stream)] = bm;
                    } catch (...) {
                        std::cerr << kTag << "ignoring a bad bookmark for stream " << k << std::endl;
                    }
                }
            }
            st.machines[serial] = mc;
        }
    }
    return st;
}

bool Ds2Store::save(const State& s) const {
    Json::Value j(Json::objectValue);
    if (s.paired) {
        j["paired"]["address"] = s.paired->address;
        j["paired"]["name"] = s.paired->name;
        j["paired"]["id"] = s.paired->id;
        j["paired"]["paired_at"] = static_cast<Json::Int64>(s.paired->paired_at);
        j["paired"]["serial"] = s.paired->serial;
    }
    j["machines"] = Json::Value(Json::objectValue);
    for (const auto& [serial, m] : s.machines) {
        Json::Value& o = j["machines"][serial];
        o["model"] = m.model;
        o["firmware"] = m.firmware;
        o["newest_session"] = m.newest_session;
        o["last_read_at"] = static_cast<Json::Int64>(m.last_read_at);
        o["last_records"] = m.last_records;
        o["last_error"] = m.last_error;
        o["last_attempt_at"] = static_cast<Json::Int64>(m.last_attempt_at);
        o["bookmarks"] = Json::Value(Json::objectValue);
        for (const auto& [stream, bm] : m.bookmarks) o["bookmarks"][std::to_string(stream)] = ds2::toHex(bm);
    }
    Json::StreamWriterBuilder w;
    w["indentation"] = " ";
    const std::string text = Json::writeString(w, j) + "\n";
    return writeAtomic(jsonPath(), std::vector<uint8_t>(text.begin(), text.end()), true);
}

bool Ds2Store::writeAtomic(const std::string& path, const std::vector<uint8_t>& bytes, bool private_file) const {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const fs::path tmp = fs::path(path).string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << kTag << "cannot write " << tmp.string() << std::endl;
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            std::cerr << kTag << "write failed: " << tmp.string() << std::endl;
            return false;
        }
    }
    if (private_file)
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    fs::rename(tmp, path, ec);
    if (ec) {
        // Windows will not rename over an existing file.
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        std::cerr << kTag << "cannot replace " << path << ": " << ec.message() << std::endl;
        return false;
    }
    return true;
}

bool Ds2Store::writeIdentity(const ds2::Identity& id) const {
    if (id.serial.empty()) return false;
    const std::string text = "SN=" + id.serial + "\r\nMN=" + id.model + "\r\nSV=" + id.firmware + "\r\n";
    return writeAtomic((fs::path(machineDir(id.serial)) / "PROP.TXT").string(),
                       std::vector<uint8_t>(text.begin(), text.end()), false);
}

std::string Ds2Store::recordPath(const ds2::Record& r) {
    switch (r.stream) {
        case 1:
        case 2: return "P0/" + ds2::cardFileName(r.stream, r.session);
        // A log record has no session file of its own; its bookmark names it
        // uniquely, so a record read twice lands on the same file.
        case 3: return "D/" + ds2::toHex(r.bookmark_out) + ".B03";
        case 4: return "E/" + ds2::toHex(r.bookmark_out) + ".B04";
        default: return "";
    }
}

std::string Ds2Store::writeRecord(const std::string& serial, const ds2::Record& r) const {
    const std::string rel = recordPath(r);
    if (rel.empty()) return "";
    const fs::path full = fs::path(machineDir(serial)) / rel;
    if (!writeAtomic(full.string(), r.body, false)) return "";
    return (fs::path("ds2") / safeSerial(serial) / rel).generic_string();
}

bool Ds2Store::saveBookmark(const std::string& serial, uint8_t stream, const ds2::Bookmark& b) const {
    State st = load();
    auto& m = st.machines[serial];
    m.serial = serial;
    m.bookmarks[stream] = b;
    return save(st);
}

std::vector<Ds2Store::StoredRecord> Ds2Store::sessionRecords(const std::string& serial) const {
    std::vector<StoredRecord> out;
    const fs::path p0 = fs::path(machineDir(serial)) / "P0";
    std::error_code ec;
    if (!fs::is_directory(p0, ec)) return out;
    for (const auto& e : fs::directory_iterator(p0, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        uint8_t stream = ext == ".B01" ? 1 : ext == ".B02" ? 2 : 0;
        if (!stream) continue;
        const auto session = hexStem(e.path().stem().string());
        if (!session) continue;
        StoredRecord r;
        r.stream = stream;
        r.session = *session;
        r.body = readFile(e.path());
        r.rel_path = (fs::path("ds2") / safeSerial(serial) / "P0" / e.path().filename()).generic_string();
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const StoredRecord& a, const StoredRecord& b) {
        return a.session != b.session ? a.session < b.session : a.stream < b.stream;
    });
    return out;
}

std::vector<std::string> Ds2Store::serials() const {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(root(), ec)) return out;
    for (const auto& e : fs::directory_iterator(root(), ec))
        if (e.is_directory()) out.push_back(e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

ds2::Identity Ds2Store::readIdentity(const std::string& serial) const {
    ds2::Identity id;
    std::ifstream in(fs::path(machineDir(serial)) / "PROP.TXT");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "SN") id.serial = v;
        else if (k == "MN") id.model = v;
        else if (k == "SV") id.firmware = v;
    }
    return id;
}

}  // namespace hms_cpap
