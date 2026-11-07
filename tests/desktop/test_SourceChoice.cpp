// The desktop first-run wizard's source step: what each choice writes to
// config.json, and the Sleep Bridge choice, which writes nothing and says
// where to point the bridge.

#include <gtest/gtest.h>

#include "ConfigGate.h"
#include "SourceChoice.h"

#include <nlohmann/json.hpp>

using namespace cpapdash::supervisor;

namespace {

SourceInputs inputs() {
    return {"http://cpapdash.local", "http://flashair.local", "/tmp/card", "/tmp/archive"};
}

/// What a first run holds when the source page is reached: the database is
/// picked on the next page, so only what the welcome page leaves.
ConfigModel firstRun() {
    ConfigModel m;
    m.loadFromDisk(nlohmann::json::object());
    return m;
}

}  // namespace

TEST(SourceChoiceTest, SleepBridgeWritesNothing) {
    auto m = firstRun();
    const auto before = m.current();
    applySourceChoice(m, SourceChoice::SleepBridge, inputs());
    EXPECT_EQ(m.current(), before);
    EXPECT_TRUE(m.changedPaths().empty());
}

TEST(SourceChoiceTest, SleepBridgeLeavesAnExistingSourceAlone) {
    // Like the web wizard's skip: the choice changes nothing in the config.
    ConfigModel m;
    m.loadFromDisk({{"source", "ezshare"}, {"ezshare_url", "http://10.0.0.9"},
                    {"archive_dir", "/keep"}});
    applySourceChoice(m, SourceChoice::SleepBridge, inputs());
    EXPECT_EQ(m.getString("source"), "ezshare");
    EXPECT_EQ(m.getString("ezshare_url"), "http://10.0.0.9");
    EXPECT_EQ(m.getString("archive_dir"), "/keep");
    EXPECT_TRUE(m.changedPaths().empty());
}

TEST(SourceChoiceTest, ASleepBridgeOnlySetupPassesTheGate) {
    // What the wizard ends with for a Sleep Bridge: no source keys, the
    // built-in database, setup_complete. The supervisor must start it rather
    // than reopen the wizard.
    auto m = firstRun();
    applySourceChoice(m, SourceChoice::SleepBridge, inputs());
    m.set("database.type", "sqlite");
    m.set("setup_complete", true);
    const auto r = evaluateConfig(m.toDiskJson());
    EXPECT_EQ(r.readiness, Readiness::Ready) << r.summary;
}

TEST(SourceChoiceTest, TheOtherChoicesStillWriteTheirSource) {
    auto local = firstRun();
    applySourceChoice(local, SourceChoice::Local, inputs());
    EXPECT_EQ(local.getString("source"), "local");
    EXPECT_EQ(local.getString("local_dir"), "/tmp/card");
    EXPECT_EQ(local.getString("archive_dir"), "/tmp/archive");

    auto flashair = firstRun();
    applySourceChoice(flashair, SourceChoice::FlashAir, inputs());
    EXPECT_EQ(flashair.getString("source"), "flashair");
    EXPECT_EQ(flashair.getString("flashair_url"), "http://flashair.local");
    EXPECT_EQ(flashair.getString("archive_dir"), "/tmp/archive");

    auto mule = firstRun();
    applySourceChoice(mule, SourceChoice::Mule, inputs());
    EXPECT_EQ(mule.getString("source"), "ezshare");
    EXPECT_EQ(mule.getString("ezshare_url"), "http://cpapdash.local");
    EXPECT_EQ(mule.getString("archive_dir"), "/tmp/archive");
}

TEST(SourceChoiceTest, AFirstRunOpensOnTheSleepBridge) {
    EXPECT_EQ(initialSourceChoice(firstRun()), SourceChoice::SleepBridge);
}

TEST(SourceChoiceTest, TheServiceDefaultEzShareStillOpensOnTheSleepBridge) {
    // The service saves source=ezshare as its default on first start, so it
    // is no sign of a Mule; the Sleep Bridge writes nothing and keeps it.
    ConfigModel m;
    m.loadFromDisk({{"source", "ezshare"}, {"ezshare_url", "http://192.168.4.1"}});
    EXPECT_EQ(initialSourceChoice(m), SourceChoice::SleepBridge);
}

TEST(SourceChoiceTest, ASavedFolderOrFlashAirOpensOnItself) {
    ConfigModel local;
    local.loadFromDisk({{"source", "local"}, {"local_dir", "/gone"}});
    EXPECT_EQ(initialSourceChoice(local), SourceChoice::Local);

    ConfigModel flashair;
    flashair.loadFromDisk({{"source", "flashair"}});
    EXPECT_EQ(initialSourceChoice(flashair), SourceChoice::FlashAir);
}

TEST(SourceChoiceTest, ThePortIsTheConfiguredOne) {
    EXPECT_EQ(sleepBridgePort(firstRun()), 8894);

    ConfigModel set;
    set.loadFromDisk({{"sleep_bridge", {{"enabled", true}, {"port", 9100}}}});
    EXPECT_EQ(sleepBridgePort(set), 9100);

    ConfigModel bad;
    bad.loadFromDisk({{"sleep_bridge", {{"port", 0}}}});
    EXPECT_EQ(sleepBridgePort(bad), kSleepBridgeDefaultPort);
}

TEST(SourceChoiceTest, TheAddressIsThePrivateLanOne) {
    EXPECT_EQ(lanAddress({"127.0.0.1", "169.254.3.4", "192.168.2.20"}), "192.168.2.20");
    EXPECT_EQ(lanAddress({"100.64.1.2", "10.0.0.5"}), "10.0.0.5");
    EXPECT_EQ(lanAddress({"172.20.1.1"}), "172.20.1.1");
    // No private address: the first routable one rather than none.
    EXPECT_EQ(lanAddress({"100.64.1.2"}), "100.64.1.2");
    EXPECT_EQ(lanAddress({"127.0.0.1", "169.254.1.1", "nonsense", "300.1.1.1"}), "");
    EXPECT_EQ(lanAddress({}), "");
}

TEST(SourceChoiceTest, TheNoteGivesAddressAndPort) {
    const auto note = sleepBridgeNote("192.168.2.20", 8894);
    EXPECT_NE(note.find("already listening"), std::string::npos);
    EXPECT_NE(note.find("192.168.2.20:8894"), std::string::npos);
    EXPECT_NE(note.find("own folder"), std::string::npos);
}

TEST(SourceChoiceTest, TheNoteStillGivesThePortWithNoAddress) {
    const auto note = sleepBridgeNote("", 9100);
    EXPECT_NE(note.find("this computer's IP address, port 9100"), std::string::npos);
}
