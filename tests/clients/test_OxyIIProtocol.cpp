#include <gtest/gtest.h>

#include "clients/OxyIIProtocol.h"

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
    std::vector<uint8_t> p(40, 0);
    p[5] = 1; p[6] = 96; p[7] = 10; p[8] = 62; p[13] = 80;
    auto l = parseLive(p);
    ASSERT_TRUE(l.has_value());
    EXPECT_TRUE(l->reading);
    EXPECT_EQ(l->spo2, 96);
    EXPECT_EQ(l->hr, 62);
    EXPECT_EQ(l->battery, 80);

    p[5] = 0;  // no finger
    EXPECT_FALSE(parseLive(p)->reading);
    p[5] = 1; p[8] = 0xFF;  // no-contact HR sentinel
    EXPECT_FALSE(parseLive(p)->reading);
    EXPECT_FALSE(parseLive(std::vector<uint8_t>(23, 0)).has_value());
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
