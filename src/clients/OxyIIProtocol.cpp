#include "clients/OxyIIProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace hms_cpap::oxyii {

namespace {

// MD5("lepucloud"), the protocol's fixed salt.
constexpr uint8_t kLepucloudMd5[16] = {
    0xc2, 0xa7, 0xcf, 0x50, 0xda, 0xfe, 0xd8, 0x85,
    0xa8, 0xf8, 0xf7, 0xea, 0xc4, 0x43, 0x35, 0xf3,
};

bool isRingName(const uint8_t* s) {
    for (size_t i = 0; i < kNameLen; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

bool plausible(const DateTime& t) {
    return t.year >= 2020 && t.year <= 2099 && t.month >= 1 && t.month <= 12 &&
           t.day >= 1 && t.day <= 31 && t.hour <= 23 && t.minute <= 59 && t.second <= 59;
}

std::string printable(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i] != 0; i++)
        s.push_back((p[i] >= 0x20 && p[i] < 0x7F) ? static_cast<char>(p[i]) : '?');
    return s;
}

} // namespace

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                               : static_cast<uint8_t>(crc << 1);
    }
    return crc;
}

std::vector<uint8_t> encode(uint8_t op, uint8_t seq, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f;
    f.reserve(kOverhead + payload.size());
    const auto len = static_cast<uint16_t>(payload.size());
    f.push_back(kLead);
    f.push_back(op);
    f.push_back(static_cast<uint8_t>(~op));
    f.push_back(0x00);
    f.push_back(seq);
    f.push_back(static_cast<uint8_t>(len & 0xFF));
    f.push_back(static_cast<uint8_t>(len >> 8));
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(crc8(f.data(), f.size()));
    return f;
}

std::optional<Frame> takeFrame(std::vector<uint8_t>& buf) {
    for (size_t i = 0; i < buf.size(); i++) {
        if (buf[i] != kLead) continue;
        const size_t left = buf.size() - i;
        if (left < 3) { buf.erase(buf.begin(), buf.begin() + i); return std::nullopt; }
        if (buf[i + 2] != static_cast<uint8_t>(~buf[i + 1])) continue;
        if (left < kHeaderLen) { buf.erase(buf.begin(), buf.begin() + i); return std::nullopt; }
        const size_t plen = buf[i + 5] | (static_cast<size_t>(buf[i + 6]) << 8);
        if (plen > kMaxPayload) continue;
        const size_t total = kOverhead + plen;
        if (left < total) { buf.erase(buf.begin(), buf.begin() + i); return std::nullopt; }
        if (crc8(buf.data() + i, total - 1) != buf[i + total - 1]) continue;

        Frame f;
        f.op = buf[i + 1];
        f.payload.assign(buf.begin() + i + kHeaderLen, buf.begin() + i + kHeaderLen + plen);
        buf.erase(buf.begin(), buf.begin() + i + total);
        return f;
    }
    buf.clear();
    return std::nullopt;
}

std::vector<uint8_t> authPayload(uint32_t ts) {
    uint8_t key[16];
    for (int i = 0; i < 8; i++) key[i] = kLepucloudMd5[i * 2];
    std::memcpy(key + 8, "0000", 4);
    for (int n = 0; n < 4; n++) key[12 + n] = static_cast<uint8_t>(ts >> n);
    std::vector<uint8_t> out(16);
    for (int i = 0; i < 16; i++) out[i] = key[i] ^ kLepucloudMd5[i];
    return out;
}

std::vector<uint8_t> timePayload(const DateTime& t) {
    return {static_cast<uint8_t>(t.year & 0xFF), static_cast<uint8_t>(t.year >> 8),
            t.month, t.day, t.hour, t.minute, t.second, 0x00};
}

uint32_t epochOf(const DateTime& t) {
    // Days from civil (Howard Hinnant).
    const int y = static_cast<int>(t.year) - (t.month <= 2);
    const int era = y / 400;
    const int yoe = y - era * 400;
    const int mp = (t.month + 9) % 12;
    const int doy = (153 * mp + 2) / 5 + t.day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = static_cast<long>(era) * 146097 + doe - 719468;
    return static_cast<uint32_t>(days * 86400L + t.hour * 3600L + t.minute * 60L + t.second);
}

std::optional<Info> parseInfo(const std::vector<uint8_t>& p) {
    if (p.size() < 38) return std::nullopt;
    Info info;
    info.firmware = printable(p.data() + 9, 8);
    info.clock.year = static_cast<uint16_t>(p[24] | (p[25] << 8));
    info.clock.month = p[26];
    info.clock.day = p[27];
    info.clock.hour = p[28];
    info.clock.minute = p[29];
    info.clock.second = p[30];
    info.clock_valid = plausible(info.clock);
    const size_t sn_len = p[37];
    if (sn_len > 0 && 38 + sn_len <= p.size()) info.serial = printable(p.data() + 38, sn_len);
    return info;
}

std::optional<Live> parseLive(const std::vector<uint8_t>& p) {
    if (p.size() < 24) return std::nullopt;
    Live l;
    l.state = p[5];
    l.spo2 = p[6];
    l.motion = p[7];
    l.hr = p[8];
    l.battery = p[13];
    l.reading = l.state != 0 && l.spo2 >= 1 && l.spo2 <= 100 && l.hr != 0 && l.hr != 0xFF;
    return l;
}

std::vector<std::string> parseFileList(const std::vector<uint8_t>& p) {
    std::vector<std::string> names;
    if (p.empty()) return names;
    const size_t count = p[0];
    const size_t slots = std::min(count, (p.size() - 1) / 16);
    for (size_t i = 0; i < slots; i++) {
        const uint8_t* s = p.data() + 1 + i * 16;
        if (!isRingName(s)) continue;
        names.emplace_back(std::string(reinterpret_cast<const char*>(s), kNameLen) + kFileExt);
    }
    return names;
}

std::optional<std::vector<uint8_t>> fileStartPayload(const std::string& filename) {
    std::string name = filename;
    const std::string ext = kFileExt;
    if (name.size() == kNameLen + ext.size() && name.compare(kNameLen, ext.size(), ext) == 0)
        name.resize(kNameLen);
    if (name.size() != kNameLen || !isRingName(reinterpret_cast<const uint8_t*>(name.data())))
        return std::nullopt;
    // [0..15] the name slot, [16..19] u32 file type 0 = oximetry.
    std::vector<uint8_t> out(20, 0);
    std::memcpy(out.data(), name.data(), kNameLen);
    return out;
}

std::vector<uint8_t> fileDataPayload(uint32_t offset) {
    return {static_cast<uint8_t>(offset), static_cast<uint8_t>(offset >> 8),
            static_cast<uint8_t>(offset >> 16), static_cast<uint8_t>(offset >> 24)};
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool nameMatches(const std::string& name) {
    std::string lower = name.substr(0, 5);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "s8-aw" || lower == "t8520";
}

} // namespace hms_cpap::oxyii
