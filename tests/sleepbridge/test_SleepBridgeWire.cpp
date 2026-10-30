// The Sleep Bridge local stream's bytes: frame header, CYS1/CYS2/CYR1, the
// card statement, sender names and the cursor rules.

#include "SleepBridgeTestUtil.h"

#include <gtest/gtest.h>

using namespace sbtest;

TEST(SleepBridgeWire, FrameHeaderRoundTripsLittleEndian) {
    FrameHeader h;
    h.req_id = 0x01020304;
    h.seq    = 0xA0B0C0D0;
    h.flags  = 1;
    h.kind   = 7;
    const std::string s = encodeFrameHeader(h);
    ASSERT_EQ(s.size(), kFrameHeaderSize);
    EXPECT_EQ(static_cast<uint8_t>(s[0]), 0x04);
    EXPECT_EQ(static_cast<uint8_t>(s[4]), 0xD0);
    EXPECT_EQ(static_cast<uint8_t>(s[10]), 7);
    const auto back = parseFrameHeader(s + "body");
    ASSERT_TRUE(back);
    EXPECT_EQ(back->req_id, h.req_id);
    EXPECT_EQ(back->seq, h.seq);
    EXPECT_EQ(back->flags, 1);
    EXPECT_EQ(back->kind, 7);
    EXPECT_FALSE(parseFrameHeader(std::string(11, '\0')));
}

TEST(SleepBridgeWire, ParsesAWriteBatch) {
    const std::string b = batch(41, {10, 11, 12});
    Batch out;
    ASSERT_EQ(parseBatch(u8(b), b.size(), out), BatchError::None);
    EXPECT_FALSE(out.reads);
    EXPECT_EQ(out.first_sequence, 41u);
    EXPECT_EQ(out.lastSequence(), 43u);
    ASSERT_EQ(out.records.size(), 3u);
    EXPECT_EQ(out.records[1].lba, 11u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(out.records[1].data), kSectorSize),
              sector(11, 42));
}

TEST(SleepBridgeWire, ParsesAReadBatch) {
    const std::string b = batch(5, {0, 1}, /*reads=*/true);
    Batch out;
    ASSERT_EQ(parseBatch(u8(b), b.size(), out), BatchError::None);
    EXPECT_TRUE(out.reads);
    EXPECT_EQ(out.records.size(), 2u);
}

TEST(SleepBridgeWire, RefusesBadBatches) {
    Batch out;
    std::string b = batch(1, {1, 2});
    EXPECT_EQ(parseBatch(u8(b), b.size() - 1, out), BatchError::LengthMismatch);
    EXPECT_EQ(parseBatch(u8(b), 10, out), BatchError::TooShort);
    std::string bad = b;
    bad[0] = 'X';
    EXPECT_EQ(parseBatch(u8(bad), bad.size(), out), BatchError::BadMagic);
    std::string many = header("CYS1", kMaxRecords + 1, 0);
    EXPECT_EQ(parseBatch(u8(many), many.size(), out), BatchError::TooManyRecords);
    std::string huge = header("CYS1", 2, static_cast<uint64_t>(INT64_MAX)) + std::string(2 * kRecordSize, 'a');
    EXPECT_EQ(parseBatch(u8(huge), huge.size(), out), BatchError::SequenceTooLarge);
    // A CYS2 is expanded first, never parsed as is.
    std::string fill = header("CYS2", 0, 0);
    EXPECT_EQ(parseBatch(u8(fill), fill.size(), out), BatchError::BadMagic);
}

TEST(SleepBridgeWire, HeartbeatAndCardStatement) {
    Batch out;
    const std::string hb = heartbeat();
    ASSERT_EQ(parseBatch(u8(hb), hb.size(), out), BatchError::None);
    EXPECT_TRUE(out.isHeartbeat());
    const std::string cs = cardStatement(kCard);
    ASSERT_EQ(parseBatch(u8(cs), cs.size(), out), BatchError::None);
    EXPECT_TRUE(out.isCard());
    EXPECT_EQ(out.card, kCard);
    // Trailing bytes that are not a JSON object are a truncated batch.
    const std::string junk = heartbeat() + "zz";
    EXPECT_EQ(parseBatch(u8(junk), junk.size(), out), BatchError::LengthMismatch);
}

