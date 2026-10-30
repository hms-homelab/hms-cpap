// The bank: a batch on disk before its ack, the cursor after it, a re-send
// stored once, a crash between the batch's fsync and the ack resent and
// stored once.

#include "SleepBridgeTestUtil.h"

#include "sleepbridge/BatchStore.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace sbtest;

namespace {
const std::string kSender  = "TAP-S3-00000123";
const std::string kMachine = "airsense-bench";
}  // namespace

TEST(SleepBridgeStore, BatchIsOnDiskAndCursorSavedBeforeTheAck) {
    TempDir t;
    BatchStore store(t.path.string());
    const std::string b = run(0, 4);
    const Answer a = store.handle(kSender, kMachine, u8(b), b.size());
    ASSERT_EQ(a.status, 200);
    EXPECT_TRUE(a.body["ok"].asBool());
    EXPECT_EQ(a.body["ack"].asInt64(), 3);
    EXPECT_EQ(a.body["accepted"].asInt64(), 4);
    EXPECT_FALSE(a.body["gap"].asBool());
    EXPECT_FALSE(a.body["duplicate"].asBool());

    // By the time the answer exists, both are on disk.
    const fs::path dir = store.batchesDir(kSender);
    EXPECT_EQ(slurp(dir / "0-3.cys"), b);
    EXPECT_NE(slurp(dir / "cursor.json").find("\"high_water\":3"), std::string::npos);
    // And no temporary file is left behind.
    for (const auto& e : fs::directory_iterator(dir))
        EXPECT_NE(e.path().filename().string().rfind(".", 0), 0u) << e.path();
}

TEST(SleepBridgeStore, ARedeliveredBatchIsAckedAndNotStoredAgain) {
    TempDir t;
    BatchStore store(t.path.string());
    const std::string b = run(0, 4);
    ASSERT_EQ(store.handle(kSender, kMachine, u8(b), b.size()).status, 200);
    const Answer again = store.handle(kSender, kMachine, u8(b), b.size());
    EXPECT_EQ(again.status, 200);
    EXPECT_TRUE(again.body["duplicate"].asBool());
    EXPECT_EQ(again.body["ack"].asInt64(), 3);
    EXPECT_EQ(filesIn(store.batchesDir(kSender), ".cys").size(), 1u);

    // A resend cut differently, overlapping the mark: stored, new part counted.
    const std::string over = run(2, 4);   // 2..5
    const Answer o = store.handle(kSender, kMachine, u8(over), over.size());
    EXPECT_EQ(o.status, 200);
    EXPECT_EQ(o.body["accepted"].asInt64(), 2);
    EXPECT_EQ(o.body["ack"].asInt64(), 5);
}

TEST(SleepBridgeStore, ACrashBetweenFsyncAndAckIsResentAndStoredOnce) {
    TempDir t;
    {
        BatchStore store(t.path.string());
        const std::string first = run(0, 2);
        ASSERT_EQ(store.handle(kSender, kMachine, u8(first), first.size()).status, 200);
        store.failNextCursorSave();
        const std::string b = run(2, 3);   // 2..4
        const Answer a = store.handle(kSender, kMachine, u8(b), b.size());
        EXPECT_EQ(a.status, 500);
        EXPECT_EQ(a.body["ack"].asInt64(), 1);   // the old mark, never the new one
        EXPECT_TRUE(fs::exists(fs::path(store.batchesDir(kSender)) / "2-4.cys"));
    }
    // The process comes back; the bridge resends from its last ack.
    BatchStore store(t.path.string());
    const std::string b = run(2, 3);
    const Answer a = store.handle(kSender, kMachine, u8(b), b.size());
    EXPECT_EQ(a.status, 200);
    EXPECT_FALSE(a.body["duplicate"].asBool());
    EXPECT_EQ(a.body["ack"].asInt64(), 4);
    const auto files = filesIn(store.batchesDir(kSender), ".cys");
    EXPECT_EQ(files.size(), 2u);   // 0-1 and 2-4, the latter once
    EXPECT_EQ(slurp(fs::path(store.batchesDir(kSender)) / "2-4.cys"), b);
}

TEST(SleepBridgeStore, TheMarkSurvivesARestart) {
    TempDir t;
    {
        BatchStore store(t.path.string());
        const std::string b = run(10, 5);
        ASSERT_EQ(store.handle(kSender, kMachine, u8(b), b.size()).status, 200);
    }
    BatchStore store(t.path.string());
    const auto c = store.cursorOf(kSender);
    ASSERT_TRUE(c);
    EXPECT_TRUE(c->exists);
    EXPECT_EQ(c->high_water, 14);
    const std::string hb = heartbeat();
    EXPECT_EQ(store.handle(kSender, kMachine, u8(hb), hb.size()).body["ack"].asInt64(), 14);
}

