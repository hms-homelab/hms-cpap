#include "services/CardUpload.h"

#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"
#include "services/PrismaIngestion.h"
#include "services/RemovedNights.h"
#include "services/SefamIngestion.h"
#include "services/SyncFolderState.h"

#include <cpapdash/parser/SefamParser.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <system_error>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// How deep a card may sit in a zip: a wrapper folder ("cpap files/", as the
/// reporter's), then the card's own <model>/<serial>/<session> nesting.
constexpr int kZipDepth = 4;

/// A Löwenstein file anywhere within [depth] levels: .wmedf (Prisma Smart,
/// under date/sequence folders, where the parser's top-level check does not
/// look) or therapy.pdat (Prisma Line, a zip the parser cannot see into).
bool hasLowensteinFile(const fs::path& dir, int depth) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (it->is_regular_file(e)) {
            const auto ext = lower(it->path().extension().string());
            if (ext == ".pdat" || ext == ".wmedf") return true;
        } else if (depth > 0 && it->is_directory(e) && hasLowensteinFile(it->path(), depth - 1)) {
            return true;
        }
    }
    return false;
}

/// A Sefam session folder (DATA_<n> or a YYMMDD day) holding a manifest,
/// anywhere within [depth] levels. The parser's own check stops at three,
/// one short of a card inside a wrapper folder.
bool hasSefamSessionFolder(const fs::path& dir, int depth) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (!it->is_directory(e)) continue;
        const std::string name = it->path().filename().string();
        const auto up = lower(name);
        const bool data_n = up.rfind("data_", 0) == 0 && up.size() > 5 &&
                            std::all_of(up.begin() + 5, up.end(),
                                        [](unsigned char c) { return std::isdigit(c); });
        const bool day = up.size() == 6 &&
                         std::all_of(up.begin(), up.end(),
                                     [](unsigned char c) { return std::isdigit(c); });
        if (data_n || day) {
            for (fs::directory_iterator f(it->path(), e), fe; !e && f != fe; f.increment(e))
                if (lower(f->path().extension().string()) == ".ini") return true;
        }
        if (depth > 0 && hasSefamSessionFolder(it->path(), depth - 1)) return true;
    }
    return false;
}

std::string dashed(const std::string& yyyymmdd) {
    return yyyymmdd.size() == 8
        ? yyyymmdd.substr(0, 4) + "-" + yyyymmdd.substr(4, 2) + "-" + yyyymmdd.substr(6, 2)
        : yyyymmdd;
}

std::string nightOf(const std::chrono::system_clock::time_point& start) {
    return dashed(strDayForSessionStart(start));
}

}  // namespace

const char* uploadedCardName(UploadedCard kind) {
    switch (kind) {
        case UploadedCard::ResMed:     return "resmed";
        case UploadedCard::Sefam:      return "sefam";
        case UploadedCard::Lowenstein: return "lowenstein";
        case UploadedCard::Unknown:    break;
    }
    return "unknown";
}

UploadedCard classifyUploadedCard(const std::string& dir) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return UploadedCard::Unknown;
    // Sefam first: a session folder that the Sefam discovery then accepts (a
    // manifest it can read a start time from), so a stray DATA_1 is not enough.
    if (hasSefamSessionFolder(dir, kZipDepth)) {
        SefamIngestion probe(dir);
        if (probe.initialize()) return UploadedCard::Sefam;
    }
    if (hasLowensteinFile(dir, kZipDepth)) return UploadedCard::Lowenstein;
    if (cpapdash::parser::detectManufacturer(dir) == DeviceManufacturer::RESMED)
        return UploadedCard::ResMed;
    return UploadedCard::Unknown;
}

bool mergeCardInto(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        error = "cannot create " + to + ": " + ec.message();
        return false;
    }
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot copy the card into " + to + ": " + ec.message();
        return false;
    }
    return true;
}

std::vector<std::string> prepareLowensteinUpload(const std::string& from, const std::string& to,
                                                 std::string& error) {
    std::vector<std::string> roots;
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        error = "cannot create " + to + ": " + ec.message();
        return roots;
    }
    std::vector<fs::path> pdats, pcfgs, others;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (!it->is_regular_file(e)) continue;
        const auto ext = lower(it->path().extension().string());
        (ext == ".pdat" ? pdats : ext == ".pcfg" ? pcfgs : others).push_back(it->path());
    }

    // A .pdat opens into its own folder, named for the file:
    // "therapy(1).pdat" -> therapy(1)/, holding mnt/flash/data/therapy/ and the
    // mnt/flash/conf/device.xml the staging copies beside each session.
    std::map<fs::path, fs::path> dest_for_dir;   // source folder -> its .pdat's folder
    for (const auto& p : pdats) {
        std::error_code e;
        const fs::path dest = fs::path(to) / p.stem();
        fs::remove_all(dest, e);   // a newer card replaces the older copy
        fs::create_directories(dest, e);
        if (!PrismaIngestion::extractZip(p.string(), dest.string())) {
            error = "cannot open " + fs::relative(p, from, e).string();
            continue;
        }
        dest_for_dir[p.parent_path()] = dest;
        roots.push_back(dest.string());
    }
    // The settings archive beside it opens into the same folder.
    for (const auto& p : pcfgs) {
        const auto it = dest_for_dir.find(p.parent_path());
        if (it != dest_for_dir.end()) PrismaIngestion::extractZip(p.string(), it->second.string());
    }
    bool copied_tree = false;
    for (const auto& p : others) {
        std::error_code e;
        const fs::path dest = fs::path(to) / fs::relative(p, from, e);
        fs::create_directories(dest.parent_path(), e);
        fs::copy_file(p, dest, fs::copy_options::overwrite_existing, e);
        if (!e && lower(p.extension().string()) == ".wmedf") copied_tree = true;
    }
    if (copied_tree) roots.push_back(to);   // a Prisma Smart tree, read in place
    return roots;
}

