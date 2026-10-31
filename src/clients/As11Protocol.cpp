#include "clients/As11Protocol.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstring>
#include <initializer_list>
#include <memory>

namespace hms_cpap::as11 {

namespace {

constexpr uint8_t kMagic[4] = {0xBE, 0xBA, 0xFE, 0xCA};

// RFC 5054, 2048-bit group.
constexpr const char* kNHex =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CB"
    "B4A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0"
    "CF6095179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740A"
    "DBF4FF747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481"
    "F1D2B9078717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDB"
    "F52FB3786160279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C382"
    "71AE35F8E9DBFBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F"
    "9E4AFF73";

void putLe16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void putLe32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// SHA-256 over several parts.
struct Part { const uint8_t* p; size_t n; };
std::array<uint8_t, 32> sha256(std::initializer_list<Part> parts) {
    std::array<uint8_t, 32> out{};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> c(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!c || EVP_DigestInit_ex(c.get(), EVP_sha256(), nullptr) != 1) return out;
    for (const auto& part : parts) EVP_DigestUpdate(c.get(), part.p, part.n);
    EVP_DigestFinal_ex(c.get(), out.data(), nullptr);
    return out;
}

// AES-256-CBC over whole blocks, no padding (the frame carries its own).
bool aes256cbc(const Key& key, const uint8_t iv[16], const uint8_t* in, size_t n,
               uint8_t* out, bool enc) {
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> c(EVP_CIPHER_CTX_new(),
                                                                      EVP_CIPHER_CTX_free);
    if (!c || EVP_CipherInit_ex(c.get(), EVP_aes_256_cbc(), nullptr, key.data(), iv,
                                enc ? 1 : 0) != 1)
        return false;
    EVP_CIPHER_CTX_set_padding(c.get(), 0);
    int len = 0;
    return EVP_CipherUpdate(c.get(), out, &len, in, static_cast<int>(n)) == 1 &&
           static_cast<size_t>(len) == n;
}

using BnPtr = std::unique_ptr<BIGNUM, decltype(&BN_clear_free)>;
BnPtr bn() { return BnPtr(BN_new(), BN_clear_free); }

// PAD(x): x as 256 big-endian bytes. False when x does not fit.
bool pad(const BIGNUM* x, uint8_t* out) {
    return BN_bn2binpad(x, out, static_cast<int>(Srp::kNBytes)) == static_cast<int>(Srp::kNBytes);
}

bool readHex(BIGNUM* out, const std::string& hex) {
    auto bytes = hexDecode(hex);
    return bytes && !bytes->empty() &&
           BN_bin2bn(bytes->data(), static_cast<int>(bytes->size()), out) != nullptr;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

// ── CRC-32, the zlib one (poly 0xEDB88320 reflected, init and xor 0xFFFFFFFF) ──

uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> buildFrame(uint16_t type, const std::vector<uint8_t>& payload) {
    if (payload.size() > 0xFFFF) return {};
    std::vector<uint8_t> out(kHeaderLen + payload.size());
    std::memcpy(out.data(), kMagic, 4);
    putLe16(out.data() + 4, type);
    putLe16(out.data() + 6, static_cast<uint16_t>(payload.size()));
    putLe32(out.data() + 8, crc32(payload.data(), payload.size()));
    putLe32(out.data() + 12, crc32(out.data() + 4, 8));
    std::copy(payload.begin(), payload.end(), out.begin() + kHeaderLen);
    return out;
}

// ── reassembly ──

void Reassembler::feed(const uint8_t* data, size_t len) {
    constexpr size_t cap = kHeaderLen + kMaxPayload;
    if (len > cap) return;
    if (buf_.size() + len > cap) {
        // Older than a whole buffer with no frame in it: it is noise. Keep the
        // tail, which may hold the start of the frame being received.
        drop(buf_.size() - (cap - len));
    }
    buf_.insert(buf_.end(), data, data + len);
}

void Reassembler::drop(size_t n) {
    if (n >= buf_.size()) { buf_.clear(); return; }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(n));
}

// Offset of the first magic at or after `from` whose header checks, or -1.
long Reassembler::nextHeader(size_t from) const {
    for (size_t i = from; i + kHeaderLen <= buf_.size(); i++) {
        if (std::memcmp(buf_.data() + i, kMagic, 4) != 0) continue;
        if (crc32(buf_.data() + i + 4, 8) == le32(buf_.data() + i + 12)) return static_cast<long>(i);
    }
    return -1;
}

std::optional<Frame> Reassembler::next() {
    for (;;) {
        // Find a magic; what precedes it can never start a frame.
        size_t i = 0;
        while (i + 4 <= buf_.size() && std::memcmp(buf_.data() + i, kMagic, 4) != 0) i++;
        if (i + 4 > buf_.size()) {
            // No magic: keep at most the last three bytes (a magic split).
            if (buf_.size() > 3) drop(buf_.size() - 3);
            return std::nullopt;
        }
        if (i) drop(i);
        if (buf_.size() < kHeaderLen) return std::nullopt;

        if (crc32(buf_.data() + 4, 8) != le32(buf_.data() + 12)) {
            drop(1);   // a false magic: resync
            continue;
        }
        const size_t ln = le16(buf_.data() + 6);
        if (ln > kMaxPayload) { drop(1); continue; }
        if (buf_.size() < kHeaderLen + ln) {
            // Still waiting. If a valid header already sits inside what we
            // have, a notification of this frame was lost: skip to it.
            const long nxt = nextHeader(1);
            if (nxt < 0) return std::nullopt;
            drop(static_cast<size_t>(nxt));
            continue;
        }
        if (crc32(buf_.data() + kHeaderLen, ln) != le32(buf_.data() + 8)) {
            // The declared length ran into the next frame (a lost piece): skip
            // only this header, so the next frame is not eaten.
            drop(1);
            continue;
        }
        Frame f;
        f.type = le16(buf_.data() + 4);
        f.payload.assign(buf_.begin() + kHeaderLen, buf_.begin() + static_cast<long>(kHeaderLen + ln));
        drop(kHeaderLen + ln);
        return f;
    }
}

// ── session cipher ──

Cipher::Cipher(const Key& master, const std::vector<uint8_t>& nonce)
    : key_(sha256({{master.data(), master.size()}, {nonce.data(), nonce.size()}})) {}

std::vector<uint8_t> Cipher::encrypt(const std::array<uint8_t, 16>& iv, const std::string& json) const {
    if (json.size() > 0xFFFF) return {};
    const size_t padded = (2 + json.size() + 15) & ~static_cast<size_t>(15);
    std::vector<uint8_t> plain(padded, 0);
    putLe16(plain.data(), static_cast<uint16_t>(json.size()));
    std::memcpy(plain.data() + 2, json.data(), json.size());

    std::vector<uint8_t> out(16 + padded);
    std::copy(iv.begin(), iv.end(), out.begin());
    if (!aes256cbc(key_, iv.data(), plain.data(), padded, out.data() + 16, true)) return {};
    return out;
}

std::optional<std::string> Cipher::decrypt(const std::vector<uint8_t>& payload) const {
    if (payload.size() < 32 || (payload.size() - 16) % 16 != 0) return std::nullopt;
    const size_t ct = payload.size() - 16;
    std::vector<uint8_t> plain(ct);
    if (!aes256cbc(key_, payload.data(), payload.data() + 16, ct, plain.data(), false))
        return std::nullopt;
    const size_t ln = le16(plain.data());
    if (ln + 2 > ct) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(plain.data() + 2), ln);
}

std::optional<std::string> challengeResponse(const Key& master, const std::string& challenge_hex) {
    auto ch = hexDecode(challenge_hex);
    if (!ch) return std::nullopt;
    uint8_t mac[32];
    unsigned int n = 0;
    if (!HMAC(EVP_sha256(), master.data(), static_cast<int>(master.size()), ch->data(), ch->size(),
              mac, &n) || n != 32)
        return std::nullopt;
    return hexEncode(mac, 32);
}

// ── helpers ──

std::string hexEncode(const uint8_t* in, size_t n) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out(2 * n, '0');
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xF];
    }
    return out;
}

