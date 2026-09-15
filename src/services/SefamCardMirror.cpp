#include "services/SefamCardMirror.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

/// Card levels above the session folders: <model>/<serial>/ on an S.Box AUTO,
/// <model><serial>/ on a SleepBox. Three is one to spare.
constexpr int kMaxDepth = 3;

/// The manifest: "<relative path>\t<size_kb>\t<card timestamp>" per file, so a
/// pass can tell an unchanged file without keeping anything in memory across
/// restarts.
const char* kManifest = ".ezshare_sefam_manifest";

std::string stampOf(const EzShareFileEntry& e) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d", e.year, e.month, e.day,
                  e.hour, e.minute, e.second);
    return buf;
}

std::string sigOf(const EzShareFileEntry& e) {
    return std::to_string(e.size_kb) + "\t" + stampOf(e);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool isCardSystemFolder(const std::string& name) {
    const auto n = upper(name);
    return n == "DATALOG" || n == "SETTINGS" || n == "SYSTEM VOLUME INFORMATION" ||
           n == "EZSHARE" || (!n.empty() && n[0] == '.');
}

std::string nightKey(const EzShareFileEntry& e) {
    // The listing's own wall clock, shifted back 12 h: the night a session
    // belongs to, the rule the rest of hms-cpap uses.
    std::tm tm{};
    tm.tm_year = e.year - 1900;
    tm.tm_mon = e.month - 1;
    tm.tm_mday = e.day;
    tm.tm_hour = e.hour - 12;
    tm.tm_min = e.minute;
    tm.tm_isdst = -1;
    std::mktime(&tm);  // normalises the shifted hour into the previous day
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return buf;
}

long long sessionNumber(const std::string& name) {
    // DATA_229 -> 229; 260913 -> 260913. Non-numeric sorts first.
    std::string digits;
    for (char c : name) if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
    return digits.empty() ? -1 : std::stoll(digits);
}

struct Pass {
    IDataSource& card;
    fs::path root;
    std::map<std::string, std::string> manifest;  // rel path -> signature
    SefamMirrorResult r;

    void syncFile(const std::string& card_dir, const std::string& rel_dir,
                  const EzShareFileEntry& f) {
        ++r.files_seen;
        const std::string rel = rel_dir.empty() ? f.name : rel_dir + "/" + f.name;
        const fs::path local = root / rel;
        std::error_code ec;
        const auto it = manifest.find(rel);
        if (it != manifest.end() && it->second == sigOf(f) && fs::exists(local, ec)) {
            ++r.unchanged;
            return;
        }
        const std::string card_path = card_dir.empty() ? f.name : card_dir + "\\" + f.name;
        const fs::path part = local.string() + ".part";
        if (card.downloadByPath(card_path, part.string())) {
            fs::rename(part, local, ec);
            if (!ec) {
                manifest[rel] = sigOf(f);
                ++r.fetched;
                return;
            }
        }
        fs::remove(part, ec);
        ++r.failed;
    }

    /// A session folder: its files, no deeper.
    void syncSession(const std::string& card_dir, const std::string& rel_dir) {
        const auto entries = card.listDir(card_dir);
        ++r.dirs_listed;
        for (const auto& e : entries)
            if (!e.is_dir) syncFile(card_dir, rel_dir, e);
    }

    void walk(const std::string& card_dir, const std::string& rel_dir, int depth,
              bool whole_card) {
        const auto entries = card.listDir(card_dir);
        ++r.dirs_listed;
        walkEntries(entries, card_dir, rel_dir, depth, whole_card);
    }

    void walkEntries(const std::vector<EzShareFileEntry>& entries, const std::string& card_dir,
                     const std::string& rel_dir, int depth, bool whole_card) {
        std::vector<EzShareFileEntry> sessions, others;
        for (const auto& e : entries) {
            if (!e.is_dir) {
                syncFile(card_dir, rel_dir, e);   // loose files: the serial's .RAM
                continue;
            }
            if (isCardSystemFolder(e.name)) continue;
            (isSefamSessionFolderName(e.name) ? sessions : others).push_back(e);
        }
        const auto chosen = whole_card ? sessions : lastTwoNights(sessions);
        for (const auto& s : chosen) {
            syncSession(card_dir.empty() ? s.name : card_dir + "\\" + s.name,
                        rel_dir.empty() ? s.name : rel_dir + "/" + s.name);
        }
        if (depth <= 0) return;
        for (const auto& d : others) {
            walk(card_dir.empty() ? d.name : card_dir + "\\" + d.name,
                 rel_dir.empty() ? d.name : rel_dir + "/" + d.name, depth - 1, whole_card);
        }
    }
};

}  // namespace

bool isSefamSessionFolderName(const std::string& name) {
    const auto n = upper(name);
    if (n.rfind("DATA_", 0) == 0 && n.size() > 5)
        return std::all_of(n.begin() + 5, n.end(),
                           [](unsigned char c) { return std::isdigit(c); });
    return n.size() == 6 &&
           std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::vector<EzShareFileEntry> lastTwoNights(std::vector<EzShareFileEntry> dirs) {
    if (dirs.empty()) return dirs;
    const bool stamped = std::all_of(dirs.begin(), dirs.end(),
                                     [](const EzShareFileEntry& e) { return e.year > 0; });
    if (!stamped) {
        std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
            return sessionNumber(a.name) < sessionNumber(b.name);
        });
        if (dirs.size() > 4) dirs.erase(dirs.begin(), dirs.end() - 4);
        return dirs;
    }
    std::set<std::string> nights;
    for (const auto& d : dirs) nights.insert(nightKey(d));
    while (nights.size() > 2) nights.erase(nights.begin());
    std::vector<EzShareFileEntry> out;
    for (const auto& d : dirs)
        if (nights.count(nightKey(d))) out.push_back(d);
    return out;
}

SefamMirrorResult mirrorSefamCard(IDataSource& card, const std::string& archive_root,
                                  bool whole_card) {
    Pass p{card, fs::path(archive_root), {}, {}};
    p.r.whole_card = whole_card;
    std::error_code ec;
    if (archive_root.empty()) {
        p.r.error = "no archive folder to copy the card into (set archive_dir)";
        return p.r;
    }
    fs::create_directories(p.root, ec);

    {
        std::ifstream in(p.root / kManifest);
        std::string line;
        while (std::getline(in, line)) {
            const auto t = line.find('\t');
            if (t != std::string::npos) p.manifest[line.substr(0, t)] = line.substr(t + 1);
        }
    }

    // The root first: an empty answer is a card that did not answer, not an
    // empty card, and must not read as a pass that found nothing new.
    const auto root_entries = card.listDir("");
    ++p.r.dirs_listed;
    if (root_entries.empty()) {
        p.r.error = "the ez Share did not answer a listing of the card root";
        return p.r;
    }
    p.walkEntries(root_entries, "", "", kMaxDepth, whole_card);

    {
        std::ofstream out(p.root / kManifest, std::ios::trunc);
        for (const auto& [rel, sig] : p.manifest) out << rel << "\t" << sig << "\n";
    }
    p.r.ok = true;
    std::cout << "Sefam ez Share: " << (whole_card ? "whole card" : "last two nights") << ", "
              << p.r.dirs_listed << " folder(s) listed, " << p.r.fetched << " file(s) fetched, "
              << p.r.unchanged << " unchanged, " << p.r.failed << " failed" << std::endl;
    return p.r;
}

}  // namespace hms_cpap
