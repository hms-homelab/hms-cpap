#include <gtest/gtest.h>

#include "clients/BlueZAdapter.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace hms_cpap::bluez;
namespace fs = std::filesystem;

// The adapter is made ready before any scan, with no BlueZ: the decision runs
// over callbacks that record what was asked of the host.

namespace {

struct Host {
    std::optional<bool> powered = false;
    std::vector<std::string> power_on_answers;   // one per power_on call, "" = ok
    std::vector<std::string> blocked;
    bool unblock_works = true;
    int power_reads = 0, power_ons = 0, rfkill_reads = 0;
    std::vector<std::string> unblocked;

    AdapterOps ops() {
        AdapterOps o;
        o.powered = [this] { power_reads++; return powered; };
        o.power_on = [this] {
            const size_t i = static_cast<size_t>(power_ons++);
            return i < power_on_answers.size() ? power_on_answers[i] : std::string("org.bluez.Error.Failed Failed");
        };
        o.soft_blocked = [this] { rfkill_reads++; return blocked; };
        o.unblock = [this](const std::string& f) { unblocked.push_back(f); return unblock_works; };
        return o;
    }
};

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("hms-rfkill-" + std::to_string(::getpid()) + "-" +
                                            std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    void rfkill(const std::string& name, const std::string& type, const std::string& soft) {
        fs::create_directories(path / name);
        std::ofstream(path / name / "type") << type << "\n";
        std::ofstream(path / name / "soft") << soft << "\n";
    }
};

} // namespace

TEST(BlueZAdapter, AnAdapterAlreadyOnIsLeftAlone) {
    Host h;
    h.powered = true;
    const auto out = ensurePowered(h.ops());
    EXPECT_EQ(out.power, Power::Ready);
    EXPECT_FALSE(out.powered_on);
    EXPECT_EQ(h.power_ons, 0);
    EXPECT_EQ(h.rfkill_reads, 0);
}

TEST(BlueZAdapter, AnAdapterOffIsPoweredOn) {
    Host h;
    h.power_on_answers = {""};
    const auto out = ensurePowered(h.ops());
    EXPECT_EQ(out.power, Power::Ready);
    EXPECT_TRUE(out.powered_on);
    EXPECT_FALSE(out.rfkill_lifted);
    EXPECT_EQ(h.power_ons, 1);
    EXPECT_EQ(h.rfkill_reads, 0);   // no refusal, so no reason to look at rf-kill
}

TEST(BlueZAdapter, ARefusalWithoutRfKillIsAPlainFailure) {
    Host h;
    h.power_on_answers = {"org.bluez.Error.Failed Failed"};
    const auto out = ensurePowered(h.ops());
    EXPECT_EQ(out.power, Power::Failed);
    EXPECT_NE(out.detail.find("org.bluez.Error.Failed"), std::string::npos);
    EXPECT_EQ(out.detail.find("rfkill"), std::string::npos);
    EXPECT_TRUE(h.unblocked.empty());
}

TEST(BlueZAdapter, ASoftBlockIsLiftedAndTheAdapterPoweredAgain) {
    Host h;
    h.power_on_answers = {"org.bluez.Error.Failed Failed", ""};
    h.blocked = {"/sys/class/rfkill/rfkill0/soft"};
    const auto out = ensurePowered(h.ops());
    EXPECT_EQ(out.power, Power::Ready);
    EXPECT_TRUE(out.powered_on);
    EXPECT_TRUE(out.rfkill_lifted);
    ASSERT_EQ(h.unblocked.size(), 1u);
    EXPECT_EQ(h.unblocked[0], "/sys/class/rfkill/rfkill0/soft");
    EXPECT_EQ(h.power_ons, 2);
}

TEST(BlueZAdapter, ASoftBlockThatCannotBeLiftedNamesRfKillAndTheCommand) {
    Host h;
    h.blocked = {"/sys/class/rfkill/rfkill0/soft"};
    h.unblock_works = false;   // not root: the sysfs file is read-only
    const auto out = ensurePowered(h.ops());
    EXPECT_EQ(out.power, Power::RfKilled);
    EXPECT_FALSE(out.powered_on);
    EXPECT_EQ(h.power_ons, 1);   // no second try: nothing changed
    EXPECT_NE(out.detail.find("rf-kill"), std::string::npos);
    EXPECT_NE(out.detail.find("rfkill unblock bluetooth"), std::string::npos);
    EXPECT_EQ(out.detail, rfkillAdvice());
}

TEST(BlueZAdapter, ABlockLiftedButStillRefusedIsRfKilled) {
    // A hard block (the laptop's switch) stays after the soft one is lifted.
    Host h;
    h.power_on_answers = {"org.bluez.Error.Failed Failed", "org.bluez.Error.Failed Failed"};
    h.blocked = {"/sys/class/rfkill/rfkill0/soft"};
    const auto out = ensurePowered(h.ops());
    EXPECT_EQ(out.power, Power::RfKilled);
    EXPECT_EQ(h.power_ons, 2);
}

TEST(BlueZAdapter, AnUnreadableAdapterIsAFailure) {
    Host h;
    h.powered = std::nullopt;
    const auto out = ensurePowered(h.ops());
    EXPECT_EQ(out.power, Power::Failed);
    EXPECT_EQ(h.power_ons, 0);
}

TEST(BlueZAdapter, OnlyBluetoothSwitchesThatReadOneAreSoftBlocked) {
    TempDir t;
    t.rfkill("rfkill0", "wlan", "1");         // the wifi switch: not ours
    t.rfkill("rfkill1", "bluetooth", "0");    // ours, but open
    t.rfkill("rfkill2", "bluetooth", "1");    // ours, blocked
    fs::create_directories(t.path / "other");  // not an rfkill entry at all
    const auto blocked = softBlockedBluetooth(t.path.string());
    ASSERT_EQ(blocked.size(), 1u);
    EXPECT_EQ(blocked[0], (t.path / "rfkill2" / "soft").string());
}

TEST(BlueZAdapter, NoRfKillTreeMeansNothingBlocked) {
    EXPECT_TRUE(softBlockedBluetooth("/nonexistent/rfkill").empty());
}

TEST(BlueZAdapter, LiftingASoftBlockWritesZero) {
    TempDir t;
    t.rfkill("rfkill0", "bluetooth", "1");
    const auto soft = (t.path / "rfkill0" / "soft").string();
    EXPECT_TRUE(liftSoftBlock(soft));
    EXPECT_TRUE(softBlockedBluetooth(t.path.string()).empty());
    std::ifstream in(soft);
    std::string v;
    std::getline(in, v);
    EXPECT_EQ(v, "0");
}

TEST(BlueZAdapter, LiftingAMissingSoftBlockFails) {
    EXPECT_FALSE(liftSoftBlock("/nonexistent/rfkill/rfkill0/soft"));
}

#ifndef _WIN32
TEST(BlueZAdapter, LiftingAReadOnlySoftBlockFails) {
    if (::geteuid() == 0) GTEST_SKIP() << "root writes anything";
    TempDir t;
    t.rfkill("rfkill0", "bluetooth", "1");
    const auto soft = (t.path / "rfkill0" / "soft").string();
    ::chmod(soft.c_str(), 0444);
    EXPECT_FALSE(liftSoftBlock(soft));
    ::chmod(soft.c_str(), 0644);
    EXPECT_EQ(softBlockedBluetooth(t.path.string()).size(), 1u);   // still blocked
}
#endif
