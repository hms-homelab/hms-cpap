#include "services/BmcIngestion.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <system_error>

namespace hms_cpap {

namespace fs = std::filesystem;

BmcIngestion::BmcIngestion(std::string card_root) : root_(std::move(card_root)) {}

namespace {

bool isBmcCard(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) names.push_back(it->path().filename().string());
    }
    return cpapdash::parser::detectManufacturer(names) == DeviceManufacturer::BMC;
}

std::optional<std::string> search(const fs::path& dir, int depth_left) {
    if (isBmcCard(dir)) return dir.string();
    if (depth_left <= 0) return std::nullopt;
    std::error_code ec;
    std::vector<fs::path> subdirs;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) subdirs.push_back(it->path());
    }
    std::sort(subdirs.begin(), subdirs.end());   // the same answer every burst
    for (const auto& sub : subdirs) {
        if (auto found = search(sub, depth_left - 1)) return found;
    }
    return std::nullopt;
}

#ifdef CPAPDASH_WITH_BMC
std::vector<uint8_t> readAll(const fs::path& p, size_t at_most = SIZE_MAX) {
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> out;
    char buf[65536];
    while (in && out.size() < at_most) {
        in.read(buf, static_cast<std::streamsize>(std::min(sizeof(buf), at_most - out.size())));
        out.insert(out.end(), buf, buf + in.gcount());
    }
    return out;
}

bool isRawExtension(const std::string& ext) {
    return ext.size() == 4 && ext[0] == '.' && std::isdigit(static_cast<unsigned char>(ext[1])) &&
           std::isdigit(static_cast<unsigned char>(ext[2])) &&
           std::isdigit(static_cast<unsigned char>(ext[3]));
}

/// The card from its data files alone (.000, .001 ... in order) and the model
/// from its .USR, with no events: what the parser's buffers read builds.
std::optional<cpapdash::parser::BmcCard> readDataFilesOnly(const std::string& dir) {
    namespace p = cpapdash::parser;
    std::map<std::string, std::map<std::string, fs::path>> raw;   // stem -> ext -> file
    std::map<std::string, fs::path> usr;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string stem = it->path().stem().string();
        if (isRawExtension(ext)) raw[stem][ext] = it->path();
        else if (ext == ".usr") usr[stem] = it->path();
    }
    for (const auto& [stem, files] : raw) {
        const auto u = usr.find(stem);
        if (u == usr.end()) continue;   // the .USR is what makes it a card
        std::vector<std::vector<uint8_t>> raws;
        for (const auto& [ext, path] : files) raws.push_back(readAll(path));
        auto card = p::buildBmcCard(stem, raws, {});
        // Only up to the end of the model string: the rest is patient details.
        const auto head = readAll(u->second, p::kBmcUsrModelOffset + p::kBmcUsrModelBytes);
        card.model = p::bmcModelFromUsr(head.data(), head.size());
        return card;
    }
    return std::nullopt;
}
#endif

}  // namespace

#ifdef CPAPDASH_WITH_BMC
std::optional<cpapdash::parser::BmcCard> BmcIngestion::readCard(const std::string& dir,
                                                                bool* has_event_file) {
    auto card = cpapdash::parser::BmcParser::readCard(dir);
    if (has_event_file) *has_event_file = card.has_value();
    // The parser's folder read takes a .000 only with its .evt beside it; a
    // card the machine has not stopped on yet has none, so its data files go
    // the way the parser's buffers read takes them (SDD-058 D4).
    if (!card) card = readDataFilesOnly(dir);
    return card;
}
#endif

std::optional<std::string> BmcIngestion::findCardDir(const std::string& root, int max_depth) {
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return std::nullopt;
    return search(fs::path(root), max_depth);
}

BmcIngestion::Read BmcIngestion::readSessions(const std::string& device_id,
                                              const std::string& device_name,
                                              bool flow_apneas) const {
    Read out;
#ifdef CPAPDASH_WITH_BMC
    const auto dir = findCardDir(root_);
    if (!dir) {
        out.error = "no BMC card (serial-named data files and a .USR) in " + root_;
        return out;
    }
    out.card_dir = *dir;
    const auto card = readCard(*dir, &out.has_event_file);
    if (!card) {
        out.error = "the BMC card in " + *dir + " could not be read";
        return out;
    }
    for (const auto& span : card->sessions) {
        auto s = cpapdash::parser::BmcParser::sessionFromCard(*card, span, device_id,
                                                              device_name, nullptr, flow_apneas);
        if (!s || !s->session_start) continue;
        // The parser names the mode in the session's settings (2 on a bi-level);
        // the metrics are what is stored and published, and a mode left there
        // as nothing reads as 0, which is CPAP.
        if (s->metrics && s->settings && s->settings->therapy_mode &&
            s->metrics->therapy_mode.value_or(0) == 0)
            s->metrics->therapy_mode = s->settings->therapy_mode;
        out.sessions.push_back(std::move(s));
    }
    out.ok = true;
#else
    (void)device_id;
    (void)device_name;
    (void)flow_apneas;
    out.error = "this build has no BMC parser";
#endif
    return out;
}

}  // namespace hms_cpap
