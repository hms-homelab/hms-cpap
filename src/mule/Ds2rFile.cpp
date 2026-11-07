#include "mule/Ds2rFile.h"

#include <cpapdash/ds2ble/Protocol.h>

#include <cstdio>
#include <cstring>

namespace hms_cpap::mule::ds2r {

namespace ds2 = cpapdash::ds2ble;

namespace {

constexpr uint8_t kMagic[5] = {'D', 'S', '2', 'R', 0x01};

std::string cstr(const std::vector<uint8_t>& d) {
    std::string s;
    for (const uint8_t c : d) {
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

uint32_t u32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

}  // namespace

std::optional<File> parse(const std::string& bytes, std::string* why) {
    const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t n = bytes.size();
    if (n < sizeof(kMagic) || std::memcmp(p, kMagic, 4) != 0) {
        if (why) *why = "not a .ds2r (no DS2R magic)";
        return std::nullopt;
    }
    if (p[4] != kMagic[4]) {
        if (why) *why = "a .ds2r of version " + std::to_string(p[4]) + ", this reads version 1";
        return std::nullopt;
    }
    File f;
    size_t at = sizeof(kMagic);
    while (at < n) {
        if (n - at < 5) {
            f.cut = true;
            break;
        }
        const uint8_t kind = p[at];
        const uint32_t len = p[at + 1] | (p[at + 2] << 8) | (p[at + 3] << 16) |
                             (static_cast<uint32_t>(p[at + 4]) << 24);
        at += 5;
        if (len > n - at) {
            f.cut = true;   // the stream broke inside this item
            break;
        }
        const uint8_t* q = p + at;
        at += len;
        switch (kind) {
            case kItemAnswer: {
                if (len < 4) continue;   // no status byte: nothing to read
                Answer a;
                a.cmd    = static_cast<uint16_t>((q[0] << 8) | q[1]);
                a.sub    = q[2];
                a.status = q[3];
                a.data.assign(q + 4, q + len);
                f.answers.push_back(std::move(a));
                break;
            }
            case kItemRecord: {
                if (len < 1 + 4 + ds2::kBookmarkSize) continue;
                ds2::Record r;
                r.stream  = q[0];
                r.session = u32be(q + 1);
                r.bookmark_out.assign(q + 5, q + 5 + ds2::kBookmarkSize);
                r.body.assign(q + 5 + ds2::kBookmarkSize, q + len);
                f.records.push_back(std::move(r));
                break;
            }
            case kItemStreamEnd: {
                if (len < 4 + ds2::kBookmarkSize) continue;
                StreamEnd e;
                e.stream  = q[0];
                e.how     = q[1];
                e.records = static_cast<uint16_t>(q[2] | (q[3] << 8));
                e.bookmark.assign(q + 4, q + 4 + ds2::kBookmarkSize);
                f.stream_ends.push_back(std::move(e));
                break;
            }
            case kItemEnd: {
                if (len < 3) continue;
                f.end = End{q[0], q[1] != 0, q[2]};
                break;
            }
            default:
                break;   // a later version's item: skipped by its length
        }
    }
    if (!f.end) f.cut = true;
    return f;
}

ds2::Identity identity(const File& f) {
    ds2::Identity id;
    for (const auto& a : f.answers) {
        if (a.status != ds2::kStatusOk) continue;
        switch (a.cmd) {
            case ds2::kCmdSerial: id.serial = cstr(a.data); break;
            case ds2::kCmdFirmware: id.firmware = cstr(a.data); break;
            case ds2::kCmdModel: id.model = cstr(a.data); break;
            case ds2::kCmdCommands:
                for (size_t i = 0; i + 1 < a.data.size(); i += 2)
                    id.commands.push_back(static_cast<uint16_t>((a.data[i] << 8) | a.data[i + 1]));
                break;
            case ds2::kCmdCursor:
                if (a.sub == ds2::kCurNewest && a.data.size() >= 4) id.newest_session = u32be(a.data.data());
                break;
            case ds2::kCmdCounters: id.counters = a.data; break;
            default: break;
        }
    }
    return id;
}

std::string resultToken(uint8_t result, uint8_t status) {
    static const char* const k[] = {"",       "mode_off",  "busy",          "not_found",     "connect",
                                    "not_paired", "bond_lost", "protocol",   "aborted",       "pair_failed",
                                    "bonding", "link_lost", "other_machine", "code_mismatch"};
    if (result == 7 && status != 0) {
        char b[16];
        std::snprintf(b, sizeof b, "status_%02x", status);
        return b;
    }
    if (result < sizeof(k) / sizeof(k[0])) return k[result];
    return "result_" + std::to_string(result);
}

bool noBookmark(const ds2::Bookmark& b) {
    for (const uint8_t c : b)
        if (c != 0) return false;
    return true;
}

std::string toHex(const ds2::Bookmark& b) {
    std::string s;
    char h[3];
    for (const uint8_t c : b) {
        std::snprintf(h, sizeof h, "%02x", c);
        s += h;
    }
    return s;
}

std::optional<ds2::Bookmark> fromHex(const std::string& s) {
    if (s.size() != 2 * ds2::kBookmarkSize) return std::nullopt;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    ds2::Bookmark b;
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        b.push_back(static_cast<uint8_t>(hi * 16 + lo));
    }
    return b;
}

}  // namespace hms_cpap::mule::ds2r
