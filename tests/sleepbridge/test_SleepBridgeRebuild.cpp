// The rebuild seam: banked batches replayed once each, in sequence order, into
// the rebuild; what a look returns written into a card root LocalDataSource
// reads; held files listed, never written; nothing written while holding;
// the rebuild's state kept across a restart.

#include "SleepBridgeTestUtil.h"

#include "clients/LocalDataSource.h"
#include "sleepbridge/Conversation.h"
#include "sleepbridge/RebuildWorker.h"
#include "sleepbridge/SleepBridgeService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

using namespace sbtest;

namespace {

const std::string kSender = "TAP-S3-00000123";

void bank(BatchStore& s, const std::string& body) {
    ASSERT_EQ(s.handle(kSender, "airsense-bench", u8(body), body.size()).status, 200);
}

}  // namespace

TEST(SleepBridgeRebuild, SafeCardPaths) {
    EXPECT_TRUE(safeCardPath("STR.edf"));
    EXPECT_TRUE(safeCardPath("DATALOG/20261030/20261030_221500_BRP.edf"));
    EXPECT_FALSE(safeCardPath(""));
    EXPECT_FALSE(safeCardPath("/etc/passwd"));
    EXPECT_FALSE(safeCardPath("../x"));
    EXPECT_FALSE(safeCardPath("DATALOG/../../x"));
    EXPECT_FALSE(safeCardPath("DATALOG//x"));
    EXPECT_FALSE(safeCardPath("DATALOG/./x"));
    EXPECT_FALSE(safeCardPath("C:/x"));
    EXPECT_FALSE(safeCardPath("DATALOG\\x"));
    EXPECT_FALSE(safeCardPath("batches/cursor.json"));
    EXPECT_FALSE(safeCardPath("BATCHES/x"));
    EXPECT_FALSE(safeCardPath("DATALOG/"));
    EXPECT_FALSE(safeCardPath(std::string(256, 'a')));
}

TEST(SleepBridgeRebuild, HoldingUntilTheMountThenWritten) {
    TempDir t;
    BatchStore store(t.path.string());
    auto log = std::make_shared<FakeLog>();
    RebuildWorker w(store, kSender, fakeFactory(log));

    bank(store, run(0, 2, 100));
    auto r = w.step();
    EXPECT_EQ(r.records_fed, 2);
    EXPECT_TRUE(r.holding);
    EXPECT_EQ(r.holding_reason, "no_mount_since_boot");
    EXPECT_EQ(r.held_records, 2u);
    EXPECT_EQ(r.written, 0);
    EXPECT_FALSE(fs::exists(t.path / kSender / "DATALOG"));

    // The machine mounts the card: the boot sector read releases the hold.
    bank(store, batch(2, {0, 1}, /*reads=*/true));
    r = w.step();
    EXPECT_EQ(log->reads, 2);
    EXPECT_FALSE(r.holding);
    EXPECT_EQ(r.written, 2);
    EXPECT_EQ(slurp(t.path / kSender / "DATALOG/20261030/100.edf"), sector(100, 0));
    EXPECT_EQ(slurp(t.path / kSender / "DATALOG/20261030/101.edf"), sector(101, 1));
}

TEST(SleepBridgeRebuild, ACardStatementGoesInAtItsPlace) {
    TempDir t;
    BatchStore store(t.path.string());
    auto log = std::make_shared<FakeLog>();
    RebuildWorker w(store, kSender, fakeFactory(log));
    bank(store, cardStatement(kCard));   // before anything: before sequence 0
    bank(store, run(0, 2, 100));
    bank(store, cardStatement(kCard));   // after 0..1: before sequence 2
    bank(store, run(2, 1, 102));
    const auto r = w.step();
    EXPECT_EQ(r.cards_fed, 2);
    EXPECT_EQ(r.written, 3);
    EXPECT_EQ(log->seqs, (std::vector<uint64_t>{0, 1, 2}));
    const auto cards = filesIn(fs::path(store.batchesDir(kSender)) / "cards", ".json");
    ASSERT_EQ(cards.size(), 2u);
}

