#include <gtest/gtest.h>

#include "clients/OxyIIProtocol.h"

#include <algorithm>
#include <cstring>

using namespace hms_cpap::oxyii;

// The O2Ring-S (OxyII) codec, no Bluetooth needed. The CRC fixture is the one
// the public protocol write-up publishes; the auth and frame vectors come from
// its reference implementation.

namespace {

std::vector<uint8_t> hex(const std::string& s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}

void putSlot(std::vector<uint8_t>& p, size_t i, const std::string& name) {
    std::memset(p.data() + 1 + i * 16, 0, 16);
    std::memcpy(p.data() + 1 + i * 16, name.data(), name.size());
}

} // namespace

TEST(OxyIIProtocol, CrcFixture) {
    const uint8_t get_info[] = {0xA5, 0xE1, 0x1E, 0x00, 0x02, 0x00, 0x00};
    EXPECT_EQ(crc8(get_info, sizeof(get_info)), 0xBF);
}

TEST(OxyIIProtocol, EncodesGetInfo) {
    EXPECT_EQ(encode(kOpGetInfo, 2), hex("a5e11e00020000bf"));
}

TEST(OxyIIProtocol, EncodesAuthFrame) {
    EXPECT_EQ(encode(kOpAuth, 0, authPayload(1791158400u)),
              hex("a5ff00000010000068158872091cb098c8c7da440315e3a2"));
}

TEST(OxyIIProtocol, TakesAFrameAfterNoise) {
    auto reply = hex("a5f30c01070a0000010203040506070809b5");  // F3 reply, 10 bytes
    std::vector<uint8_t> buf = {0x00, 0xA5, 0x11, 0x42};
    buf.insert(buf.end(), reply.begin(), reply.end());

    auto f = takeFrame(buf);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->op, kOpFileData);
    ASSERT_EQ(f->payload.size(), 10u);
    EXPECT_EQ(f->payload[9], 0x09);
    EXPECT_TRUE(buf.empty());
}

TEST(OxyIIProtocol, WaitsForTheRestOfASplitFrame) {
    auto reply = hex("a5f30c01070a0000010203040506070809b5");
    std::vector<uint8_t> buf(reply.begin(), reply.begin() + 9);
    EXPECT_FALSE(takeFrame(buf).has_value());
    EXPECT_EQ(buf.size(), 9u);  // kept for the next notification

    buf.insert(buf.end(), reply.begin() + 9, reply.end());
    auto f = takeFrame(buf);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->payload.size(), 10u);
}

TEST(OxyIIProtocol, RefusesABadCrcAndAHugeLength) {
    auto bad = hex("a5f30c01070a0000010203040506070809b5");
    bad.back() ^= 0xFF;
    EXPECT_FALSE(takeFrame(bad).has_value());
    EXPECT_TRUE(bad.empty());

    std::vector<uint8_t> huge = {0xA5, 0xF3, 0x0C, 0x01, 0x00, 0xFF, 0xFF};
    EXPECT_FALSE(takeFrame(huge).has_value());
    EXPECT_TRUE(huge.empty());
}

TEST(OxyIIProtocol, TimeAndEpoch) {
    DateTime t{2026, 10, 5, 23, 4, 59};
    EXPECT_EQ(timePayload(t), (std::vector<uint8_t>{0xEA, 0x07, 10, 5, 23, 4, 59, 0x00}));
    EXPECT_EQ(epochOf(DateTime{2026, 10, 5, 0, 0, 0}), 1791158400u);
    EXPECT_EQ(epochOf(DateTime{2028, 2, 29, 12, 0, 0}), 1835438400u);
}

