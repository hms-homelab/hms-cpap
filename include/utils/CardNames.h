#pragma once
//
// SDD-054 D7: a name a card lists that is not a name.
//
// A damaged or clone card can list its root as about a thousand entries whose
// names are all 0xFF bytes, dated 2107-15-31 31:63:62 (every bit of the FAT
// date and time set): blank flash read back as a directory. Such a name cannot
// be stored (PostgreSQL refuses invalid UTF-8 in a text column), cannot be
// requested back from the card, and means nothing. Listings drop it, count it,
// and say so once per listing.
//
#include <string>

namespace hms_cpap {

/// Strict UTF-8 with no NUL: no overlongs, no surrogates, nothing past U+10FFFF.
inline bool isCardNameUsable(const std::string& s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == 0) return false;
        if (c < 0x80) { ++i; continue; }
        int len = 0;
        unsigned cp = 0;
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + static_cast<std::size_t>(len) > n) return false;
        for (int k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += static_cast<std::size_t>(len);
    }
    return true;
}

}  // namespace hms_cpap
