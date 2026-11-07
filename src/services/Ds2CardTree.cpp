#include "services/Ds2CardTree.h"

#include "services/Ds2Store.h"
#include "sleepbridge/BatchStore.h"   // durableWrite

#include <json/json.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace hms_cpap {

namespace fs  = std::filesystem;
namespace ds2 = cpapdash::ds2ble;

namespace {

constexpr const char* kTag = "[ds2 card] ";

bool digits(const std::string& s, size_t from, size_t n) {
    if (s.size() < from + n) return false;
    for (size_t i = from; i < from + n; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

bool upperHex(const std::string& s, size_t from, size_t n) {
    if (s.size() < from + n) return false;
    for (size_t i = from; i < from + n; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}

/// "P<n>/<8 hex>.B0<stream>", n one or two digits; the P folder's number, or -1.
int pFolderOf(const std::string& path, uint8_t stream) {
    if (path.size() < 2 || path[0] != 'P') return -1;
    const size_t slash = path.find('/');
    if (slash != 2 && slash != 3) return -1;
    if (!digits(path, 1, slash - 1)) return -1;
    const std::string file = path.substr(slash + 1);
    const std::string ext  = std::string(".B0") + static_cast<char>('0' + stream);
    if (file.size() != 12 || !upperHex(file, 0, 8) || file.substr(8) != ext) return -1;
    return std::stoi(path.substr(1, slash - 1));
}

/// The miner's "D/<nnn>.B03" or "E/<nnn>.B04" for stream 3 or 4.
bool logPath(const std::string& path, uint8_t stream) {
    const std::string dir = stream == 3 ? "D/" : "E/";
    const std::string ext = stream == 3 ? ".B03" : ".B04";
    return path.size() == 9 && path.compare(0, 2, dir) == 0 && digits(path, 2, 3) && path.compare(5, 4, ext) == 0;
}

std::string hex(const ds2::Bookmark& b) {
    std::string s;
    char h[3];
    for (const uint8_t c : b) {
        std::snprintf(h, sizeof h, "%02x", c);
        s += h;
    }
    return s;
}

int filesIn(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().filename().string().rfind('.', 0) != 0) ++n;   // not a temporary
    return n;
}

bool writeWhole(const fs::path& p, const void* data, size_t size) {
    std::string err;
    if (sleepbridge::durableWrite(p.string(), data, size, &err)) return true;
    std::cerr << kTag << err << std::endl;
    return false;
}

}  // namespace

Ds2CardTree::Ds2CardTree(std::string data_dir) : data_dir_(std::move(data_dir)) {}

std::string Ds2CardTree::folderFor(const std::string& serial) {
    uint32_t h = 0x811C9DC5u;   // FNV-1a, 32 bits
    for (const unsigned char c : serial) {
        h ^= c;
        h *= 0x01000193u;
    }
    char b[16];
    std::snprintf(b, sizeof b, "%06X00", static_cast<unsigned>(h >> 8));
    return b;
}

std::string Ds2CardTree::root(const std::string& serial) const {
    return (fs::path(data_dir_) / "ds2card" / Ds2Store::safeSerial(serial)).string();
}

std::string Ds2CardTree::machineDir(const std::string& serial) const {
    return (fs::path(root(serial)) / "P-SERIES" / folderFor(serial)).string();
}

Ds2CardTree::State Ds2CardTree::load(const std::string& serial) const {
    State s;
    std::ifstream in(fs::path(root(serial)) / "tree.json");
    if (!in) return s;
    std::stringstream ss;
    ss << in.rdbuf();
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream is(ss.str());
    if (!Json::parseFromStream(rb, is, &v, nullptr) || !v.isObject()) return s;
    for (const auto& k : v["d"].getMemberNames()) s.d[k] = v["d"][k].asInt();
    for (const auto& k : v["e"].getMemberNames()) s.e[k] = v["e"][k].asInt();
    s.fd = v.get("fd", 0).asInt64();
    return s;
}

bool Ds2CardTree::save(const std::string& serial, const State& s) const {
    Json::Value v(Json::objectValue);
    v["d"] = Json::Value(Json::objectValue);
    v["e"] = Json::Value(Json::objectValue);
    for (const auto& [k, n] : s.d) v["d"][k] = n;
    for (const auto& [k, n] : s.e) v["e"][k] = n;
    v["fd"] = static_cast<Json::Int64>(s.fd);
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    const std::string text = Json::writeString(w, v);
    return writeWhole(fs::path(root(serial)) / "tree.json", text.data(), text.size());
}

Ds2CardTree::Placed Ds2CardTree::place(const std::string& serial, const ds2::ReadSealed& s) {
    Placed out;
    std::lock_guard<std::mutex> lk(mu_);
    std::string rel;
    State st;
    bool numbered = false;
    if (s.stream == 0) {
        if (s.path == "PROP.BIN" || s.path == "LOG.SEQ") rel = s.path;
    } else if (s.stream == 1 || s.stream == 2) {
        if (pFolderOf(s.path, s.stream) >= 0) rel = s.path;
    } else if ((s.stream == 3 || s.stream == 4) && logPath(s.path, s.stream)) {
        // Each read numbers its logs from 000: across reads they go on here,
        // one number per record, by its bookmark.
        st         = load(serial);
        auto& map  = s.stream == 3 ? st.d : st.e;
        const auto key = hex(s.bookmark);
        auto it    = map.find(key);
        if (it == map.end()) {
            it       = map.emplace(key, static_cast<int>(map.size())).first;
            numbered = true;
        }
        char b[16];
        std::snprintf(b, sizeof b, "%s/%03d.%s", s.stream == 3 ? "D" : "E", it->second, s.stream == 3 ? "B03" : "B04");
        rel = b;
    }
    if (rel.empty()) {
        out.error = "bad_path";
        std::cerr << kTag << serial << ": stream " << static_cast<int>(s.stream) << " path \"" << s.path
                  << "\" is not a card path: not placed" << std::endl;
        return out;
    }
    const fs::path machine = machineDir(serial);
    if (!writeWhole(machine / fs::path(rel), s.card.data(), s.card.size())) {
        out.error = "storage";
        return out;
    }
    // The number is kept once its file is on disk.
    if (numbered && !save(serial, st)) {
        out.error = "storage";
        return out;
    }
    const fs::path last = machine.parent_path() / "LAST.TXT";
    const std::string folder = folderFor(serial);
    std::error_code ec;
    if (!fs::exists(last, ec) && !writeWhole(last, folder.data(), folder.size())) {
        out.error = "storage";
        return out;
    }
    out.ok  = true;
    out.rel = rel;
    return out;
}

void Ds2CardTree::noteSessionStart(const std::string& serial, long long start) {
    if (start <= 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    State st = load(serial);
    if (st.fd != 0 && st.fd <= start) return;
    st.fd = start;
    save(serial, st);
}

Ds2CardTree::Counts Ds2CardTree::counts(const std::string& serial) const {
    Counts c;
    std::lock_guard<std::mutex> lk(mu_);
    const fs::path machine = machineDir(serial);
    std::error_code ec;
    int highest = -1;
    for (const auto& e : fs::directory_iterator(machine, ec)) {
        const std::string n = e.path().filename().string();
        if (!e.is_directory() || n.size() < 2 || n.size() > 3 || n[0] != 'P' || !digits(n, 1, n.size() - 1)) continue;
        highest = std::max(highest, std::stoi(n.substr(1)));
    }
    if (highest >= 0) {
        c.p_folder = highest;
        c.pfn      = filesIn(machine / ("P" + std::to_string(highest)));
    }
    c.dfn = filesIn(machine / "D");
    c.efn = filesIn(machine / "E");
    c.fd  = load(serial).fd;
    return c;
}

}  // namespace hms_cpap
