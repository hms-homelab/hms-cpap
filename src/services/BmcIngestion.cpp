#include "services/BmcIngestion.h"

#include <algorithm>
#include <filesystem>
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

}  // namespace

std::optional<std::string> BmcIngestion::findCardDir(const std::string& root, int max_depth) {
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return std::nullopt;
    return search(fs::path(root), max_depth);
}

BmcIngestion::Read BmcIngestion::readSessions(const std::string& device_id,
                                              const std::string& device_name) const {
    Read out;
#ifdef CPAPDASH_WITH_BMC
    const auto dir = findCardDir(root_);
    if (!dir) {
        out.error = "no BMC card (serial-named data files and a .evt) in " + root_;
        return out;
    }
    out.card_dir = *dir;
    const auto card = cpapdash::parser::BmcParser::readCard(*dir);
    if (!card) {
        out.error = "the BMC card in " + *dir + " could not be read";
        return out;
    }
    for (const auto& span : card->sessions) {
        auto s = cpapdash::parser::BmcParser::sessionFromCard(*card, span, device_id,
                                                              device_name);
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
    out.error = "this build has no BMC parser";
#endif
    return out;
}

}  // namespace hms_cpap