TEST(OxyIIProtocol, ParsesInfo) {
    std::vector<uint8_t> p(60, 0);
    std::memcpy(p.data() + 9, "2D010002", 8);
    p[24] = 0xEA; p[25] = 0x07; p[26] = 10; p[27] = 5; p[28] = 22; p[29] = 30; p[30] = 1;
    p[37] = 10;
    std::memcpy(p.data() + 38, "25B2303210", 10);

    auto info = parseInfo(p);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->firmware, "2D010002");
    EXPECT_EQ(info->serial, "25B2303210");
    EXPECT_TRUE(info->clock_valid);
    EXPECT_EQ(info->clock.hour, 22);

    p[24] = 0; p[25] = 0;  // a clock never set
    EXPECT_FALSE(parseInfo(p)->clock_valid);
    EXPECT_FALSE(parseInfo(std::vector<uint8_t>(30, 0)).has_value());
}

TEST(OxyIIProtocol, ParsesLive) {
    // The vendor SDK's RtParam: [4] run, [5] sensor, [6] SpO2, [7] PI x10,
    // [8..9] PR u16 LE, [11] motion, [12] battery state, [13] battery %.
    std::vector<uint8_t> p(40, 0);
    p[4] = 2; p[5] = 1; p[6] = 96; p[7] = 13; p[8] = 62; p[11] = 4; p[12] = 0; p[13] = 80;
    auto l = parseLive(p);
    ASSERT_TRUE(l.has_value());
    EXPECT_TRUE(l->reading);
    EXPECT_EQ(l->spo2, 96);
    EXPECT_EQ(l->hr, 62);
    EXPECT_EQ(l->pi_x10, 13);   // [7] is perfusion, not motion
    EXPECT_EQ(l->motion, 4);
    EXPECT_EQ(l->battery, 80);
    EXPECT_EQ(l->run, 2);

    p[5] = 0;  // no finger
    EXPECT_FALSE(parseLive(p)->reading);
    p[5] = 2;  // probe off is not a finger either
    EXPECT_FALSE(parseLive(p)->reading);
    p[5] = 1; p[8] = 0xFF;  // PR "no value"
    EXPECT_FALSE(parseLive(p)->reading);
    p[8] = 0xFF; p[9] = 0x01;  // 511, the other "no value"; [9] is the high byte
    EXPECT_FALSE(parseLive(p)->reading);
    EXPECT_EQ(parseLive(p)->hr, 0xFF);
    p[8] = 62; p[9] = 0; p[6] = 127;  // SpO2 "no value"
    EXPECT_FALSE(parseLive(p)->reading);
    p[6] = 96;
    EXPECT_TRUE(parseLive(p)->reading);

    EXPECT_TRUE(parseLive(std::vector<uint8_t>(p.begin(), p.begin() + 14)).has_value());
    EXPECT_FALSE(parseLive(std::vector<uint8_t>(13, 0)).has_value());
}

// The AES vectors come from `openssl enc -aes-128-ecb` (PKCS7 by default), so
// the padding is checked against an implementation other than ours.
TEST(OxyIIProtocol, EncryptsAndDecryptsTheSession) {
    Key key;
    for (size_t i = 0; i < key.size(); i++) key[i] = static_cast<uint8_t>(i);

    // A 20-byte READ_FILE_START payload: two blocks, 12 bytes of 0x0c pad.
    auto start = fileStartPayload("20261006013000.o2s");
    ASSERT_TRUE(start.has_value());
    const auto want = hex("1b10db80b5762ce93d102cd76b484e0ed2591ea1e1a2a00b753e691052e40191");
    EXPECT_EQ(encrypt(key, *start), want);
    EXPECT_EQ(decrypt(key, want), *start);

    // One byte (setup's 00): one block.
    EXPECT_EQ(encrypt(key, {0x00}), hex("4c4d10e1f5542fef3e2da31ff4b4471a"));

    // A whole block takes a whole block of padding.
    const std::vector<uint8_t> block(reinterpret_cast<const uint8_t*>("0123456789abcdef"),
                                     reinterpret_cast<const uint8_t*>("0123456789abcdef") + 16);
    const auto two = hex("281567ab2f4cf0d73d3198225b8b8393954f64f2e4e86e9eee82d20216684899");
    EXPECT_EQ(encrypt(key, block), two);
    EXPECT_EQ(decrypt(key, two), block);

    // A partial block and a wrong key both refuse.
    EXPECT_FALSE(decrypt(key, std::vector<uint8_t>(want.begin(), want.begin() + 20)).has_value());
    EXPECT_FALSE(decrypt(Key{}, want).has_value());
}