TEST(SleepBridgeWire, Cys2ExpandsToTheCys1ItStandsFor) {
    std::string b = header("CYS2", 2, 9);
    put32(b, 300);
    b.push_back(static_cast<char>(kFillKindSector));
    b += sector(300, 9);
    put32(b, 301);
    b.push_back(static_cast<char>(kFillKindFill));
    b.push_back(static_cast<char>(0xE5));

    std::string x;
    ASSERT_EQ(expandFillBatch(u8(b), b.size(), x), BatchError::None);
    std::string want = header("CYS1", 2, 9);
    put32(want, 300);
    want += sector(300, 9);
    put32(want, 301);
    want += std::string(kSectorSize, static_cast<char>(0xE5));
    EXPECT_EQ(x, want);

    std::string out = "untouched";
    EXPECT_EQ(expandFillBatch(u8(b), b.size() - 1, out), BatchError::LengthMismatch);
    EXPECT_EQ(out, "untouched");
    std::string badkind = header("CYS2", 1, 0);
    put32(badkind, 1);
    badkind.push_back(9);
    badkind.push_back(0);
    EXPECT_EQ(expandFillBatch(u8(badkind), badkind.size(), out), BatchError::BadKind);
}

TEST(SleepBridgeWire, CardStatementShapes) {
    std::string why;
    const auto c = parseCardStatement(kCard, &why);
    ASSERT_TRUE(c) << why;
    EXPECT_EQ(c->cid, "0123456789abcdef0123456789abcdef");
    EXPECT_EQ(*c->mount, 3u);
    EXPECT_EQ(*c->seq, 0u);

    // A layout instead of a csd.
    EXPECT_TRUE(parseCardStatement(
        R"({"cid":"0123456789ABCDEF0123456789ABCDEF","sectors_per_cluster":64,"data_begin_lba":8192})",
        &why));
    // Neither: says nothing usable.
    EXPECT_FALSE(parseCardStatement(R"({"cid":"0123456789abcdef0123456789abcdef"})", &why));
    EXPECT_FALSE(parseCardStatement(R"({"cid":"short","csd":"x"})", &why));
    EXPECT_FALSE(parseCardStatement(
        R"({"cid":"0123456789abcdef0123456789abcdef","sectors_per_cluster":3,"data_begin_lba":1})", &why));
    EXPECT_FALSE(parseCardStatement("[1]", &why));
}

TEST(SleepBridgeWire, SenderNamesStayInsideTheFolder) {
    EXPECT_TRUE(validSender("TAP-S3-00000123"));
    EXPECT_TRUE(validSender("s3-a1b2c3d4e5f6"));
    EXPECT_FALSE(validSender(""));
    EXPECT_FALSE(validSender("."));
    EXPECT_FALSE(validSender(".."));
    EXPECT_FALSE(validSender("a/b"));
    EXPECT_FALSE(validSender(std::string(65, 'a')));
    EXPECT_TRUE(validMachine(""));
    EXPECT_FALSE(validMachine("../x"));
}

TEST(SleepBridgeWire, CursorRules) {
    // First batch: never a gap, however high it starts.
    auto d = decideCursor({}, 5000, 5009);
    EXPECT_FALSE(d.gap);
    EXPECT_EQ(d.accepted, 10);
    EXPECT_EQ(d.new_high_water, 5009);

    CursorState s{true, 100};
    d = decideCursor(s, 90, 100);   // wholly held: a re-send
    EXPECT_TRUE(d.duplicate);
    EXPECT_EQ(d.new_high_water, 100);

    d = decideCursor(s, 101, 110);  // in order
    EXPECT_FALSE(d.gap);
    EXPECT_EQ(d.accepted, 10);

    d = decideCursor(s, 95, 104);   // overlap: only the new part counts
    EXPECT_FALSE(d.gap);
    EXPECT_EQ(d.accepted, 4);
    EXPECT_EQ(d.new_high_water, 104);

    d = decideCursor(s, 120, 121);  // a gap, named
    EXPECT_TRUE(d.gap);
    EXPECT_EQ(d.gap_first, 101);
    EXPECT_EQ(d.gap_last, 119);
    EXPECT_EQ(d.new_high_water, 121);
}