namespace {

/// A Löwenstein session and the ingestion that found it: staging copies that
/// card's own device.xml beside the session, so the two travel together.
struct FoundPrismaSession {
    std::shared_ptr<PrismaIngestion> from;
    PrismaSessionFile session;
};

/// Where the Löwenstein sessions of an upload store can be: the store itself
/// (a Prisma Smart tree) and each folder directly in it (an opened .pdat).
/// A session found through two of them is one session.
std::vector<FoundPrismaSession> lowensteinSessions(const std::string& root) {
    std::vector<std::string> candidates{root};
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (it->is_directory(e)) candidates.push_back(it->path().string());
    }
    std::vector<FoundPrismaSession> out;
    std::set<long long> seen;
    for (const auto& c : candidates) {
        auto ing = std::make_shared<PrismaIngestion>(c);
        if (!ing->initialize()) continue;
        for (auto& s : ing->discoverSessions(std::nullopt)) {
            const long long key =
                std::chrono::duration_cast<std::chrono::seconds>(s.session_start.time_since_epoch()).count();
            if (seen.insert(key).second) out.push_back({ing, std::move(s)});
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.session.session_start < b.session.session_start;
    });
    return out;
}

}  // namespace

std::set<std::string> cardNights(UploadedCard kind, const std::string& root) {
    std::set<std::string> nights;
    if (kind == UploadedCard::Sefam) {
        SefamIngestion ing(root);
        if (ing.initialize())
            for (const auto& s : ing.discoverSessions(std::nullopt)) nights.insert(nightOf(s.session_start));
    } else if (kind == UploadedCard::Lowenstein) {
        for (const auto& s : lowensteinSessions(root)) nights.insert(nightOf(s.session.session_start));
    }
    return nights;
}

CardImportCounts importCardSessions(IDatabase& db, UploadedCard kind, const std::string& root,
                                    const std::string& device_id,
                                    const std::string& device_name,
                                    const CardImportProgress& progress) {
    CardImportCounts c;
    const auto removed = removedNightSet(db, device_id);

    // Everything a night needs once the parser has handed it over: the same
    // two calls the burst makes, without its MQTT and AI summary.
    auto save = [&](const CPAPSession& s, const std::chrono::system_clock::time_point& start) {
        if (!db.saveSession(s)) return false;
        db.markSessionCompleted(device_id, start);
        ++c.imported;
        c.nights.insert(nightOf(start));
        return true;
    };

    if (kind == UploadedCard::Sefam) {
        SefamIngestion ing(root);
        if (!ing.initialize()) return c;
        const auto sessions = ing.discoverSessions(std::nullopt);   // D3: all of them
        c.found = static_cast<int>(sessions.size());
        auto base = createParser(DeviceManufacturer::SEFAM);
        auto* parser = dynamic_cast<cpapdash::parser::SefamParser*>(base.get());
        if (!parser) return c;
        int done = 0;
        for (const auto& ss : sessions) {
            if (progress) progress(done++, c.found);
            if (isRemovedNight(removed, ss.session_start)) { ++c.removed; continue; }
            if (db.sessionExists(device_id, ss.session_start)) { ++c.already_stored; continue; }
            auto parsed = parser->parseSessionNamed(ss.dir, ss.stem, device_id, device_name);
            if (!parsed || !save(*parsed, ss.session_start)) ++c.refused;
        }
    } else if (kind == UploadedCard::Lowenstein) {
        const auto sessions = lowensteinSessions(root);   // D3: all of them
        c.found = static_cast<int>(sessions.size());
        if (sessions.empty()) return c;
        auto parser = createParser(DeviceManufacturer::LOWENSTEIN);
        if (!parser) return c;
        int done = 0;
        for (const auto& found : sessions) {
            const auto& ps = found.session;
            if (progress) progress(done++, c.found);
            if (isRemovedNight(removed, ps.session_start)) { ++c.removed; continue; }
            if (db.sessionExists(device_id, ps.session_start)) { ++c.already_stored; continue; }
            const std::string staged = found.from->stageSession(ps);
            auto parsed = parser->parseSession(staged, device_id, device_name);
            std::error_code ec;
            fs::remove_all(staged, ec);
            if (!parsed || !save(*parsed, ps.session_start)) ++c.refused;
        }
    } else {
        return c;
    }
    if (progress) progress(c.found, c.found);

    // Neither card has an STR, so the sessions are the only writer of the
    // daily summary (as in the burst's Löwenstein and Sefam branches).
    if (c.imported > 0) db.aggregateDailySummaryFromSessions(device_id);
    std::cout << "CardUpload: " << uploadedCardName(kind) << " card at " << root << ": "
              << c.found << " session(s), " << c.imported << " imported, "
              << c.already_stored << " already stored, " << c.removed << " removed night(s), "
              << c.refused << " refused" << std::endl;
    return c;
}

}  // namespace hms_cpap
