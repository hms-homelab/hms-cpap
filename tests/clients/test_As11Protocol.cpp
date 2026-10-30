#include <gtest/gtest.h>

#include "clients/As11Protocol.h"

#include <cstring>

using namespace hms_cpap::as11;

// SDD-057: the AirSense 11's framing, session cipher and SRP-6a pairing, no
// Bluetooth needed. Every vector was produced by the protocol's public
// reference implementation (MIT) with fixed random values, so a wrong byte
// here cannot agree with itself.

namespace {

std::vector<uint8_t> hex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

Key key(const std::string& h) {
    Key k{};
    const auto v = hex(h);
    std::copy(v.begin(), v.end(), k.begin());
    return k;
}

const char* kA =
    "630ACDFF5D334462D92A29E0B7FA6E20020F3333292F6D3A640F1C7A76AD9D317531C57979952E5736C88DB118D060DC0539A812B9B0AF3B4002380A9F28AE4A7C45A896542DE05FBCF76A4E7E0739B9A55D5D6C7ABA4F1E1B58729A79BC084D5FF513EAEC33CE978F5BAD87E579B5A95FC773198E22697B2EADAB9EB94F84CDCF1FE94FF09F88D4CA46E968BBA443FF71167571F19FEB052869BD28D7DABF963B7FE399A1F70E7E08D00E1A3778ED1DDDC3325DD09E05D31E774D1FD295C4ABFBC613446232004D67CB03D6A034D2CE6CA0A544A0FF5B434B4B4267FA6C6D72ACBBDA2EFC1EF1D1FE36D35382B089ABE556862AEC35B29D3D0CDF359A9CFED3";
const char* kB =
    "4A543A4E438654FD009A367BE0D7399605883DCF77D66CA9089F5EE6AA02F4DD8DE04F4AD22717770E94FA03BFDDC76ECA756EAB79832DCCF55DFF0478897243DED17928EB18850BBC1A414B081D690E089D20564F524313BBF7A9BB43A8D5FA20ED1C8353662D35281F69C6DEE2858CDD2CCAC249611A541362CB76C46166A45CBDF2D5BD02820D53915E789FBC50067ABCD4BCFE9B7B18DE6260BB1884E65F35A246E24BB0CB336F0DA1D4CA936A54F8A02E2B882DEBC2F28B98B10FD1B7F26C162C3E2AE1076EAE115D2F563612B7DC8FC322D065E2028893DC3F4690E254DF4B721CAB1B878F51FE5BB8004EC655669C54CB2033BBCD2E02A85ED860D2D0";
const char* kSalt = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
const char* kM1 = "4D83AB47D53E0B11E7E686CB8E35479E98B2830350CFDDC3A9A963D3318E1EB7";
const char* kM2 = "1EE6ADAE4718D5C203EBE7BE5CAD69FF1E2DAEC0E0D03A6B42EF606FF177F336";
const char* kK = "B93068BFDF2B1D98577F696CC022FA049273FD4AC8F42DCC5A3D13E28FF14BC2";
const char* kSession = "67E90B8C4336B3348D882C3D1F1AD07F83A38DBC4597090C89FFDF79F9F503AA";
const char* kHmac = "0BAD7C7B0B814A7EF25E2281E1EEC56058B84D251B6345ECD863A61D2BB322FF";
const char* kEnc =
    "000102030405060708090A0B0C0D0E0F5BED79EA22FA94E1493CF4FCF5C166B9C211685736EB3ED1ECC0CF13D2CC38A2143913D5E44ABCF9B66E22DBBE00BE3FDF379C23BCC075129419E067B044110A43213B33617C0CB05EC416BA60813524";
const char* kFrame = "BEBAFECA93030800C5F85D44476E769D7B226964223A317D";

std::array<uint8_t, 32> fixedA() {
    std::array<uint8_t, 32> a{};
    for (size_t i = 0; i < a.size(); i++) a[i] = static_cast<uint8_t>(i + 1);
    return a;
}

} // namespace

TEST(As11Protocol, CrcAndFrame) {
    EXPECT_EQ(crc32(reinterpret_cast<const uint8_t*>("abc"), 3), 0x352441C2u);
    EXPECT_EQ(buildFrame(kAppPlain, bytes("{\"id\":1}")), hex(kFrame));
    EXPECT_TRUE(buildFrame(kAppPlain, std::vector<uint8_t>(0x10000)).empty());
}

TEST(As11Protocol, JoinsAFrameSplitAcrossNotifications) {
    const auto f1 = buildFrame(kDevPlain, bytes("{\"id\":1}"));
    Reassembler r;
    r.feed(f1.data(), 5);
    EXPECT_FALSE(r.next().has_value());
    r.feed(f1.data() + 5, 10);
    EXPECT_FALSE(r.next().has_value());
    r.feed(f1.data() + 15, f1.size() - 15);
    auto f = r.next();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->type, kDevPlain);
    EXPECT_EQ(f->payload, bytes("{\"id\":1}"));
    EXPECT_FALSE(r.next().has_value());
}

