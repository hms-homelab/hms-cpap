// SDD-061: a card learned on a computer. The CBL1 payload checked by content
// before it is kept; given to the bridges that can be that card; fed to the
// rebuild first, as a mount read in session 0; and the verdict on each bridge.
// The rebuild here is a recorder, not the component: what goes in, in which
// order, is the subject.

#include "LearnedCardTestUtil.h"
#include "SleepBridgeTestUtil.h"

#include "sleepbridge/LearnedCard.h"
#include "sleepbridge/RebuildWorker.h"
#include "sleepbridge/SleepBridgeService.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace sbtest;

namespace {

const std::string kSender = "TAP-S3-00000123";
const std::string kOther  = "TAP-S3-00000124";
constexpr uint64_t kSession1 = uint64_t{1} << 32;

struct Fed {
    uint64_t seq;
    uint32_t lba;
    bool     read;
};

struct Recorded {
    std::vector<Fed> fed;
    int built{0};
};

// Records every record in order; trusts nothing, writes nothing.
class Recorder : public IRebuilder {
public:
    explicit Recorder(std::shared_ptr<Recorded> r) : r_(std::move(r)) { ++r_->built; }
    void record(uint64_t seq, uint32_t lba, const uint8_t*, bool read) override {
        r_->fed.push_back({seq, lba, read});
    }
    void card(const CardStatement&) override {}
    RebuildLook look() override { return {}; }

private:
    std::shared_ptr<Recorded> r_;
};

RebuilderFactory recorder(std::shared_ptr<Recorded> r) {
    return [r](const RebuildStart&) { return std::make_unique<Recorder>(r); };
}

void bank(BatchStore& s, const std::string& sender, const std::string& body) {
    ASSERT_EQ(s.handle(sender, "airsense-bench", u8(body), body.size()).status, 200);
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

RebuildGeometry readGeometry(uint32_t spc, uint32_t data, uint32_t session) {
    RebuildGeometry g;
    g.sectors_per_cluster = spc;
    g.data_begin_lba      = data;
    g.root_cluster        = 2;
    g.origin              = "read";
    g.confirmed_by        = "read";
    g.session             = session;
    return g;
}

std::string refusal(const std::string& bytes) {
    std::string why;
    EXPECT_FALSE(decodeLearnedCard(bytes, &why).has_value());
    return why;
}

}  // namespace

// ── The payload ─────────────────────────────────────────────────────────────

TEST(LearnedCard, ADesktopPayloadDecodesWithItsLayout) {
    const auto c = lctest::card();
    std::string why;
    const auto d = decodeLearnedCard(c.cbl1(), &why);
    ASSERT_TRUE(d) << why;
    EXPECT_EQ(d->fat, "fat32");
    EXPECT_EQ(d->partition_lba, 8u);
    EXPECT_EQ(d->sectors_per_cluster, 8u);
    EXPECT_EQ(d->fat_begin_lba, 40u);
    EXPECT_EQ(d->data_begin_lba, 240u);
    EXPECT_EQ(d->root_cluster, 2u);
    EXPECT_EQ(d->volume_serial, "A1B2C3D4");
    EXPECT_EQ(d->brand, "resmed");
    // From the boot sector, which the statement does not carry.
    EXPECT_EQ(d->fats, 2u);
    EXPECT_EQ(d->fat_sectors, 100u);
    EXPECT_EQ(d->cluster_count, (2000000u - 232u) / 8u);
    ASSERT_EQ(d->sectors.size(), 5u);
    EXPECT_EQ(d->sectors.front().first, 0u);
    EXPECT_EQ(d->folders, std::vector<std::string>{"DATALOG"});
}

TEST(LearnedCard, AFat16CardDecodes) {
    const auto c = lctest::card(/*fat16=*/true);
    std::string why;
    const auto d = decodeLearnedCard(c.cbl1(), &why);
    ASSERT_TRUE(d) << why;
    EXPECT_FALSE(d->fat32());
    EXPECT_EQ(d->data_begin_lba, 144u);
}

TEST(LearnedCard, TheFrameIsChecked) {
    const std::string good = lctest::card().cbl1();
    EXPECT_NE(refusal("CBL2" + good.substr(4)).find("not a card layout"), std::string::npos);
    EXPECT_NE(refusal(good.substr(0, good.size() - 1)).find("cut short"), std::string::npos);
    EXPECT_NE(refusal(good + "x").find("cut short"), std::string::npos);
    EXPECT_NE(refusal("CBL1").find("not a card layout"), std::string::npos);
    std::string huge = good;
    huge.resize(kLearnedCardMax + 1, '\0');
    EXPECT_NE(refusal(huge).find("32 MB"), std::string::npos);

    auto c = lctest::card();
    std::swap(c.sectors[1], c.sectors[2]);
    EXPECT_NE(refusal(c.cbl1()).find("out of order"), std::string::npos);
}

TEST(LearnedCard, TheStatementMustNameTheLayout) {
    for (const char* key : {"fat", "boot_sha256", "partition_lba", "sectors_per_cluster",
                            "data_begin_lba", "root_cluster", "fat_begin_lba", "root_dir_lbas"}) {
        auto c = lctest::card();
        c.statement.removeMember(key);
        EXPECT_NE(refusal(c.cbl1()).find("leaves out part"), std::string::npos) << key;
    }
    auto c = lctest::card();
    c.statement["fat"] = "fat12";
    EXPECT_NE(refusal(c.cbl1()).find("fat12"), std::string::npos);
}

TEST(LearnedCard, TheBootSectorIsCheckedByContent) {
    {
        auto c = lctest::card();
        c.drop(8);
        EXPECT_NE(refusal(c.cbl1()).find("no boot sector"), std::string::npos);
    }
    {
        auto c = lctest::card();
        c.at(8)[510] = 0;
        c.rehash();
        EXPECT_NE(refusal(c.cbl1()).find("does not describe"), std::string::npos);
    }
    {
        auto c = lctest::card();
        c.at(8)[100] = 1;   // the hash no longer names it
        EXPECT_NE(refusal(c.cbl1()).find("not the one"), std::string::npos);
    }
    for (const char* key : {"sectors_per_cluster", "data_begin_lba", "fat_begin_lba", "root_cluster"}) {
        auto c = lctest::card();
        c.statement[key] = c.statement[key].asUInt() + 1;
        EXPECT_NE(refusal(c.cbl1()).find("does not agree"), std::string::npos) << key;
    }
}

TEST(LearnedCard, TheRootFolderAndTheFatAreCheckedByContent) {
    {
        auto c = lctest::card();
        c.drop(240);
        EXPECT_NE(refusal(c.cbl1()).find("root folder"), std::string::npos);
    }
    {
        auto c = lctest::card();
        c.at(240) = std::string(kSectorSize, '\x01');
        EXPECT_NE(refusal(c.cbl1()).find("does not read as a folder"), std::string::npos);
    }
    {
        auto c = lctest::card();
        c.drop(40);
        EXPECT_NE(refusal(c.cbl1()).find("file table"), std::string::npos);
    }
    {
        auto c = lctest::card();
        lctest::le32(c.at(40), 4, 0x00000005);
        EXPECT_NE(refusal(c.cbl1()).find("does not start as a FAT"), std::string::npos);
    }
    {
        // The clean and error flags a machine clears are not a fault.
        auto c = lctest::card();
        lctest::le32(c.at(40), 4, 0x03FFFFFF);
        std::string why;
        EXPECT_TRUE(decodeLearnedCard(c.cbl1(), &why)) << why;
    }
}

TEST(LearnedCard, LayoutsAgreeOnClustersDataAndFat) {
    const auto d = *decodeLearnedCard(lctest::card().cbl1(), nullptr);
    auto g = readGeometry(8, 240, 1);
    EXPECT_TRUE(sameLayout(d, g));
    g.fat_begin_lba = 40;
    EXPECT_TRUE(sameLayout(d, g));
    g.fat_begin_lba = 41;
    EXPECT_FALSE(sameLayout(d, g));
    EXPECT_FALSE(sameLayout(d, readGeometry(64, 240, 1)));
    EXPECT_FALSE(sameLayout(d, readGeometry(8, 24576, 1)));
    EXPECT_FALSE(sameLayout(d, RebuildGeometry{}));
    EXPECT_TRUE(sameLayout(learnedSummary(d), readGeometry(8, 240, 1)));

    EXPECT_TRUE(knowsOwnLayout(readGeometry(8, 240, 1)));
    EXPECT_FALSE(knowsOwnLayout(readGeometry(8, 240, 0)));   // the learn's own session
    auto solved   = readGeometry(8, 240, 1);
    solved.origin = "solved";
    EXPECT_FALSE(knowsOwnLayout(solved));
}

// ── The rebuild: fed first, as a mount read in session 0 ────────────────────

TEST(LearnedCard, TheLearnedSectorsGoFirstAsReadsInSessionZero) {
    TempDir t;
    BatchStore store(t.path.string());
    const auto c = lctest::card();
    bank(store, kSender, run(kSession1 + 1, 2, 300));
    ASSERT_TRUE(giveBridgeLearned(store.batchesDir(kSender), c.cbl1(),
                                  learnedSummary(*decodeLearnedCard(c.cbl1(), nullptr)), nullptr));

    auto rec = std::make_shared<Recorded>();
    RebuildWorker w(store, kSender, recorder(rec));
    const auto r = w.step();
    EXPECT_EQ(w.learned().state, "fed");
    EXPECT_EQ(r.learned_fed, 5);
    EXPECT_EQ(r.records_fed, 2);   // the seed is not counted as capture
    ASSERT_EQ(rec->fed.size(), 7u);
    for (size_t i = 0; i < 5; i++) {
        EXPECT_EQ(rec->fed[i].seq, i);
        EXPECT_EQ(rec->fed[i].lba, c.sectors[i].first);
        EXPECT_TRUE(rec->fed[i].read);
    }
    EXPECT_EQ(rec->fed[5].seq, kSession1 + 1);
    EXPECT_FALSE(rec->fed[5].read);

    // Only once per rebuilder: the next batch adds only itself.
    bank(store, kSender, run(kSession1 + 3, 1, 302));
    const auto r2 = w.step();
    EXPECT_EQ(r2.learned_fed, 0);
    EXPECT_EQ(rec->fed.size(), 8u);
}

TEST(LearnedCard, ARestartFeedsItAgainBeforeTheBank) {
    TempDir t;
    BatchStore store(t.path.string());
    const auto c = lctest::card();
    bank(store, kSender, run(kSession1 + 1, 2, 300));
    giveBridgeLearned(store.batchesDir(kSender), c.cbl1(), Json::objectValue, nullptr);
    {
        auto rec = std::make_shared<Recorded>();
        RebuildWorker w(store, kSender, recorder(rec));
        w.step();
    }
    auto rec = std::make_shared<Recorded>();
    RebuildWorker again(store, kSender, recorder(rec));
    again.step();
    ASSERT_EQ(rec->fed.size(), 7u);
    EXPECT_EQ(rec->fed[0].seq, 0u);
    EXPECT_EQ(rec->fed[5].seq, kSession1 + 1);
}

TEST(LearnedCard, ALayoutTheBridgeReadFromItsOwnCardWins) {
    TempDir t;
    BatchStore store(t.path.string());
    const auto c = lctest::card();
    bank(store, kSender, run(kSession1 + 1, 1, 300));
    giveBridgeLearned(store.batchesDir(kSender), c.cbl1(), Json::objectValue, nullptr);
    writeFile(fs::path(store.batchesDir(kSender)) / kGeometryFile, geometryJson(readGeometry(64, 24576, 3)));

    auto rec = std::make_shared<Recorded>();
    RebuildWorker w(store, kSender, recorder(rec));
    w.step();
    EXPECT_EQ(w.learned().state, "another_layout");
    ASSERT_EQ(rec->fed.size(), 1u);   // the bank alone
    EXPECT_EQ(rec->fed[0].seq, kSession1 + 1);
}

TEST(LearnedCard, TheSameLayoutReadFromTheCardIsStillFed) {
    TempDir t;
    BatchStore store(t.path.string());
    const auto c = lctest::card();
    bank(store, kSender, run(kSession1 + 1, 1, 300));
    giveBridgeLearned(store.batchesDir(kSender), c.cbl1(), Json::objectValue, nullptr);
    writeFile(fs::path(store.batchesDir(kSender)) / kGeometryFile, geometryJson(readGeometry(8, 240, 3)));
    auto rec = std::make_shared<Recorded>();
    RebuildWorker w(store, kSender, recorder(rec));
    EXPECT_EQ(w.learned().state, "fed");
}

TEST(LearnedCard, NotFedToABankThatStreamedInSessionZero) {
    TempDir t;
    BatchStore store(t.path.string());
    const auto c = lctest::card();
    bank(store, kSender, run(1, 1, 300));
    giveBridgeLearned(store.batchesDir(kSender), c.cbl1(), Json::objectValue, nullptr);
    auto rec = std::make_shared<Recorded>();
    RebuildWorker w(store, kSender, recorder(rec));
    EXPECT_EQ(w.learned().state, "session_zero");
    EXPECT_TRUE(rec->fed.empty());
}

TEST(LearnedCard, AFat16CardIsKeptButNotFed) {
    TempDir t;
    BatchStore store(t.path.string());
    const auto c = lctest::card(/*fat16=*/true);
    bank(store, kSender, run(kSession1 + 1, 1, 300));
    giveBridgeLearned(store.batchesDir(kSender), c.cbl1(), Json::objectValue, nullptr);
    auto rec = std::make_shared<Recorded>();
    RebuildWorker w(store, kSender, recorder(rec));
    EXPECT_EQ(w.learned().state, "fat16");
    EXPECT_TRUE(rec->fed.empty());
    EXPECT_EQ(learnedVerdict(w.learned(), true, {}, false, "").verdict, "not_used");
}

TEST(LearnedCard, ABridgeWithoutALearnedCardIsUntouched) {
    TempDir t;
    BatchStore store(t.path.string());
    bank(store, kSender, run(kSession1 + 1, 1, 300));
    auto rec = std::make_shared<Recorded>();
    RebuildWorker w(store, kSender, recorder(rec));
    w.step();
    EXPECT_EQ(w.learned().state, "");
    ASSERT_EQ(rec->fed.size(), 1u);
}

// ── The verdict ─────────────────────────────────────────────────────────────

TEST(LearnedCard, TheVerdictFollowsWhatTheRebuildTrusts) {
    const auto d = *decodeLearnedCard(lctest::card().cbl1(), nullptr);
    LearnedUse fed;
    fed.state   = "fed";
    fed.summary = learnedSummary(d);

    EXPECT_EQ(learnedVerdict(fed, false, {}, false, "").verdict, "waiting");
    // Trusted from the learn itself, session 0: nothing written since.
    EXPECT_EQ(learnedVerdict(fed, true, readGeometry(8, 240, 0), false, "").verdict, "waiting");
    // A session read the same boot sector.
    auto v = learnedVerdict(fed, true, readGeometry(8, 240, 4), false, "");
    EXPECT_EQ(v.verdict, "latched");
    EXPECT_EQ(v.reason, "boot sector matches");
    // A session's writes passed the test.
    auto tested         = readGeometry(8, 240, 4);
    tested.confirmed_by = "test";
    v = learnedVerdict(fed, true, tested, false, "");
    EXPECT_EQ(v.verdict, "latched");
    EXPECT_NE(v.reason.find("writes landed"), std::string::npos);
    // Another card in the bridge.
    EXPECT_EQ(learnedVerdict(fed, true, readGeometry(64, 24576, 4), false, "").verdict, "mismatch");
    // The writes contradict the learn.
    EXPECT_EQ(learnedVerdict(fed, true, readGeometry(8, 240, 0), true, "geometry_failed").verdict,
              "mismatch");
    // The bridge's own read layout refused the learn.
    LearnedUse refused;
    refused.state = "another_layout";
    EXPECT_EQ(learnedVerdict(refused, true, {}, false, "").verdict, "mismatch");
}

// ── The service: kept, given, rebuilt, shown ────────────────────────────────

TEST(LearnedCard, TheServiceRefusesWhatFailsTheCheck) {
    TempDir t;
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01}, fakeFactory(std::make_shared<FakeLog>()));
    const auto a = svc.learnCard("not a card", "x.cpapcard");
    EXPECT_EQ(a.status, 400);
    EXPECT_FALSE(a.body["error"].asString().empty());
    EXPECT_FALSE(fs::exists(t.path / kLearnedDir / kLearnedCardFile));
    EXPECT_TRUE(svc.status()["learned_card"].isNull());
}

