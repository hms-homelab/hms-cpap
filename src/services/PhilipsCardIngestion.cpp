#include "services/PhilipsCardIngestion.h"

#ifdef CPAPDASH_WITH_PHILIPS
#include <cpapdash/parser/PhilipsParser.h>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

namespace hms_cpap {

namespace fs = std::filesystem;

PhilipsCardIngestion::PhilipsCardIngestion(std::string card_root) : root_(std::move(card_root)) {}

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

/// "P0" .. "P99": a folder of session files.
bool isSessionFolderName(const std::string& name) {
    const auto n = upper(name);
    return n.size() >= 2 && n[0] == 'P' &&
           std::all_of(n.begin() + 1, n.end(), [](unsigned char c) { return std::isdigit(c); });
}

/// The card's identity file, any case: PROP.BIN (a DreamStation 2) or
/// PROP.TXT (a DreamStation 1 on the same layout).
std::optional<fs::path> propertiesFile(const fs::path& dir) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto n = upper(it->path().filename().string());
        if (n == "PROP.BIN" || n == "PROP.TXT") return it->path();
    }
    return std::nullopt;
}

std::vector<fs::path> sessionFolders(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && isSessionFolderName(it->path().filename().string()))
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool isCard(const fs::path& dir) {
    return propertiesFile(dir).has_value() && !sessionFolders(dir).empty();
}

std::optional<std::string> search(const fs::path& dir, int depth_left) {
    if (isCard(dir)) return dir.string();
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

std::optional<std::string> PhilipsCardIngestion::findCardDir(const std::string& root, int max_depth) {
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return std::nullopt;
    return search(fs::path(root), max_depth);
}

PhilipsCardIngestion::Read PhilipsCardIngestion::readSessions(const std::string& device_id,
                                                              const std::string& device_name) const {
    Read out;
#ifdef CPAPDASH_WITH_PHILIPS
    using cpapdash::parser::PhilipsChunk;
    using cpapdash::parser::PhilipsParser;
    using cpapdash::parser::PhilipsParseWarnings;

    const auto dir = findCardDir(root_);
    if (!dir) {
        out.error = "no Philips card (a PROP.BIN beside P0 .. Pn folders) in " + root_;
        return out;
    }
    out.card_dir = *dir;
    const fs::path card(*dir);

    if (PhilipsParser::isEncryptedCard(card.string())) {
        out.encrypted = true;
        out.error = "the Philips card in " + *dir +
                    " is encrypted; hms-cpap reads a DreamStation card only when its files are plain";
        return out;
    }

    std::optional<cpapdash::parser::PhilipsDeviceInfo> info;
    if (const auto props = propertiesFile(card)) info = PhilipsParser::parseProperties(props->string());

    // Every session's files, from every P folder: a session's summary can sit
    // in one folder and its events in another.
    struct Files {
        std::vector<std::string> summary, events, waveform;
    };
    std::map<uint32_t, Files> by_number;
    for (const auto& folder : sessionFolders(card)) {
        for (const auto& g : PhilipsParser::discoverSessions(folder.string())) {
            auto& f = by_number[g.session_number];
            f.summary.insert(f.summary.end(), g.summary_files.begin(), g.summary_files.end());
            f.events.insert(f.events.end(), g.events_files.begin(), g.events_files.end());
            f.waveform.insert(f.waveform.end(), g.waveform_files.begin(), g.waveform_files.end());
        }
    }

    const auto chunksOf = [](const std::vector<std::string>& files, uint32_t number,
                             PhilipsParseWarnings& w) {
        std::vector<PhilipsChunk> out_chunks;
        for (const auto& path : files) {
            std::ifstream in(path, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
            for (auto& c : PhilipsParser::readChunks(bytes.data(), bytes.size(), w)) {
                if (c.session_number == number) out_chunks.push_back(std::move(c));
            }
        }
        return out_chunks;
    };

    for (const auto& [number, files] : by_number) {
        PhilipsParseWarnings w;
        const auto summary = chunksOf(files.summary, number, w);
        const auto events = chunksOf(files.events, number, w);
        const auto waveform = chunksOf(files.waveform, number, w);
        auto s = PhilipsParser::sessionFromChunks(summary, events, waveform,
                                                  info ? &*info : nullptr, device_id,
                                                  device_name, w);
        if (!s || !s->session_start) continue;
        Session one;
        one.number = number;
        one.closed = !summary.empty();
        one.parsed = std::move(s);
        out.sessions.push_back(std::move(one));
    }
    std::sort(out.sessions.begin(), out.sessions.end(), [](const Session& a, const Session& b) {
        return *a.parsed->session_start < *b.parsed->session_start;
    });
    out.ok = true;
#else
    (void)device_id;
    (void)device_name;
    out.error = "this build has no Philips parser";
#endif
    return out;
}

}  // namespace hms_cpap