TEST(OxyIIProtocol, TakesTheSessionKeyFromTheAuthReply) {
    const uint8_t lepu[16] = {0xc2, 0xa7, 0xcf, 0x50, 0xda, 0xfe, 0xd8, 0x85,
                              0xa8, 0xf8, 0xf7, 0xea, 0xc4, 0x43, 0x35, 0xf3};
    std::vector<uint8_t> plain = {0x01, 0x10, 0x00, 0x00};
    for (int i = 0; i < 16; i++) plain.push_back(static_cast<uint8_t>(0xA0 + i));
    std::vector<uint8_t> blob(plain.size());
    for (size_t i = 0; i < blob.size(); i++) blob[i] = plain[i] ^ lepu[i % 16];

    auto key = parseAuthReply(blob);
    ASSERT_TRUE(key.has_value());
    EXPECT_TRUE(std::equal(key->begin(), key->end(), plain.begin() + 4));

    // The 16-byte reply some rings send before working in plaintext.
    EXPECT_FALSE(parseAuthReply(std::vector<uint8_t>(blob.begin(), blob.begin() + 16)).has_value());
    blob[0] ^= 0x02;  // type 3: not AES
    EXPECT_FALSE(parseAuthReply(blob).has_value());
    blob[0] ^= 0x02; blob[1] ^= 0x18;  // key length 8
    EXPECT_FALSE(parseAuthReply(blob).has_value());
}

TEST(OxyIIProtocol, KnowsWhichRepliesAreEncrypted) {
    for (uint8_t op : {kOpLive, kOpGetInfo, kOpGetBattery, kOpFileList, kOpFileStart,
                       kOpFileData, kOpGetConfig})
        EXPECT_TRUE(replyEncrypted(op)) << int(op);
    for (uint8_t op : {kOpAuth, kOpSetup, kOpSetTime, kOpFileEnd})
        EXPECT_FALSE(replyEncrypted(op)) << int(op);
}

TEST(OxyIIProtocol, ParsesFileList) {
    std::vector<uint8_t> p(1 + 4 * 16, 0);
    p[0] = 4;
    putSlot(p, 0, "20261001220000");
    putSlot(p, 1, "garbage-name!!");
    putSlot(p, 2, "20261002220000");
    putSlot(p, 3, "20261003220000");

    auto names = parseFileList(p);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "20261001220000.o2s");
    EXPECT_EQ(names[2], "20261003220000.o2s");

    // A count larger than the slots sent reads only what arrived.
    p.resize(1 + 16);
    EXPECT_EQ(parseFileList(p).size(), 1u);
    EXPECT_TRUE(parseFileList({0}).empty());
}

TEST(OxyIIProtocol, FileStartAndDataPayloads) {
    auto p = fileStartPayload("20261005220000.o2s");
    ASSERT_TRUE(p.has_value());
    ASSERT_EQ(p->size(), 20u);
    EXPECT_EQ(std::string(p->begin(), p->begin() + 14), "20261005220000");
    for (size_t i = 14; i < 20; i++) EXPECT_EQ((*p)[i], 0);

    EXPECT_TRUE(fileStartPayload("20261005220000").has_value());
    EXPECT_FALSE(fileStartPayload("20261005220000.vld").has_value());
    EXPECT_FALSE(fileStartPayload("2026100522000x.o2s").has_value());

    EXPECT_EQ(fileDataPayload(0x00012345), (std::vector<uint8_t>{0x45, 0x23, 0x01, 0x00}));
}

TEST(OxyIIProtocol, MatchesItsNames) {
    EXPECT_TRUE(nameMatches("S8-AW 1A2B"));
    EXPECT_TRUE(nameMatches("T8520_e85a"));
    EXPECT_FALSE(nameMatches("O2Ring 1276"));
    EXPECT_FALSE(nameMatches("S8"));
}
