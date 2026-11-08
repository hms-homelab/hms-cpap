#include "services/PhilipsCardMirror.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <system_error>
#include <vector>

namespace hms_cpap {

namespace fs = std::filesystem;

namespace {

/// "<relative path>\t<size_kb>\t<card timestamp>" per file.
const char* kManifest = ".ezshare_philips_manifest";

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string sigOf(const EzShareFileEntry& e) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d\t%04d%02d%02d%02d%02d%02d", e.size_kb, e.year, e.month,
                  e.day, e.hour, e.minute, e.second);
    return buf;
}

bool isIdentityFile(const std::string& name) {
    const auto n = upper(name);
    return n == "PROP.BIN" || n == "PROP.TXT" || n == "PROP.BAK" || n == "LOG.SEQ";
}

bool isSessionFolder(const std::string& name) {
    const auto n = upper(name);
    return n.size() >= 2 && n[0] == 'P' &&
           std::all_of(n.begin() + 1, n.end(), [](unsigned char c) { return std::isdigit(c); });
}

/// A session file: .B01 .B02 .B05 (a DreamStation 2), or .001 .002 .005 (a
/// DreamStation 1 on the same layout).
bool isSessionFile(const std::string& name) {
    const auto n = upper(name);
    const auto dot = n.rfind('.');
    if (dot == std::string::npos) return false;
    const auto ext = n.substr(dot + 1);
    return ext == "B01" || ext == "B02" || ext == "B05" || ext == "001" || ext == "002" ||
           ext == "005";
}

/// Plain identity text: printable bytes with at least one key=value line. An
/// encrypted PROP.BIN opens with the container's binary header.
bool isPlainIdentity(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;
    for (unsigned char c : bytes) {
        if (c == '\r' || c == '\n' || c == '\t') continue;
        if (c < 0x20 || c > 0x7E) return false;
    }
    return bytes.find('=') != std::string::npos;
}

struct Pass {
    IDataSource& card;
    fs::path root;
    std::map<std::string, std::string> manifest;
    PhilipsMirrorResult r;

    std::vector<EzShareFileEntry> list(const std::string& card_dir) {
        ++r.dirs_listed;
        return card.listDir(card_dir);
    }

    void syncFile(const std::string& card_dir, const std::string& rel_dir,
                  const EzShareFileEntry& f) {
        const std::string rel = rel_dir + "/" + f.name;
        const fs::path local = root / rel;
        std::error_code ec;
        const auto it = manifest.find(rel);
        if (it != manifest.end() && it->second == sigOf(f) && fs::exists(local, ec)) {
            ++r.unchanged;
            return;
        }
        fs::create_directories(local.parent_path(), ec);
        const fs::path part = local.string() + ".part";
        if (card.downloadByPath(card_dir + "\\" + f.name, part.string())) {
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

    /// One machine folder: its identity files, then its P folders' session files.
    void machine(const std::string& card_dir, const std::string& rel_dir) {
        const auto entries = list(card_dir);
        std::vector<EzShareFileEntry> folders;
        bool identity = false;
        for (const auto& e : entries) {
            if (e.is_dir) {
                if (isSessionFolder(e.name)) folders.push_back(e);
            } else if (isIdentityFile(e.name)) {
                syncFile(card_dir, rel_dir, e);
                identity = true;
            }
        }
        if (!identity) return;
        for (const auto* name : {"PROP.BIN", "PROP.TXT"}) {
            std::error_code ec;
            for (fs::directory_iterator it(root / rel_dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (upper(it->path().filename().string()) != name) continue;
                if (!isPlainIdentity(it->path())) {
                    r.encrypted = true;
                    return;   // hms-cpap does not decrypt: nothing more to copy
                }
            }
        }
        for (const auto& d : folders) {
            const std::string sub_card = card_dir + "\\" + d.name;
            const std::string sub_rel = rel_dir + "/" + d.name;
            for (const auto& f : list(sub_card))
                if (!f.is_dir && isSessionFile(f.name)) syncFile(sub_card, sub_rel, f);
        }
    }
};

}  // namespace

PhilipsMirrorResult mirrorPhilipsCard(IDataSource& card, const std::string& local_dir) {
    Pass p{card, fs::path(local_dir), {}, {}};
    std::error_code ec;
    if (local_dir.empty()) {
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

    // The root first: an empty answer is a card that did not answer.
    const auto root = p.list("");
    if (root.empty()) {
        p.r.error = "the card did not answer a listing of its root";
        return p.r;
    }
    bool found = false;
    for (const auto& e : root) {
        if (!e.is_dir || upper(e.name) != "P-SERIES") continue;
        for (const auto& m : p.list(e.name)) {
            if (!m.is_dir) continue;
            found = true;
            p.machine(e.name + "\\" + m.name, e.name + "/" + m.name);
            if (p.r.encrypted) break;
        }
    }
    if (!found) {
        p.r.error = "no P-SERIES folder on the card: not a Philips DreamStation card";
        return p.r;
    }

    {
        std::ofstream out(p.root / kManifest, std::ios::trunc);
        for (const auto& [rel, sig] : p.manifest) out << rel << "\t" << sig << "\n";
    }
    p.r.ok = true;
    if (p.r.fetched > 0 || p.r.failed > 0) {
        std::cout << "Philips ez Share: " << p.r.dirs_listed << " folder(s) listed, "
                  << p.r.fetched << " file(s) fetched, " << p.r.unchanged << " unchanged, "
                  << p.r.failed << " failed" << std::endl;
    }
    return p.r;
}

}  // namespace hms_cpap
