#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::as11 {

/**
 * The AirSense 11's own Bluetooth protocol (SDD-057): its "FIG" framing, the
 * session cipher and the SRP-6a pairing.
 *
 * Pure functions over bytes on OpenSSL, no Bluetooth, so the test binary
 * covers every byte the direct client puts on the air. The GATT side lives in
 * AirSense11BleClient; the JSON-RPC bodies are built there.
 *
 * Every message, both directions, is one frame:
 *
 *   0   4  magic   CA FE BA BE, on the wire BE BA FE CA
 *   4   2  type    u16 LE: 0x0393 app->machine clear, 0x0392 machine->app clear,
 *                  0x0397 app->machine encrypted, 0x0396 machine->app encrypted;
 *                  bit 0x0004 says encrypted
 *   6   2  length  u16 LE, of the payload
 *   8   4  pcrc    CRC-32 (zlib) of the payload, LE
 *   12  4  hcrc    CRC-32 of bytes 4..11, LE
 *   16  n  payload JSON, or IV(16) || AES-256-CBC(len(2 LE) || JSON || zero pad)
 *
 * A notification may carry a piece of a frame; the Reassembler joins them and
 * drops a frame whose CRC fails, resyncing on the next magic.
 */

constexpr size_t kHeaderLen = 16;
constexpr uint16_t kDevPlain = 0x0392;
constexpr uint16_t kAppPlain = 0x0393;
constexpr uint16_t kDevEncrypted = 0x0396;
constexpr uint16_t kAppEncrypted = 0x0397;
constexpr uint16_t kEncryptedBit = 0x0004;

// The largest frame the machine sends: a spool fragment of maxFragmentSize
// 2808 bytes is base64 (3744) inside JSON inside the cipher.
constexpr size_t kMaxPayload = 4096;

constexpr const char* kServiceUuid = "0000fd56-0000-1000-8000-00805f9b34fb";
constexpr const char* kWriteUuid = "a6220002-35f1-4b20-afae-cb089d2044aa";
constexpr const char* kNotifyUuid = "a6220003-35f1-4b20-afae-cb089d2044aa";

uint32_t crc32(const uint8_t* data, size_t len);

/// One frame; empty when the payload is over 65535 bytes.
std::vector<uint8_t> buildFrame(uint16_t type, const std::vector<uint8_t>& payload);

struct Frame {
    uint16_t type = 0;
    std::vector<uint8_t> payload;
};

/// Joins notifications into frames.
class Reassembler {
public:
    void reset() { buf_.clear(); }
    void feed(const uint8_t* data, size_t len);
    void feed(const std::vector<uint8_t>& data) { feed(data.data(), data.size()); }
    /// The next whole, valid frame. Call again until nullopt: one notification
    /// may complete more than one frame.
    std::optional<Frame> next();

private:
    std::vector<uint8_t> buf_;
    long nextHeader(size_t from) const;
    void drop(size_t n);
};

using Key = std::array<uint8_t, 32>;

/// The session cipher: sessionKey = SHA-256(masterPairKey || nonce); a payload
/// is IV || AES-256-CBC(len(2 LE) || JSON || zero padding to 16).
class Cipher {
public:
    Cipher(const Key& master, const std::vector<uint8_t>& nonce);
    const Key& key() const { return key_; }
    /// The caller supplies the IV so a test is deterministic; the client draws
    /// it from RAND_bytes. Empty on a crypto failure or JSON over 65535 bytes.
    std::vector<uint8_t> encrypt(const std::array<uint8_t, 16>& iv, const std::string& json) const;
    /// The JSON, or nullopt when the payload is malformed.
    std::optional<std::string> decrypt(const std::vector<uint8_t>& payload) const;

private:
    Key key_{};
};

/// HMAC-SHA256(masterPairKey, challenge) as upper-case hex: the
/// CheckSessionIntegrity response. challenge_hex is the machine's hex.
std::optional<std::string> challengeResponse(const Key& master, const std::string& challenge_hex);

std::string hexEncode(const uint8_t* in, size_t n);   // upper case
inline std::string hexEncode(const std::vector<uint8_t>& v) { return hexEncode(v.data(), v.size()); }
std::optional<std::vector<uint8_t>> hexDecode(const std::string& hex);
std::optional<std::vector<uint8_t>> base64Decode(const std::string& in);

/**
 * SRP-6a as the AirSense 11 pairs, the first time, with the code on its
 * screen:
 *
 *   group RFC 5054 2048-bit, g = 2, H = SHA-256, PAD() = left-pad to 256 bytes,
 *   no identity; the password is the screen code as ASCII
 *   a     32 random bytes,  A = g^a mod N
 *   u     H(PAD(A) | PAD(B)),  k = H(PAD(N) | PAD(g)),  x = H(salt | H(code))
 *   S     (B - k*g^x)^(a + u*x) mod N
 *   K     H(PAD(S))                                    -> masterPairKey
 *   M1    H(H(PAD(N)) xor H(PAD(g)) | salt | PAD(A) | PAD(B) | K)
 *   M2    H(PAD(A) | M1 | K)
 *
 * The client sends A (StartKeyExchange); only then does the machine show the
 * code. It gets B and the salt, sends M1 (ConfirmKeyExchange) within the
 * machine's pairing window (under a minute), gets M2, a clientId and a nonce.
 * K is kept for every later session; the code is never stored.
 */
class Srp {
public:
    static constexpr size_t kNBytes = 256;

    /// Step 1: A from the caller's 32 random bytes. False on a bignum failure.
    bool start(const std::array<uint8_t, 32>& a);
    /// PAD(A) as upper-case hex, for StartKeyExchange.
    std::string clientPublicHex() const { return hexEncode(A_.data(), A_.size()); }

    /// Step 2: the machine's serverPk and salt (hex), the screen code. M1 as
    /// upper-case hex; nullopt when B or u is zero mod N (an invalid server),
    /// the hex is malformed, or a bignum fails.
    std::optional<std::string> challenge(const std::string& server_pk_hex,
                                         const std::string& salt_hex, const std::string& code);

    /// Step 3: true when the machine's serverConfirmation (hex) is M2 for this
    /// exchange: the code was right and the machine is the one we talked to.
    bool verify(const std::string& m2_hex) const;

    /// The masterPairKey, once challenge() succeeded.
    const Key& masterKey() const { return K_; }

private:
    std::array<uint8_t, 32> a_{};
    std::array<uint8_t, kNBytes> A_{};
    std::array<uint8_t, kNBytes> B_{};
    std::vector<uint8_t> salt_;
    Key K_{};
    std::array<uint8_t, 32> M1_{};
    bool have_A_ = false, have_K_ = false;
};

} // namespace hms_cpap::as11
