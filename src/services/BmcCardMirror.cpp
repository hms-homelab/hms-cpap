#include "services/BmcCardMirror.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <system_error>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

/// "<name>\t<size_kb>" per file, so a pass can tell an unchanged file without
/// keeping anything in memory across restarts.
const char* kManifest = ".ezshare_bmc_manifest";

/// The bytes asked for again before an append, to prove the card still holds
/// what we hold (D4).
constexpr std::uintmax_t kOverlap = 4096;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// "22B39570.EVT" -> {"22B39570", "evt"}.
std::pair<std::string, std::string> split(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return {name, ""};
    return {name.substr(0, dot), lower(name.substr(dot + 1))};
}

bool isRawExt(const std::string& ext) {
    return ext.size() == 3 && std::all_of(ext.begin(), ext.end(),
                                          [](unsigned char c) { return std::isdigit(c); });
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Pass {
    IDataSource& card;
    fs::path dir;
    std::map<std::string, std::string> manifest;   // name -> size_kb
    BmcMirrorResult r;

    bool unchanged(const EzShareFileEntry& f) const {
        std::error_code ec;
        const auto it = manifest.find(f.name);
        return it != manifest.end() && it->second == std::to_string(f.size_kb) &&
               fs::exists(dir / f.name, ec);
    }

    bool whole(const EzShareFileEntry& f) {
        const fs::path local = dir / f.name;
        const fs::path part = local.string() + ".part";
        std::error_code ec;
        if (card.downloadByPath(f.name, part.string())) {
            fs::rename(part, local, ec);
            if (!ec) {
                manifest[f.name] = std::to_string(f.size_kb);
                ++r.fetched;
                return true;
            }
        }
        fs::remove(part, ec);
        ++r.failed;
        return false;
    }

    /// D4: the rest of a raw file that grew, or false to take it whole.
    bool append(const EzShareFileEntry& f) {
        const fs::path local = dir / f.name;
        std::error_code ec;
        const auto held = fs::file_size(local, ec);
        if (ec || held <= kOverlap || !card.supportsRange()) return false;
        if (static_cast<std::uintmax_t>(f.size_kb) * 1024 <= held) return false;   // not longer

        const std::uintmax_t from = held - kOverlap;
        const fs::path tail = local.string() + ".tail";
        fs::remove(tail, ec);
        size_t got = 0;
        const bool ok = card.downloadFileRange("", f.name, tail.string(), from, got);
        const std::string fresh = ok ? slurp(tail) : std::string();
        fs::remove(tail, ec);
        if (!ok || fresh.size() <= kOverlap) return false;

        // The card must still hold what we hold: the same 4 KB where we left off.
        std::string ours;
        {
            std::ifstream in(local, std::ios::binary);
            in.seekg(static_cast<std::streamoff>(from));
            ours.resize(kOverlap);
            in.read(ours.data(), static_cast<std::streamsize>(kOverlap));
            if (in.gcount() != static_cast<std::streamsize>(kOverlap)) return false;
        }
        if (fresh.compare(0, kOverlap, ours) != 0) {
            std::cout << "Luna ez Share: " << f.name
                      << " no longer starts with what we hold; taking it whole" << std::endl;
            return false;
        }

        const fs::path part = local.string() + ".part";
        fs::copy_file(local, part, fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
        {
            std::ofstream out(part, std::ios::binary | std::ios::app);
            out.write(fresh.data() + kOverlap,
                      static_cast<std::streamsize>(fresh.size() - kOverlap));
            if (!out) {
                fs::remove(part, ec);
                return false;
            }
        }
        fs::rename(part, local, ec);
        if (ec) {
            fs::remove(part, ec);
            return false;
        }
        manifest[f.name] = std::to_string(f.size_kb);
        ++r.appended;
        return true;
    }
};

}  // namespace

bool isBmcRawName(const std::string& name, const std::string& stem) {
    const auto [s, ext] = split(name);
    return lower(s) == lower(stem) && isRawExt(ext);
}

std::optional<std::string> bmcStemOf(const std::vector<EzShareFileEntry>& root) {
    struct Seen {
        bool usr = false, other = false;
        int raw_kb = -1;
    };
    std::map<std::string, Seen> stems;   // lowercased stem -> what it has
    std::map<std::string, std::string> spelled;
    for (const auto& e : root) {
        if (e.is_dir) continue;
        const auto [s, ext] = split(e.name);
        const auto key = lower(s);
        spelled.emplace(key, s);
        auto& seen = stems[key];
        if (ext == "usr") seen.usr = true;
        if (ext == "evt") seen.other = true;
        if (isRawExt(ext)) {
            seen.other = true;
            seen.raw_kb = std::max(seen.raw_kb, e.size_kb);
        }
    }
    std::optional<std::string> best;
    int best_kb = -2;
    for (const auto& [key, seen] : stems) {
        if (!seen.usr || !seen.other) continue;
        if (seen.raw_kb > best_kb) {
            best = spelled[key];
            best_kb = seen.raw_kb;
        }
    }
    return best;
}

BmcMirrorResult mirrorBmcCard(IDataSource& card, const std::string& local_dir) {
    Pass p{card, fs::path(local_dir), {}, {}};
    std::error_code ec;
    if (local_dir.empty()) {
        p.r.error = "no folder to copy the card into (set archive_dir)";
        return p.r;
    }
    fs::create_directories(p.dir, ec);

    {
        std::ifstream in(p.dir / kManifest);
        std::string line;
        while (std::getline(in, line)) {
            const auto t = line.find('\t');
            if (t != std::string::npos) p.manifest[line.substr(0, t)] = line.substr(t + 1);
        }
    }

    // An empty answer is a card that did not answer, not an empty card.
    const auto root = card.listDir("");
    if (root.empty()) {
        p.r.error = "the ez Share did not answer a listing of the card root";
        return p.r;
    }
    const auto stem = bmcStemOf(root);
    if (!stem) {
        p.r.error = "no Luna card on the ez Share (no <serial>.usr beside its .000 or .evt)";
        return p.r;
    }
    p.r.stem = *stem;

    // D2: only this stem's files, raw files first so D3 knows whether the
    // small ones are owed.
    std::vector<EzShareFileEntry> raws, sidecars, usr;
    for (const auto& e : root) {
        if (e.is_dir) continue;
        const auto [s, ext] = split(e.name);
        if (lower(s) != lower(*stem)) continue;
        if (isRawExt(ext)) raws.push_back(e);
        else if (ext == "evt" || ext == "idx" || ext == "log") sidecars.push_back(e);
        else if (ext == "usr") usr.push_back(e);
    }
    std::sort(raws.begin(), raws.end(),
              [](const auto& a, const auto& b) { return lower(a.name) < lower(b.name); });

    for (const auto& f : raws) {
        if (p.unchanged(f)) {
            ++p.r.unchanged;
            continue;
        }
        p.r.raw_changed = true;
        if (!p.append(f)) p.whole(f);
    }
    for (const auto& f : sidecars) {
        if (!p.r.raw_changed && p.unchanged(f)) {
            ++p.r.unchanged;
            continue;
        }
        p.whole(f);
    }
    for (const auto& f : usr) {
        if (p.unchanged(f)) {
            ++p.r.unchanged;
            continue;
        }
        p.whole(f);
    }

    {
        std::ofstream out(p.dir / kManifest, std::ios::trunc);
        for (const auto& [name, kb] : p.manifest) out << name << "\t" << kb << "\n";
    }
    p.r.ok = true;
    std::cout << "Luna ez Share: card " << p.r.stem << ", " << p.r.fetched << " fetched, "
              << p.r.appended << " appended, " << p.r.unchanged << " unchanged, "
              << p.r.failed << " failed" << std::endl;
    return p.r;
}

}  // namespace hms_cpap
