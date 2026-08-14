#include "utils/CardResidue.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace hms_cpap {

namespace {
std::string toLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
}  // namespace

bool isCpapEdf(const std::string& name) {
    const std::string n = toLower(name);
    static const char* kTypes[] = {"_brp.edf", "_eve.edf", "_sad.edf",
                                   "_sa2.edf", "_pld.edf", "_csl.edf"};
    for (const char* t : kTypes)
        if (endsWith(n, t)) return true;
    return false;
}

bool residualSkip(const std::string& name, uint64_t size_bytes) {
    const std::string lower = toLower(name);
    {
        auto d = lower.find_last_of('.');
        const std::string e = (d == std::string::npos) ? "" : lower.substr(d);

        // WHOLE-CARD CONTAINERS ARE EXEMPT FROM THE SIZE CAP, checked before it.
        //
        // A Lowenstein Prisma card is not a browsable tree: it carries one
        // compressed blob, therapy.pdat, holding the entire therapy history
        // (8.8 MB in a real 2026-07 client upload, and it only grows), with
        // config.pcfg beside it. The cap exists to stop somebody's holiday video
        // riding along; applied to a container it would drop the ONLY file on
        // the card and the sweep would report a clean, complete, empty pass.
        // Same rule as the cloud's Tier 4 sweep (hms-cpapdash-api SDD-049).
        if (e == ".pdat" || e == ".pcfg") return false;
    }

    if (size_bytes > 20ull * 1024 * 1024) return true;  // >20 MB, not a card file
    if (lower.rfind("._", 0) == 0) return true;  // AppleDouble sidecars
    if (lower == ".ds_store" || lower == "thumbs.db" ||
        lower == "desktop.ini" || lower == "ezshare.cfg") return true;
    auto dot = lower.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : lower.substr(dot);
    static const std::set<std::string> deny = {
        ".jpg",".jpeg",".png",".gif",".bmp",".tif",".tiff",".heic",".webp",".svg",
        ".ico",".raw",".cr2",".nef",".dng",
        ".mp4",".mov",".avi",".mkv",".wmv",".flv",".webm",".m4v",".mpg",".mpeg",".3gp",
        ".mp3",".wav",".aac",".flac",".ogg",".m4a",".wma",
        ".docx",".doc",".xlsx",".xls",".pptx",".ppt",".pdf",".pages",".numbers",
        ".key",".rtf",".odt",".ods",
        ".rar",".7z",".gz",".tar",".dmg",".iso",".exe",".app",".pkg",
        // *** .zip and .bin are deliberately NOT denied (SDD-049). ***
        // Extension is a bad proxy for junk on a CPAP card, and size is the
        // honest gate. A vendor's whole-card container may well arrive as
        // either, and denying the extension would drop the only file there is.
    };
    return deny.count(ext) > 0;
}

}  // namespace hms_cpap