TEST(LearnedCard, TheServiceKeepsItAndGivesItToTheBridgesThatCanBeThatCard) {
    TempDir t;
    auto log = std::make_shared<FakeLog>();
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01}, fakeFactory(log));
    bank(svc.store(), kSender, run(kSession1 + 1, 1, 300));
    bank(svc.store(), kOther, run(kSession1 + 1, 1, 300));
    // kOther read its own card's boot sector: another layout.
    writeFile(fs::path(svc.store().batchesDir(kOther)) / kGeometryFile,
              geometryJson(readGeometry(64, 24576, 2)));
    svc.rebuildNow(kSender);
    const size_t built = log->starts.size();

    const auto c = lctest::card();
    const auto a = svc.learnCard(c.cbl1(), "23200000000-20261101.cpapcard");
    ASSERT_EQ(a.status, 201) << a.body.toStyledString();
    EXPECT_TRUE(a.body["stored"].asBool());
    EXPECT_EQ(a.body["volume_serial"].asString(), "A1B2C3D4");
    EXPECT_EQ(a.body["sectors"].asUInt(), 5u);
    ASSERT_EQ(a.body["bridges"].size(), 2u);
    for (const auto& b : a.body["bridges"]) {
        if (b["sender"].asString() == kSender) EXPECT_TRUE(b["applied"].asBool());
        else EXPECT_FALSE(b["applied"].asBool()) << b["reason"].asString();
    }
    EXPECT_TRUE(fs::exists(t.path / kLearnedDir / kLearnedCardFile));
    EXPECT_EQ(svc.status()["learned_card"]["file_name"].asString(), "23200000000-20261101.cpapcard");
    EXPECT_TRUE(fs::exists(fs::path(svc.store().batchesDir(kSender)) / kBridgeLearned));
    EXPECT_FALSE(fs::exists(fs::path(svc.store().batchesDir(kOther)) / kBridgeLearned));

    // kSender is rebuilt from scratch, the learn first.
    svc.rebuildNow(kSender);
    EXPECT_EQ(log->starts.size(), built + 1);
    Json::Value mine;
    const Json::Value status = svc.status();
    for (const auto& b : status["bridges"])
        if (b["sender"].asString() == kSender) mine = b;
    ASSERT_TRUE(mine["learned"].isObject());
    EXPECT_EQ(mine["learned"]["state"].asString(), "fed");
    EXPECT_EQ(mine["learned"]["sectors_fed"].asInt(), 5);
    EXPECT_EQ(mine["learned"]["volume_serial"].asString(), "A1B2C3D4");
    EXPECT_FALSE(mine["learned"]["verdict"].asString().empty());
}