std::optional<std::vector<uint8_t>> hexDecode(const std::string& hex) {
    if (hex.size() % 2) return std::nullopt;
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

std::optional<std::vector<uint8_t>> base64Decode(const std::string& in) {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        else return std::nullopt;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// ── SRP-6a ──

bool Srp::start(const std::array<uint8_t, 32>& a) {
    *this = Srp{};
    a_ = a;
    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
    auto N = bn(), g = bn(), av = bn(), A = bn();
    have_A_ = ctx && N && g && av && A && readHex(N.get(), kNHex) && BN_set_word(g.get(), 2) &&
              BN_bin2bn(a.data(), static_cast<int>(a.size()), av.get()) &&
              BN_mod_exp(A.get(), g.get(), av.get(), N.get(), ctx.get()) && pad(A.get(), A_.data());
    return have_A_;
}

std::optional<std::string> Srp::challenge(const std::string& server_pk_hex,
                                          const std::string& salt_hex, const std::string& code) {
    if (!have_A_) return std::nullopt;
    auto salt = hexDecode(salt_hex);
    if (!salt || salt->empty()) return std::nullopt;
    salt_ = *salt;

    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
    auto N = bn(), g = bn(), B = bn(), a = bn(), u = bn(), k = bn(), x = bn();
    auto gx = bn(), kgx = bn(), base = bn(), ux = bn(), exp = bn(), S = bn(), tmp = bn();
    if (!ctx || !N || !g || !B || !a || !u || !k || !x || !gx || !kgx || !base || !ux || !exp ||
        !S || !tmp)
        return std::nullopt;

    std::array<uint8_t, kNBytes> padN{}, padg{}, padS{};
    if (!readHex(N.get(), kNHex) || !BN_set_word(g.get(), 2)) return std::nullopt;
    if (!readHex(B.get(), server_pk_hex)) return std::nullopt;
    // B mod N must not be zero.
    if (!BN_mod(tmp.get(), B.get(), N.get(), ctx.get()) || BN_is_zero(tmp.get())) return std::nullopt;
    if (!pad(B.get(), B_.data())) return std::nullopt;
    if (!BN_bin2bn(a_.data(), static_cast<int>(a_.size()), a.get())) return std::nullopt;
    if (!pad(N.get(), padN.data()) || !pad(g.get(), padg.data())) return std::nullopt;

    // u = H(PAD(A) | PAD(B)), must not be zero
    auto h = sha256({{A_.data(), kNBytes}, {B_.data(), kNBytes}});
    if (!BN_bin2bn(h.data(), 32, u.get()) || BN_is_zero(u.get())) return std::nullopt;
    // k = H(PAD(N) | PAD(g))
    h = sha256({{padN.data(), kNBytes}, {padg.data(), kNBytes}});
    if (!BN_bin2bn(h.data(), 32, k.get())) return std::nullopt;
    // x = H(salt | H(code))
    const auto hcode = sha256({{reinterpret_cast<const uint8_t*>(code.data()), code.size()}});
    h = sha256({{salt_.data(), salt_.size()}, {hcode.data(), 32}});
    if (!BN_bin2bn(h.data(), 32, x.get())) return std::nullopt;

    // S = (B - k*g^x mod N)^(a + u*x) mod N
    if (!BN_mod_exp(gx.get(), g.get(), x.get(), N.get(), ctx.get()) ||
        !BN_mod_mul(kgx.get(), k.get(), gx.get(), N.get(), ctx.get()) ||
        !BN_mod_sub(base.get(), B.get(), kgx.get(), N.get(), ctx.get()) ||   // non-negative
        !BN_mul(ux.get(), u.get(), x.get(), ctx.get()) ||
        !BN_add(exp.get(), a.get(), ux.get()) ||
        !BN_mod_exp(S.get(), base.get(), exp.get(), N.get(), ctx.get()) ||
        !pad(S.get(), padS.data()))
        return std::nullopt;

    // K = H(PAD(S))
    K_ = sha256({{padS.data(), kNBytes}});
    // M1 = H(H(PAD(N)) xor H(PAD(g)) | salt | PAD(A) | PAD(B) | K)
    const auto hN = sha256({{padN.data(), kNBytes}});
    const auto hg = sha256({{padg.data(), kNBytes}});
    std::array<uint8_t, 32> hng{};
    for (size_t i = 0; i < 32; i++) hng[i] = hN[i] ^ hg[i];
    M1_ = sha256({{hng.data(), 32}, {salt_.data(), salt_.size()}, {A_.data(), kNBytes},
                  {B_.data(), kNBytes}, {K_.data(), 32}});
    have_K_ = true;
    return hexEncode(M1_.data(), 32);
}

bool Srp::verify(const std::string& m2_hex) const {
    if (!have_K_) return false;
    auto given = hexDecode(m2_hex);
    if (!given || given->size() != 32) return false;
    const auto m2 = sha256({{A_.data(), kNBytes}, {M1_.data(), 32}, {K_.data(), 32}});
    // Constant time, for form's sake: a wrong M2 is a wrong machine, not a secret.
    uint8_t diff = 0;
    for (size_t i = 0; i < 32; i++) diff |= static_cast<uint8_t>(m2[i] ^ (*given)[i]);
    return diff == 0;
}

} // namespace hms_cpap::as11
