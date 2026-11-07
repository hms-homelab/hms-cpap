// SDD-064: a DreamStation 2 read over Bluetooth, through a fake transport and
// a synthetic machine (tests/clients/Ds2TestMachine.h). Pairing by numeric
// comparison, the first read, the resume from stored bookmarks, a read cut
// short, and forgetting the bond.

#include "Ds2TestMachine.h"
#include "clients/Ds2BleReader.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace hms_cpap;
namespace fs = std::filesystem;
using ds2test::Machine;
using ds2test::Transport;

namespace {

constexpr uint32_t kT0 = 1893456000;   // 2030-01-01, made up

class Ds2Reader : public ::testing::Test {
protected:
    fs::path dir_;
    std::shared_ptr<Transport> t_ = std::make_shared<Transport>();

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("ds2_reader_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::unique_ptr<Ds2BleReader> reader(Ds2Timeouts to = fast()) {
        return std::make_unique<Ds2BleReader>(dir_.string(), "", t_, to);
    }
    static Ds2Timeouts fast() {
        Ds2Timeouts to;
        to.scan_ms = 100;
        to.answer_ms = 300;
        to.pair_ms = 5000;
        to.confirm_ms = 2000;
        return to;
    }
    static Ds2BleReader::PairState waitFor(Ds2BleReader& r, std::function<bool(Ds2BleReader::PairState)> ok,
                                           int ms = 3000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            const auto s = r.pairStatus().state;
            if (ok(s)) return s;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return r.pairStatus().state;
    }
    static bool settled(Ds2BleReader::PairState s) {
        using S = Ds2BleReader::PairState;
        return s != S::Scanning && s != S::Pairing && s != S::Confirm && s != S::OsDialog;
    }
    void pairOk(Ds2BleReader& r) {
        ASSERT_TRUE(r.pairStart());
        ASSERT_EQ(waitFor(r, [](auto s) { return s == Ds2BleReader::PairState::Confirm; }),
                  Ds2BleReader::PairState::Confirm);
        ASSERT_TRUE(r.pairAnswer(true));
        ASSERT_EQ(waitFor(r, settled), Ds2BleReader::PairState::Paired);
        while (r.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
};

}  // namespace

TEST_F(Ds2Reader, PairingShowsTheCodeAndWaitsForTheUser) {
    auto r = reader();
    EXPECT_FALSE(r->paired());
    ASSERT_TRUE(r->pairStart());
    EXPECT_FALSE(r->pairStart());   // one at a time
    ASSERT_EQ(waitFor(*r, [](auto s) { return s == Ds2BleReader::PairState::Confirm; }),
              Ds2BleReader::PairState::Confirm);
    const auto st = r->pairStatus();
    EXPECT_EQ(st.code, "042517");
    EXPECT_EQ(st.machine_name, "PR BT 0001");
    EXPECT_GT(st.seconds_left, 0);
    EXPECT_TRUE(r->pairAnswer(true));
    EXPECT_FALSE(r->pairAnswer(true));   // answered once
    EXPECT_EQ(waitFor(*r, settled), Ds2BleReader::PairState::Paired);
    EXPECT_TRUE(r->pairStatus().code.empty());
    EXPECT_TRUE(r->paired());
    const auto p = r->pairedMachine();
    ASSERT_TRUE(p);
    EXPECT_EQ(p->address, "aa:bb:cc:dd:ee:01");
    EXPECT_EQ(p->name, "PR BT 0001");
    EXPECT_TRUE(p->serial.empty());   // learned on the first read
}

TEST_F(Ds2Reader, CodesThatDoNotMatchLeaveItUnpaired) {
    auto r = reader();
    ASSERT_TRUE(r->pairStart());
    ASSERT_EQ(waitFor(*r, [](auto s) { return s == Ds2BleReader::PairState::Confirm; }),
              Ds2BleReader::PairState::Confirm);
    EXPECT_TRUE(r->pairAnswer(false));
    EXPECT_EQ(waitFor(*r, settled), Ds2BleReader::PairState::Rejected);
    EXPECT_EQ(r->pairStatus().error, "the codes did not match");
    EXPECT_FALSE(r->paired());
}

TEST_F(Ds2Reader, NoAnswerInTimeIsNotAYes) {
    auto to = fast();
    to.confirm_ms = 150;
    auto r = reader(to);
    ASSERT_TRUE(r->pairStart());
    EXPECT_NE(waitFor(*r, settled), Ds2BleReader::PairState::Paired);
    EXPECT_FALSE(r->paired());
}

TEST_F(Ds2Reader, CancelWhileTheCodeShows) {
    auto r = reader();
    ASSERT_TRUE(r->pairStart());
    ASSERT_EQ(waitFor(*r, [](auto s) { return s == Ds2BleReader::PairState::Confirm; }),
              Ds2BleReader::PairState::Confirm);
    r->pairCancel();
    EXPECT_EQ(waitFor(*r, settled), Ds2BleReader::PairState::Cancelled);
    EXPECT_FALSE(r->paired());
}

TEST_F(Ds2Reader, WhereTheOsShowsTheCodeItSaysSo) {
    t_->os_dialog = true;
    auto r = reader();
    EXPECT_TRUE(r->osShowsCode());
    ASSERT_TRUE(r->pairStart());
    EXPECT_EQ(waitFor(*r, settled), Ds2BleReader::PairState::Paired);
    EXPECT_TRUE(r->paired());
}

TEST_F(Ds2Reader, NoMachineAdvertising) {
    t_->advertising = false;
    auto r = reader();
    ASSERT_TRUE(r->pairStart());
    EXPECT_EQ(waitFor(*r, settled), Ds2BleReader::PairState::NotFound);
}

TEST_F(Ds2Reader, ReadNeedsAPairing) {
    auto r = reader();
    const auto res = r->read();
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.code, "not_paired");
    EXPECT_EQ(t_->connects, 0);
}

TEST_F(Ds2Reader, FirstReadStoresEveryRecordThenResumesFromTheBookmarks) {
    t_->machine->addNight(500, kT0, 3600);
    t_->machine->addNight(501, kT0 + 86400, 7200, 2);
    auto r = reader();
    pairOk(*r);

    auto res = r->read();
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(res.records, 4);
    EXPECT_EQ(res.sessions, (std::vector<uint32_t>{500, 501}));
    EXPECT_EQ(res.identity.serial, "TEST0000000002");
    EXPECT_EQ(res.identity.model, "520X110C");

    const fs::path m = dir_ / "ds2" / "TEST0000000002";
    EXPECT_TRUE(fs::exists(m / "P0" / "000001F4.B01"));
    EXPECT_TRUE(fs::exists(m / "P0" / "000001F4.B02"));
    EXPECT_TRUE(fs::exists(m / "P0" / "000001F5.B01"));
    EXPECT_TRUE(fs::exists(m / "PROP.TXT"));
    std::ifstream prop(m / "PROP.TXT");
    std::string first;
    std::getline(prop, first);
    EXPECT_EQ(first, "SN=TEST0000000002\r");

    auto st = r->store().load();
    ASSERT_TRUE(st.paired);
    EXPECT_EQ(st.paired->serial, "TEST0000000002");
    const auto& mc = st.machines.at("TEST0000000002");
    EXPECT_EQ(mc.bookmarks.at(1), Machine::bookmarkFor(1, 1));
    EXPECT_EQ(mc.bookmarks.at(2), Machine::bookmarkFor(2, 1));
    EXPECT_EQ(mc.newest_session, 501u);
    EXPECT_GT(mc.last_read_at, 0);
    EXPECT_EQ(mc.last_records, 4);
    EXPECT_TRUE(mc.last_error.empty());
#ifndef _WIN32
    EXPECT_EQ(fs::status(dir_ / "ds2.json").permissions() & fs::perms::all,
              fs::perms::owner_read | fs::perms::owner_write);
#endif

    // Nothing new: every stream resumes from its bookmark, none from the oldest.
    t_->machine->bare_opens = 0;
    res = r->read();
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(res.records, 0);
    EXPECT_EQ(t_->machine->bare_opens, 3);   // stream 0, and the two log streams that never had a record

    // A night later: only that night.
    t_->machine->addNight(502, kT0 + 2 * 86400, 1800);
    res = r->read();
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(res.records, 2);
    EXPECT_EQ(res.sessions, (std::vector<uint32_t>{502}));
    EXPECT_EQ(r->store().load().machines.at("TEST0000000002").bookmarks.at(1), Machine::bookmarkFor(1, 2));
}

TEST_F(Ds2Reader, AReadCutShortKeepsWhatItStoredAndResumesAfterIt) {
    for (uint32_t i = 0; i < 4; ++i) t_->machine->addNight(600 + i, kT0 + i * 86400, 3600);
    auto r = reader();
    pairOk(*r);
    // Identity is 8 requests, then each summary takes open, chunk(s), bookmark.
    t_->machine->stop_after = 18;
    auto res = r->read();
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.code, "protocol");
    EXPECT_NE(res.error.find("no answer to 009a"), std::string::npos) << res.error;
    ASSERT_GT(res.records, 0);
    ASSERT_LT(res.records, 8);
    const int got = res.records;
    auto mc = r->store().load().machines.at("TEST0000000002");
    EXPECT_FALSE(mc.last_error.empty());
    EXPECT_EQ(mc.last_read_at, 0);
    EXPECT_EQ(mc.bookmarks.at(1), Machine::bookmarkFor(1, static_cast<size_t>(got - 1)));