TEST(SleepBridgeRebuild, EveryRecordIsFedOnceAcrossOverlapsAndSteps) {
    TempDir t;
    BatchStore store(t.path.string());
    auto log = std::make_shared<FakeLog>();
    RebuildWorker w(store, kSender, fakeFactory(log));
    bank(store, cardStatement(kCard));
    bank(store, run(0, 4, 100));    // 0..3
    w.step();
    bank(store, run(2, 4, 102));    // 2..5, a resend cut differently
    bank(store, run(0, 4, 100));    // a duplicate: not stored, nothing to feed
    w.step();
    EXPECT_FALSE(w.step().looked);  // nothing new: no look
    EXPECT_EQ(log->seqs, (std::vector<uint64_t>{0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(*w.fedThrough(), 5);
}

TEST(SleepBridgeRebuild, OnlyWhatTheLookReturnsIsWritten) {
    TempDir t;
    BatchStore store(t.path.string());
    RebuildWorker w(store, kSender, fakeFactory(std::make_shared<FakeLog>()));
    bank(store, cardStatement(kCard));
    bank(store, run(0, 1, 100));
    EXPECT_EQ(w.step().written, 1);
    bank(store, run(1, 1, 200));
    auto r = w.step();
    EXPECT_EQ(r.written, 1);          // 200.edf; 100.edf unchanged, not returned
    EXPECT_EQ(r.complete_files, 2u);
    bank(store, batch(2, {100}));     // 100 written again: last write wins
    EXPECT_EQ(w.step().written, 1);
    EXPECT_EQ(slurp(t.path / kSender / "DATALOG/20261030/100.edf"), sector(100, 2));
}

TEST(SleepBridgeRebuild, HeldFilesAreListedNeverWritten) {
    TempDir t;
    BatchStore store(t.path.string());
    RebuildWorker w(store, kSender, fakeFactory(std::make_shared<FakeLog>()));
    bank(store, cardStatement(kCard));
    bank(store, batch(0, {7}));
    auto r = w.step();
    EXPECT_EQ(r.held_files, 1u);
    EXPECT_FALSE(fs::exists(t.path / kSender / "STR.edf"));
    const std::string held = slurp(t.path / kSender / "batches/held.json");
    EXPECT_NE(held.find("STR.edf"), std::string::npos);
    EXPECT_NE(held.find("\"missing_sectors\" : 1"), std::string::npos);

    bank(store, batch(1, {8}));
    r = w.step();
    EXPECT_EQ(r.held_files, 0u);
    EXPECT_EQ(slurp(t.path / kSender / "STR.edf"), sector(7, 0) + sector(8, 1));
    EXPECT_EQ(slurp(t.path / kSender / "batches/held.json").find("STR.edf"), std::string::npos);
}

TEST(SleepBridgeRebuild, CutPiecesReplaceTheFileTheyWereCutFrom) {
    TempDir t;
    BatchStore store(t.path.string());
    RebuildWorker w(store, kSender, fakeFactory(std::make_shared<FakeLog>()));
    bank(store, cardStatement(kCard));
    bank(store, batch(0, {500}));
    w.step();
    const fs::path whole = t.path / kSender / "DATALOG/20261030/500.edf";
    ASSERT_TRUE(fs::exists(whole));
    bank(store, batch(1, {501}));
    const auto r = w.step();
    EXPECT_EQ(r.removed, 1);
    EXPECT_FALSE(fs::exists(whole));
    EXPECT_EQ(slurp(t.path / kSender / "DATALOG/20261030/500-a.edf") +
                  slurp(t.path / kSender / "DATALOG/20261030/500-b.edf"),
              sector(500, 0));
}

TEST(SleepBridgeRebuild, APathOutsideTheCardRootIsRefused) {
    TempDir t;
    BatchStore store(t.path.string());
    RebuildWorker w(store, kSender,
                    fakeFactory(std::make_shared<FakeLog>(), {"../escape.edf", "batches/cursor.json"}));
    bank(store, cardStatement(kCard));
    bank(store, run(0, 1));
    const auto r = w.step();
    EXPECT_EQ(r.refused_paths, 2);
    EXPECT_FALSE(fs::exists(t.path / "escape.edf"));
    EXPECT_NE(slurp(t.path / kSender / "batches/cursor.json").find("high_water"), std::string::npos);
}

TEST(SleepBridgeRebuild, TheGeometryIsKeptDurablyOnceEstablishedAndARestartStartsFromIt) {
    TempDir t;
    const fs::path file = t.path / kSender / "batches" / kGeometryFile;
    {
        BatchStore store(t.path.string());
        auto log = std::make_shared<FakeLog>();
        RebuildWorker w(store, kSender, fakeFactory(log));
        ASSERT_EQ(log->starts.size(), 1u);
        EXPECT_FALSE(log->starts[0].geometry.valid()) << "nothing kept yet";
        EXPECT_TRUE(log->starts[0].legacy_state.empty());
        bank(store, run(0, 1, 100));
        auto r = w.step();
        EXPECT_TRUE(r.holding);
        EXPECT_FALSE(fs::exists(file)) << "no geometry while holding";
        EXPECT_FALSE(r.geometry.valid());

        bank(store, batch(1, {0}, /*reads=*/true));   // the mount: the boot sector
        bank(store, run(2, 2, 101));
        r = w.step();
        EXPECT_EQ(r.written, 3);
        EXPECT_EQ(r.geometry, fakeGeometry("read"));
        EXPECT_EQ(w.geometry(), fakeGeometry("read"));
        const auto kept = parseGeometryJson(slurp(file));
        ASSERT_TRUE(kept.has_value());
        EXPECT_EQ(*kept, fakeGeometry("read"));
        EXPECT_FALSE(fs::exists(t.path / kSender / "batches/rebuild.state"));

        // Unchanged: not rewritten.
        const auto when = fs::last_write_time(file);
        bank(store, run(4, 1, 103));
        w.step();
        EXPECT_EQ(fs::last_write_time(file), when);
    }
    // A restart: built from the file, before any batch; then the whole bank
    // again from the start, never holding.
    BatchStore store(t.path.string());
    auto log = std::make_shared<FakeLog>();
    RebuildWorker w(store, kSender, fakeFactory(log));
    ASSERT_EQ(log->starts.size(), 1u);
    EXPECT_EQ(log->starts[0].geometry, fakeGeometry("read"));
    EXPECT_TRUE(log->seqs.empty()) << "the geometry comes before the first record";
    const auto r = w.step();
    EXPECT_FALSE(r.holding);
    EXPECT_EQ(r.written, 4) << "the same files again";
    EXPECT_EQ(log->seqs, (std::vector<uint64_t>{0, 1, 2, 3, 4}));   // fed again from the start
}

TEST(SleepBridgeRebuild, TheGeometryFileAndAnEmptyBankPlaceARecord) {
    TempDir t;
    BatchStore store(t.path.string());
    const fs::path bdir = store.batchesDir(kSender);
    fs::create_directories(bdir);
    const std::string text = geometryJson(fakeGeometry("read"));
    {
        std::ofstream f(bdir / kGeometryFile, std::ios::binary);
        f << text;
    }
    auto log = std::make_shared<FakeLog>();
    RebuildWorker w(store, kSender, fakeFactory(log));
    ASSERT_EQ(log->starts.size(), 1u);
    EXPECT_EQ(log->starts[0].geometry, fakeGeometry("read"));
    EXPECT_FALSE(w.step().looked) << "an empty bank: nothing to feed";

    // A write with no mount and no statement in the bank: placed at once.
    bank(store, run(0, 1, 100));
    const auto r = w.step();
    EXPECT_FALSE(r.holding);
    EXPECT_EQ(r.written, 1);
    EXPECT_EQ(slurp(t.path / kSender / "DATALOG/20261030/100.edf"), sector(100, 0));
    EXPECT_EQ(slurp(bdir / kGeometryFile), text) << "the same geometry: the file stands";

    // Without the file the same bank holds until the bank proves the layout.
    fs::remove(bdir / kGeometryFile);
    auto log2 = std::make_shared<FakeLog>();
    RebuildWorker w2(store, kSender, fakeFactory(log2));
    EXPECT_FALSE(log2->starts[0].geometry.valid());
    EXPECT_TRUE(w2.step().holding);
}

TEST(SleepBridgeRebuild, ABootSectorThatContradictsTheGeometryFileRewritesIt) {
    TempDir t;
    BatchStore store(t.path.string());
    const fs::path bdir = store.batchesDir(kSender);
    fs::create_directories(bdir);
    RebuildGeometry stated = fakeGeometry("stated");
    stated.sectors_per_cluster = 16;
    stated.data_begin_lba      = 2000;
    {
        std::ofstream f(bdir / kGeometryFile, std::ios::binary);
        f << geometryJson(stated);
    }
    auto log = std::make_shared<FakeLog>();
    RebuildWorker w(store, kSender, fakeFactory(log));
    EXPECT_EQ(log->starts[0].geometry, stated);
    bank(store, run(0, 1, 100));
    auto r = w.step();
    EXPECT_FALSE(r.holding);
    EXPECT_EQ(r.geometry, stated);

    bank(store, batch(1, {0}, /*reads=*/true));   // the card's own boot sector
    bank(store, run(2, 1, 101));
    r = w.step();
    EXPECT_EQ(r.geometry, fakeGeometry("read"));
    const auto kept = parseGeometryJson(slurp(bdir / kGeometryFile));
    ASSERT_TRUE(kept.has_value());
    EXPECT_EQ(*kept, fakeGeometry("read")) << "the boot sector wins and the file says so";
    EXPECT_EQ(w.geometry(), fakeGeometry("read"));
}

TEST(SleepBridgeRebuild, AnOlderBuildsStateFileIsReadOnceThenReplacedByTheGeometryFile) {
    TempDir t;
    BatchStore store(t.path.string());
    const fs::path bdir = store.batchesDir(kSender);
    fs::create_directories(bdir);
    {
        std::ofstream f(bdir / "rebuild.state", std::ios::binary);
        f << "trusted";
    }
    auto log = std::make_shared<FakeLog>();
    RebuildWorker w(store, kSender, fakeFactory(log));
    ASSERT_EQ(log->starts.size(), 1u);
    EXPECT_FALSE(log->starts[0].geometry.valid());
    EXPECT_EQ(log->starts[0].legacy_state, "trusted");
    bank(store, run(0, 1, 100));
    const auto r = w.step();
    EXPECT_FALSE(r.holding);
    EXPECT_EQ(r.written, 1);
    EXPECT_TRUE(fs::exists(bdir / kGeometryFile));
    EXPECT_FALSE(fs::exists(bdir / "rebuild.state")) << "replaced by the geometry file";
}

TEST(SleepBridgeRebuild, ADamagedGeometryFileIsIgnoredNotTrusted) {
    EXPECT_EQ(parseGeometryJson(geometryJson(fakeGeometry("read"))), fakeGeometry("read"));
    EXPECT_EQ(parseGeometryJson(geometryJson(fakeGeometry("stated"))), fakeGeometry("stated"));
    EXPECT_FALSE(parseGeometryJson("").has_value());
    EXPECT_FALSE(parseGeometryJson("not json").has_value());
    EXPECT_FALSE(parseGeometryJson("[]").has_value());
    EXPECT_FALSE(parseGeometryJson(R"({"sectors_per_cluster":8})").has_value()) << "fields missing";
    RebuildGeometry bad = fakeGeometry("read");
    bad.sectors_per_cluster = 12;   // not a power of two
    EXPECT_FALSE(parseGeometryJson(geometryJson(bad)).has_value());
    bad = fakeGeometry("read");
    bad.data_begin_lba = 0;
    EXPECT_FALSE(parseGeometryJson(geometryJson(bad)).has_value());

    TempDir t;
    BatchStore store(t.path.string());
    const fs::path bdir = store.batchesDir(kSender);
    fs::create_directories(bdir);
    {
        std::ofstream f(bdir / kGeometryFile, std::ios::binary);
        f << "{\"sectors_per_cluster\": \"eight\"}";
    }
    auto log = std::make_shared<FakeLog>();
    RebuildWorker w(store, kSender, fakeFactory(log));
    EXPECT_FALSE(log->starts[0].geometry.valid());
    bank(store, run(0, 1, 100));
    EXPECT_TRUE(w.step().holding) << "the bank establishes it again";
}

TEST(SleepBridgeRebuild, WithoutTheComponentTheServiceIsUnavailable) {
    TempDir t;
    SleepBridgeService none({t.path.string(), kDefaultPort, 0.01}, nullptr);
    EXPECT_FALSE(none.available());
    EXPECT_FALSE(none.status()["available"].asBool());
    EXPECT_EQ(none.status()["message"].asString(), "The Sleep Bridge is not in this build");
    SleepBridgeService empty({t.path.string(), kDefaultPort, 0.01},
                             [](const RebuildStart&) { return std::unique_ptr<IRebuilder>(); });
    EXPECT_FALSE(empty.available());
    // This build carries the component only when it was linked in.
    EXPECT_EQ(rebuilderInBuild(), makeRebuilder({}, nullptr) != nullptr);
    EXPECT_EQ(rebuilderInBuild(), !rebuilderVersion().empty());
}

// A recorded night's shape, end to end through the conversation: the hello,
// the card statement at a mount, the mount's reads, writes in CYS1 and CYS2,
// a connection drop with a resend cut differently, heartbeats. The rebuilt
// card root is then read as a local card folder.
TEST(SleepBridgeRebuild, AReplayedSessionLandsInACardRootLocalDataSourceReads) {
    TempDir t;
    auto log = std::make_shared<FakeLog>();
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01}, fakeFactory(log));
    ASSERT_TRUE(svc.available());
    svc.start();

    Admission who{kSender, "airsense-bench", "A1B2C3D4E5F6"};
    auto c = svc.open(who);
    auto send = [&](const Effects& e) {
        svc.apply(kSender, e);
        return e;
    };
    auto ackOf = [](const Effects& e) {
        Json::Value v;
        Json::CharReaderBuilder rb;
        std::istringstream ss(e.texts.at(0));
        Json::parseFromStream(rb, ss, &v, nullptr);
        return v;
    };
    send(c->onText(R"({"t":"hello","state":{"fw_version":"1.0.0","kind":"bridge"}})"));
    uint32_t fseq = 0;
    send(c->onBinary(frame(fseq++, cardStatement(kCard))));
    send(c->onBinary(frame(fseq++, batch(0, {0, 1, 2}, true))));   // mount reads 0..2
    send(c->onBinary(frame(fseq++, run(3, 5, 100))));              // 3..7
    std::string fill = header("CYS2", 2, 8);                       // 8..9
    put32(fill, 7);
    fill.push_back(static_cast<char>(kFillKindSector));
    fill += sector(7, 8);
    put32(fill, 8);
    fill.push_back(static_cast<char>(kFillKindFill));
    fill.push_back(static_cast<char>(0xAA));
    send(c->onBinary(frame(fseq++, fill)));
    send(c->onBinary(frame(fseq++, heartbeat())));
    svc.closed(*c);

    // Reconnect: frame seq starts over, the resend overlaps the mark.
    auto c2 = svc.open(who);
    fseq = 0;
    send(c2->onText(R"({"t":"hello","state":{}})"));
    const auto a = ackOf(send(c2->onBinary(frame(fseq++, run(6, 4, 104)))));   // 6..9
    EXPECT_EQ(a["ack"].asInt64(), 9);
    EXPECT_TRUE(a["duplicate"].asBool());
    const auto b = ackOf(send(c2->onBinary(frame(fseq++, run(8, 4, 106)))));   // 8..11
    EXPECT_EQ(b["ack"].asInt64(), 11);
    EXPECT_EQ(b["accepted"].asInt64(), 2);
    EXPECT_TRUE(svc.status()["bridges"][0]["online"].asBool());
    svc.closed(*c2);

    svc.stop();
    svc.rebuildNow(kSender);   // whatever the quiet timer had not reached

    std::vector<uint64_t> want;
    for (uint64_t i = 0; i <= 11; i++) want.push_back(i);
    EXPECT_EQ(log->seqs, want);

    const fs::path root = t.path / kSender;
    EXPECT_EQ(slurp(root / "STR.edf"), sector(7, 8) + std::string(kSectorSize, static_cast<char>(0xAA)));
    EXPECT_EQ(slurp(root / "DATALOG/20261030/100.edf"), sector(100, 3));
    EXPECT_EQ(slurp(root / "DATALOG/20261030/109.edf"), sector(109, 11));

    hms_cpap::LocalDataSource src(root.string());
    const auto days = src.listDateFolders();
    EXPECT_NE(std::find(days.begin(), days.end(), "20261030"), days.end());
    EXPECT_EQ(std::find(days.begin(), days.end(), "batches"), days.end());
    // 100..104 from the first connection, 108 and 109 from the resend's new
    // part; 106 and 107 rode only in its overlap, already fed as 8 and 9.
    EXPECT_EQ(src.listFiles("20261030").size(), 7u);

    const auto st = svc.status();
    ASSERT_EQ(st["bridges"].size(), 1u);
    EXPECT_EQ(st["bridges"][0]["sender"].asString(), kSender);
    EXPECT_EQ(st["bridges"][0]["machine"].asString(), "airsense-bench");
    EXPECT_FALSE(st["bridges"][0]["online"].asBool());
}

TEST(SleepBridgeRebuild, TheMachineIsTheRebuildsToSayNotAssumed) {
    EXPECT_EQ(formatForMachine("resmed"), "resmed");
    EXPECT_EQ(formatForMachine("luna"), "bmc");
    EXPECT_EQ(formatForMachine(""), "");
    EXPECT_EQ(formatForMachine("other"), "");

    TempDir t;
    auto log     = std::make_shared<FakeLog>();
    log->machine = "luna";
    // Whatever layout the rebuild gives is written as given.
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01},
                           fakeFactory(log, {"LUNA/20261030/session.dat", "settings.dat"}));
    auto c = svc.open({kSender, "luna-bench", ""});
    // Before the layout is trusted, the machine is not known yet.
    svc.apply(kSender, c->onBinary(frame(0, run(0, 1, 100))));
    svc.rebuildNow(kSender);
    EXPECT_EQ(svc.status()["bridges"][0]["card_machine"].asString(), "");
    EXPECT_EQ(svc.status()["bridges"][0]["card_format"].asString(), "");

    svc.apply(kSender, c->onBinary(frame(1, cardStatement(kCard))));
    svc.apply(kSender, c->onBinary(frame(2, run(1, 1, 101))));
    svc.rebuildNow(kSender);
    const auto b = svc.status()["bridges"][0];
    EXPECT_EQ(b["card_machine"].asString(), "luna");
    EXPECT_EQ(b["card_format"].asString(), "bmc");
    // The status shows the geometry the rebuild trusts.
    EXPECT_EQ(b["rebuild"]["geometry"]["layout"].asString(), "8:1000");
    EXPECT_EQ(b["rebuild"]["geometry"]["origin"].asString(), "stated");
    EXPECT_EQ(b["rebuild"]["geometry"]["confirmed_by"].asString(), "test");
    EXPECT_TRUE(fs::exists(t.path / kSender / "LUNA/20261030/session.dat"));
    EXPECT_TRUE(fs::exists(t.path / kSender / "settings.dat"));
}

