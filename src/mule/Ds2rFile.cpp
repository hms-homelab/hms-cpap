#include "mule/Ds2rFile.h"

#include <cpapdash/ds2ble/Protocol.h>

#include <cstdio>

namespace hms_cpap::mule::ds2r {

namespace ds2 = cpapdash::ds2ble;

ds2::Identity identity(const ds2::ReadFile& f) {
    ds2::Identity id;
    id.serial   = f.machineSerial();
    id.model    = f.model();
    id.firmware = f.firmware();
    if (const auto n = f.newestSession()) id.newest_session = *n;
    if (const auto* a = f.answer(ds2::kCmdCommands, 0x00))
        for (size_t i = 0; i + 1 < a->data.size(); i += 2)
            id.commands.push_back(static_cast<uint16_t>((a->data[i] << 8) | a->data[i + 1]));
    if (const auto* a = f.answer(ds2::kCmdCounters, 0x00)) id.counters = a->data;
    return id;
}

std::string resultToken(uint8_t result, uint8_t status) {
    static const char* const k[] = {"",           "mode_off",  "busy",          "not_found",     "connect",
                                    "not_paired", "bond_lost", "protocol",      "aborted",       "pair_failed",
                                    "bonding",    "link_lost", "other_machine", "code_mismatch"};
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
