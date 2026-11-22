#include "services/CardUpload.h"

#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"
#include "services/BmcIngestion.h"
#include "services/PhilipsCardIngestion.h"
#include "services/PrismaIngestion.h"
#include "services/RemovedNights.h"
#include "services/SefamIngestion.h"
#include "services/SyncFolderState.h"
#include "utils/SessionEnd.h"

#include <cpapdash/parser/SefamParser.h>
#include <cpapdash/parser/WallClock.h>
#ifdef CPAPDASH_WITH_PHILIPS
#include <cpapdash/parser/PhilipsParser.h>
#endif

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
        case UploadedCard::Bmc:        return "bmc";
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
    // SDD-049: the parser's own test (a serial-named .usr beside a .evt), in the
    // folder or in one a zip wrapped it in.
    if (BmcIngestion::findCardDir(dir, kZipDepth)) return UploadedCard::Bmc;
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

// ── SDD-083: a DreamStation 2 card whose files are encrypted ───────────────

std::optional<std::string> encryptedPhilipsCardIn(const std::string& dir) {
#ifdef CPAPDASH_WITH_PHILIPS
    const auto card = PhilipsCardIngestion::findCardDir(dir, kZipDepth);
    if (!card) return std::nullopt;
    if (!cpapdash::parser::PhilipsParser::isEncryptedCard(*card)) return std::nullopt;
    return card;
#else
    (void)dir;
    return std::nullopt;
#endif
}

bool keepPhilipsUpload(const std::string& card_dir, const std::string& store, std::string& error) {
    const fs::path card(card_dir);
    const fs::path parent = card.parent_path();
    if (lower(parent.filename().string()) == "p-series")
        return mergeCardInto(parent.string(), (fs::path(store) / "P-SERIES").string(), error);
    // A zip of the machine folder alone, or of its contents.
    std::string name = card.filename().string();
    if (name.empty() || name.rfind("cpap_stage_", 0) == 0) name = "UPLOAD";
    return mergeCardInto(card_dir, (fs::path(store) / "P-SERIES" / name).string(), error);
}

Json::Value philipsUploadRefusal(bool containerised) {
    Json::Value r(Json::objectValue);
    r["format"] = "philips";
    r["encrypted"] = true;
    r["oscar"] = "not_paired";
    r["error"] = containerised
        ? "This is a DreamStation 2 card whose files are encrypted. hms-cpap does not decrypt "
          "them: in Docker or the Home Assistant add-on it reads them through the OSCAR helper on "
          "a home PC that has OSCAR 2.1.1, and no helper is paired. Install hms-cpap on that PC, "
          "choose \"Read OSCAR for hms-cpap\", let it connect here (Devices shows \"OSCAR helper\"), "
          "then upload the card again. Nothing was kept."
        : "This is a DreamStation 2 card whose files are encrypted. hms-cpap does not decrypt "
          "them: it reads them through an OSCAR you install yourself, and none is set up. Install "
          "OSCAR 2.1.1, create a profile named hms-cpap in it once, and set OSCAR up in Settings, "
          "Data Source (\"On this computer\", or \"On a home PC\" for the OSCAR helper), then "
          "upload the card again. Nothing was kept.";
    return r;
}