TEST(As11Protocol, TwoFramesInOneNotificationAfterNoise) {
    const auto f1 = buildFrame(kDevPlain, bytes("{\"id\":1}"));
    const auto f2 = buildFrame(kDevEncrypted, bytes("0123456789abcdef0123456789abcdef"));
    std::vector<uint8_t> both = {0x11, 0x22};
    both.insert(both.end(), f1.begin(), f1.end());
    both.insert(both.end(), f2.begin(), f2.end());

    Reassembler r;
    r.feed(both);
    auto a = r.next();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->type, kDevPlain);
    EXPECT_EQ(a->payload.size(), 8u);
    auto b = r.next();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->type, kDevEncrypted);
    EXPECT_EQ(b->payload, bytes("0123456789abcdef0123456789abcdef"));
    EXPECT_FALSE(r.next().has_value());
}

TEST(As11Protocol, DropsAFrameWhoseMiddleWasLost) {
    // The first frame's middle is missing and the next frame starts inside its
    // declared length: the first is dropped, the second kept.
    const auto f1 = buildFrame(kDevPlain, bytes("{\"id\":1}"));
    const auto f2 = buildFrame(kDevEncrypted, bytes("0123456789abcdef0123456789abcdef"));
    std::vector<uint8_t> buf(f2.begin(), f2.begin() + 20);
    buf.insert(buf.end(), f1.begin(), f1.end());

    Reassembler r;
    r.feed(buf);
    auto f = r.next();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->type, kDevPlain);
    EXPECT_EQ(f->payload.size(), 8u);
    EXPECT_FALSE(r.next().has_value());
}

TEST(As11Protocol, AMagicInsideAPayloadIsNotAFrame) {
    const std::vector<uint8_t> body = {0xBE, 0xBA, 0xFE, 0xCA, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    Reassembler r;
    r.feed(buildFrame(kDevPlain, body));
    auto f = r.next();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->payload, body);
    EXPECT_FALSE(r.next().has_value());
}

TEST(As11Protocol, PairsWithSrp) {
    Srp s;
    ASSERT_TRUE(s.start(fixedA()));
    EXPECT_EQ(s.clientPublicHex(), kA);

    auto m1 = s.challenge(kB, kSalt, "4829");
    ASSERT_TRUE(m1.has_value());
    EXPECT_EQ(*m1, kM1);
    EXPECT_EQ(hexEncode(s.masterKey().data(), 32), kK);
    EXPECT_TRUE(s.verify(kM2));
    EXPECT_FALSE(s.verify(kK));

    // A wrong code yields another M1: the machine would refuse it.
    Srp w;
    ASSERT_TRUE(w.start(fixedA()));
    auto wrong = w.challenge(kB, kSalt, "4830");
    ASSERT_TRUE(wrong.has_value());
    EXPECT_NE(*wrong, kM1);

    // An invalid server (B = 0 mod N) and a malformed salt both refuse.
    Srp z;
    ASSERT_TRUE(z.start(fixedA()));
    EXPECT_FALSE(z.challenge("00", kSalt, "4829").has_value());
    EXPECT_FALSE(z.challenge(kB, "zz", "4829").has_value());
    EXPECT_FALSE(Srp{}.challenge(kB, kSalt, "4829").has_value());   // never started
}

TEST(As11Protocol, SessionCipher) {
    const Cipher c(key(kK), hex("2222222222222222222222222222222222222222222222222222222222222222"));
    EXPECT_EQ(hexEncode(c.key().data(), 32), kSession);

    const std::string json =
        "{\"id\":2,\"jsonrpc\":\"1.0\",\"method\":\"Get\",\"params\":[\"SerialNumber\"]}";
    std::array<uint8_t, 16> iv{};
    for (size_t i = 0; i < iv.size(); i++) iv[i] = static_cast<uint8_t>(i);
    const auto enc = c.encrypt(iv, json);
    EXPECT_EQ(enc, hex(kEnc));

    EXPECT_EQ(c.decrypt(enc), json);
    EXPECT_FALSE(c.decrypt(std::vector<uint8_t>(enc.begin(), enc.begin() + 17)).has_value());
    // A length that runs past the plaintext is malformed, not truncated.
    const Cipher other(key(kSession), {});
    EXPECT_FALSE(other.decrypt(enc).has_value());
}

TEST(As11Protocol, ChallengeResponse) {
    EXPECT_EQ(challengeResponse(key(kK),
                                "1111111111111111111111111111111111111111111111111111111111111111"),
              kHmac);
    EXPECT_FALSE(challengeResponse(key(kK), "xyz").has_value());
}

TEST(As11Protocol, Base64AndHex) {
    EXPECT_EQ(base64Decode("AAECAwQ="), (std::vector<uint8_t>{0, 1, 2, 3, 4}));
    EXPECT_EQ(base64Decode("SGVsbG8gd29ybGQ="), bytes("Hello world"));
    EXPECT_FALSE(base64Decode("!!").has_value());
    EXPECT_EQ(hexDecode("00ff1A"), (std::vector<uint8_t>{0x00, 0xFF, 0x1A}));
    EXPECT_FALSE(hexDecode("abc").has_value());
    EXPECT_EQ(hexEncode(std::vector<uint8_t>{0x0a, 0xbc}), "0ABC");
}
