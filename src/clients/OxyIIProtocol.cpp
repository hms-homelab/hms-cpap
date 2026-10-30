#include "clients/OxyIIProtocol.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

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

// AES-128-ECB over whole blocks, no padding: PKCS7 is done by the callers, so
// a bad pad is told apart from a crypto failure.
bool aesBlocks(const Key& key, const uint8_t* in, size_t n, uint8_t* out, bool enc) {
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> c(EVP_CIPHER_CTX_new(),
                                                                      EVP_CIPHER_CTX_free);
    if (!c || EVP_CipherInit_ex(c.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr,
                                enc ? 1 : 0) != 1)
        return false;
    EVP_CIPHER_CTX_set_padding(c.get(), 0);
    int len = 0;
    return EVP_CipherUpdate(c.get(), out, &len, in, static_cast<int>(n)) == 1 &&
           static_cast<size_t>(len) == n;
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

std::optional<Key> parseAuthReply(const std::vector<uint8_t>& p) {
    if (p.size() < 4 + kKeyLen) return std::nullopt;
    uint8_t d[4 + kKeyLen];
    for (size_t i = 0; i < sizeof(d); i++) d[i] = p[i] ^ kLepucloudMd5[i % 16];
    if (d[0] != 0x01 || d[1] != kKeyLen) return std::nullopt;
    Key key;
    std::memcpy(key.data(), d + 4, kKeyLen);
    return key;
}

bool replyEncrypted(uint8_t op) {
    switch (op) {
    case 0x00: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
    case 0x11: case 0x12: case 0x13: case 0x14:
    case 0xE0: case 0xE1: case 0xE4:
    case 0xF1: case 0xF2: case 0xF3:
        return true;
    default:
        return false;
    }
}

std::vector<uint8_t> encrypt(const Key& key, const std::vector<uint8_t>& p) {
    const size_t pad = 16 - (p.size() % 16);
    std::vector<uint8_t> in(p);
    in.insert(in.end(), pad, static_cast<uint8_t>(pad));
    std::vector<uint8_t> out(in.size());
    if (!aesBlocks(key, in.data(), in.size(), out.data(), true)) return {};
    return out;
}

std::optional<std::vector<uint8_t>> decrypt(const Key& key, const std::vector<uint8_t>& p) {
    if (p.empty() || p.size() % 16 != 0) return std::nullopt;
    std::vector<uint8_t> out(p.size());
    if (!aesBlocks(key, p.data(), p.size(), out.data(), false)) return std::nullopt;
    const uint8_t pad = out.back();
    if (pad < 1 || pad > 16) return std::nullopt;
    for (size_t i = 1; i <= pad; i++)
        if (out[out.size() - i] != pad) return std::nullopt;
    out.resize(out.size() - pad);
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
    if (p.size() < 14) return std::nullopt;
    const uint16_t pr = static_cast<uint16_t>(p[8] | (p[9] << 8));
    Live l;
    l.run = p[4];
    l.state = p[5];
    l.spo2 = p[6];
    l.pi_x10 = p[7];
    l.hr = pr < 0xFF ? static_cast<uint8_t>(pr) : 0xFF;
    l.motion = p[11];
    l.battery_state = p[12];
    l.battery = p[13];
    // 0, 127 and 255 are the SDK's "no value" for SpO2; 0, 255, 511 and 65535
    // for the pulse rate. Only sensor state 1 is a finger on the ring.
    l.reading = l.state == 1 && l.spo2 >= 1 && l.spo2 <= 100 && pr > 0 && pr < 0xFF;
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