Json::Value philipsUploadReply(const oscar::OscarCardReader::ReadResult& rr, bool helper) {
    using oscar::Outcome;
    Json::Value r(Json::objectValue);
    r["format"] = "philips";
    r["encrypted"] = true;
    r["oscar"] = oscar::outcomeName(rr.outcome);
    r["read_by"] = helper ? "oscar_helper" : "oscar";
    if (!rr.version.empty()) r["oscar_version"] = rr.version;
    const std::string why = rr.message.empty() ? "" : " (" + rr.message + ")";
    switch (rr.outcome) {
        case Outcome::Imported:
        case Outcome::NothingNew: {
            std::set<std::string> nights;
            for (const auto& t : rr.landed) nights.insert(nightOf(t));
            Json::Value arr(Json::arrayValue);
            for (const auto& d : nights) arr.append(d);
            r["status"] = "done";
            r["sessions_found"] = rr.stored;
            r["dates"] = arr;
            r["message"] = rr.stored > 0 ? "OSCAR read the card; its new sessions are stored"
                                         : "OSCAR read the card; every session on it was already stored";
            return r;
        }
        case Outcome::NotSetUp:
            r["error"] = "OSCAR is not set up: fill in OSCAR's program, data folder and profile in "
                         "Settings, Data Source, then upload the card again.";
            break;
        case Outcome::NoProgram:
            r["error"] = "OSCAR did not start" + why + ". Check the OSCAR program in Settings, Data Source, "
                         "then upload the card again.";
            break;
        case Outcome::NotSupported:
            r["error"] = "OSCAR's data folder is from a version hms-cpap has not been proven on" + why +
                         "; it reads OSCAR 2.1.1. Nothing was stored.";
            break;
        case Outcome::NoProfile:
            r["error"] = "OSCAR has no profile for hms-cpap" + why + ". Open OSCAR, create a profile named "
                         "hms-cpap once, close OSCAR, then upload the card again.";
            break;
        case Outcome::Busy:
            r["error"] = "The OSCAR that is open could not import the card now. Close OSCAR, then upload "
                         "the card again.";
            break;
        case Outcome::Timeout:
            r["error"] = "OSCAR did not finish reading the card in time; nothing was stored. Upload the "
                         "card again.";
            break;
        case Outcome::ReadNothing:
            r["error"] = "OSCAR read nothing from the card" + why + ".";
            break;
        case Outcome::HelperOffline:
            r["error"] = "The OSCAR helper is not connected" + why + ". The card is kept; upload it "
                         "again once the helper is connected.";
            break;
        case Outcome::HelperUpdate:
            r["error"] = "The OSCAR helper runs another version of hms-cpap" + why + ". Update hms-cpap "
                         "on that PC, then upload the card again.";
            break;
        case Outcome::Failed:
            r["error"] = "OSCAR could not read the card" + why + ".";
            break;
    }
    return r;
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
    cpapdash::parser::WallClockLabels labels;   // SDD-074 D4
    std::set<std::string> nights;
    if (kind == UploadedCard::Sefam) {
        SefamIngestion ing(root);
        if (ing.initialize())
            for (const auto& s : ing.discoverSessions(std::nullopt)) nights.insert(nightOf(s.session_start));
    } else if (kind == UploadedCard::Lowenstein) {
        for (const auto& s : lowensteinSessions(root)) nights.insert(nightOf(s.session.session_start));
    } else if (kind == UploadedCard::Bmc) {
        // A BMC card's sessions only exist once its packets are read; the card
        // is small (about 1 MB an hour), so the reply reads it.
        const auto read = BmcIngestion(root).readSessions("upload", "BMC");
        for (const auto& s : read.sessions) nights.insert(nightOf(*s->session_start));
    }
    return nights;
}

CardImportCounts importCardSessions(IDatabase& db, UploadedCard kind, const std::string& root,
                                    const std::string& device_id,
                                    const std::string& device_name,
                                    const CardImportProgress& progress) {
    cpapdash::parser::WallClockLabels labels;   // SDD-074 D4
    CardImportCounts c;
    const auto removed = removedNightSet(db, device_id);

    // Everything a night needs once the parser has handed it over: the same
    // two calls the burst makes, without its MQTT and AI summary.
    auto save = [&](const CPAPSession& s, const std::chrono::system_clock::time_point& start) {
        if (!db.saveSession(s)) return false;
        closeWithDataEnd(db, device_id, start, s);   // SDD-037 D2
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
    } else if (kind == UploadedCard::Bmc) {
        // SDD-049 D4: every session on the card the database does not hold.
        const auto read = BmcIngestion(root).readSessions(device_id, device_name);
        if (!read.ok) {
            std::cerr << "CardUpload: " << read.error << std::endl;
            return c;
        }
        c.found = static_cast<int>(read.sessions.size());
        int done = 0;
        for (const auto& s : read.sessions) {
            const auto start = *s->session_start;
            if (progress) progress(done++, c.found);
            if (isRemovedNight(removed, start)) { ++c.removed; continue; }
            if (db.sessionExists(device_id, start)) { ++c.already_stored; continue; }
            if (!save(*s, start)) ++c.refused;
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