TEST(SleepBridgeStore, AGapIsStoredAndRecorded) {
    TempDir t;
    BatchStore store(t.path.string());
    const std::string a = run(0, 2);
    const std::string b = run(10, 2);
    store.handle(kSender, kMachine, u8(a), a.size());
    Stored st;
    const Answer r = store.handle(kSender, kMachine, u8(b), b.size(), &st);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body["gap"].asBool());
    EXPECT_TRUE(st.gap);
    const std::string gaps = slurp(fs::path(store.batchesDir(kSender)) / "gaps.jsonl");
    EXPECT_NE(gaps.find("\"first\":2"), std::string::npos);
    EXPECT_NE(gaps.find("\"last\":9"), std::string::npos);
}

TEST(SleepBridgeStore, ACys2IsStoredAsTheCys1ItStandsFor) {
    TempDir t;
    BatchStore store(t.path.string());
    std::string b = header("CYS2", 1, 0);
    put32(b, 50);
    b.push_back(static_cast<char>(kFillKindFill));
    b.push_back(0);
    ASSERT_EQ(store.handle(kSender, kMachine, u8(b), b.size()).status, 200);
    const std::string on_disk = slurp(fs::path(store.batchesDir(kSender)) / "0-0.cys");
    ASSERT_EQ(on_disk.size(), kBatchHeaderSize + kRecordSize);
    EXPECT_EQ(on_disk.substr(0, 4), "CYS1");
}

TEST(SleepBridgeStore, HeartbeatAndCardStatementMoveNoMark) {
    TempDir t;
    BatchStore store(t.path.string());
    const std::string b = run(0, 3);
    store.handle(kSender, kMachine, u8(b), b.size());

    const std::string hb = heartbeat();
    const Answer h = store.handle(kSender, kMachine, u8(hb), hb.size());
    EXPECT_EQ(h.status, 200);
    EXPECT_TRUE(h.body["heartbeat"].asBool());
    EXPECT_EQ(h.body["ack"].asInt64(), 2);

    const std::string cs = cardStatement(kCard);
    Stored st;
    const Answer c = store.handle(kSender, kMachine, u8(cs), cs.size(), &st);
    EXPECT_EQ(c.status, 200);
    EXPECT_TRUE(c.body["card"].asBool());
    EXPECT_EQ(c.body["ack"].asInt64(), 2);
    EXPECT_TRUE(st.card);
    const auto cards = filesIn(fs::path(store.batchesDir(kSender)) / "cards", ".json");
    ASSERT_EQ(cards.size(), 1u);
    EXPECT_EQ(cards[0].rfind("3-", 0), 0u);   // before the next record, sequence 3
    EXPECT_EQ(filesIn(store.batchesDir(kSender), ".cys").size(), 1u);

    const std::string bad = cardStatement(R"({"cid":"nope"})");
    const Answer r = store.handle(kSender, kMachine, u8(bad), bad.size());
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body["ack"].asInt64(), 2);
}

TEST(SleepBridgeStore, RefusalsCarryTheMark) {
    TempDir t;
    BatchStore store(t.path.string());
    const std::string b = run(0, 3);
    store.handle(kSender, kMachine, u8(b), b.size());
    std::string trunc = run(3, 2);
    trunc.pop_back();
    const Answer r = store.handle(kSender, kMachine, u8(trunc), trunc.size());
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.body["ack"].asInt64(), 2);
    EXPECT_EQ(store.handle("../evil", kMachine, u8(b), b.size()).status, 400);
    EXPECT_EQ(store.handle(kSender, "bad/machine", u8(b), b.size()).status, 400);
    EXPECT_FALSE(fs::exists(t.path.parent_path() / "evil"));
}

TEST(SleepBridgeStore, SendersAreListedFromDisk) {
    TempDir t;
    BatchStore store(t.path.string());
    const std::string b = run(0, 1);
    store.handle("bridge-a", "", u8(b), b.size());
    store.handle("bridge-b", "", u8(b), b.size());
    auto s = store.senders();
    std::sort(s.begin(), s.end());
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0], "bridge-a");
    EXPECT_EQ(store.cardRoot("bridge-a"), (t.path / "bridge-a").string());
}