TEST(LearnedCard, ABridgeFirstSeenAfterTheUploadTakesItUnlessAnotherHoldsIt) {
    TempDir t;
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01}, fakeFactory(std::make_shared<FakeLog>()));
    const auto c = lctest::card();
    ASSERT_EQ(svc.learnCard(c.cbl1(), "card.cpapcard").status, 201);   // no bridge yet

    // The card goes into the bridge; its first batch is the event.
    bank(svc.store(), kSender, run(kSession1 + 1, 1, 300));
    svc.rebuildNow(kSender);
    EXPECT_TRUE(fs::exists(fs::path(svc.store().batchesDir(kSender)) / kBridgeLearned));

    // A second bridge, on another machine, does not take the same card.
    bank(svc.store(), kOther, run(kSession1 + 1, 1, 300));
    svc.rebuildNow(kOther);
    EXPECT_FALSE(fs::exists(fs::path(svc.store().batchesDir(kOther)) / kBridgeLearned));
}

TEST(LearnedCard, ABridgeWithAGeometryAlreadyIsNotOfferedItAtItsFirstRebuild) {
    TempDir t;
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01}, fakeFactory(std::make_shared<FakeLog>()));
    ASSERT_EQ(svc.learnCard(lctest::card().cbl1(), "card.cpapcard").status, 201);
    bank(svc.store(), kSender, run(kSession1 + 1, 1, 300));
    writeFile(fs::path(svc.store().batchesDir(kSender)) / kGeometryFile, geometryJson(readGeometry(8, 240, 1)));
    svc.rebuildNow(kSender);
    EXPECT_FALSE(fs::exists(fs::path(svc.store().batchesDir(kSender)) / kBridgeLearned));
}

TEST(LearnedCard, ABuildWithoutTheRebuildStillKeepsIt) {
    TempDir t;
    SleepBridgeService svc({t.path.string(), kDefaultPort, 0.01}, RebuilderFactory{});
    const auto a = svc.learnCard(lctest::card().cbl1(), "card.cpapcard");
    EXPECT_EQ(a.status, 201);
    EXPECT_FALSE(a.body["rebuild_in_build"].asBool());
    EXPECT_TRUE(fs::exists(t.path / kLearnedDir / kLearnedCardFile));
}