    t_->machine->stop_after.reset();
    res = r->read();
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_EQ(got + res.records, 8);   // nothing twice, nothing skipped
    EXPECT_TRUE(r->store().load().machines.at("TEST0000000002").last_error.empty());
}

TEST_F(Ds2Reader, ARefusedSubscriptionMeansTheBondIsGone) {
    auto r = reader();
    pairOk(*r);
    t_->connect_error = ble::LinkError::Notify;
    const auto res = r->read();
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.code, "bond_lost");
}

TEST_F(Ds2Reader, ForgetRemovesTheBondAndKeepsTheBookmarks) {
    t_->machine->addNight(700, kT0, 3600);
    auto r = reader();
    pairOk(*r);
    ASSERT_TRUE(r->read().ok);
    EXPECT_TRUE(r->forget());
    EXPECT_EQ(t_->unpair_calls, 1);
    EXPECT_FALSE(r->paired());
    EXPECT_EQ(r->store().load().machines.at("TEST0000000002").bookmarks.at(1), Machine::bookmarkFor(1, 0));
    EXPECT_EQ(r->read().code, "not_paired");
}

TEST_F(Ds2Reader, ACorruptStateFileStartsClean) {
    {
        std::ofstream(dir_ / "ds2.json") << "{ not json";
    }
    auto r = reader();
    EXPECT_FALSE(r->paired());
    pairOk(*r);
    EXPECT_TRUE(r->paired());
}