TEST(SleepBridgeRebuild, TheServiceRebuildsAfterTheStreamGoesQuiet) {
    TempDir t;
    auto log = std::make_shared<FakeLog>();
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.05}, fakeFactory(log));
    svc.start();
    Admission who{kSender, "", ""};
    auto c = svc.open(who);
    svc.apply(kSender, c->onText(R"({"t":"hello","state":{}})"));
    svc.apply(kSender, c->onBinary(frame(0, cardStatement(kCard))));
    svc.apply(kSender, c->onBinary(frame(1, run(0, 2, 100))));
    const fs::path f = t.path / kSender / "DATALOG/20261030/101.edf";
    for (int i = 0; i < 200 && !fs::exists(f); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(fs::exists(f));
    EXPECT_TRUE(svc.status()["bridges"][0]["online"].asBool());
    svc.stop();
}

TEST(SleepBridgeRebuild, ABusyStreamIsStillRebuiltWithinTheCeiling) {
    TempDir t;
    auto log = std::make_shared<FakeLog>();
    SleepBridgeService::Options opt{t.path.string(), kDefaultPort, 0.2};
    opt.most_s = 0.3;
    SleepBridgeService svc(opt, fakeFactory(log));
    svc.start();
    auto c = svc.open({kSender, "", ""});
    svc.apply(kSender, c->onBinary(frame(0, cardStatement(kCard))));
    const fs::path f = t.path / kSender / "DATALOG/20261030/100.edf";
    // A batch every 50 ms never leaves the stream quiet for 200 ms.
    bool seen = false;
    for (int i = 0; i < 20 && !seen; i++) {
        svc.apply(kSender, c->onBinary(frame(i + 1, batch(static_cast<uint64_t>(i), {100}))));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        seen = fs::exists(f);
    }
    EXPECT_TRUE(seen);
    svc.stop();
}
