// SDD-064: the Devices page's DreamStation 2 answers, without a web server.

#include "../clients/Ds2TestMachine.h"
#include "web/Ds2BleApi.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace hms_cpap;
namespace fs = std::filesystem;

namespace {

class Ds2Api : public ::testing::Test {
protected:
    fs::path dir_;
    std::shared_ptr<ds2test::Transport> t_ = std::make_shared<ds2test::Transport>();
    std::unique_ptr<Ds2BleReader> r_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("ds2_api_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        Ds2Timeouts to;
        to.scan_ms = 50;
        to.answer_ms = 300;
        to.confirm_ms = 2000;
        r_ = std::make_unique<Ds2BleReader>(dir_.string(), "", t_, to);
    }
    void TearDown() override {
        r_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    Json::Value statusBody() { return ds2api::status(r_.get(), true, true, {{"sleep_bridge"}}).body; }
    void waitState(const std::string& s) {
        for (int i = 0; i < 600 && statusBody()["pairing"]["state"].asString() != s; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
};

}  // namespace

TEST_F(Ds2Api, NotCompiledOrDisabled) {
    auto s = ds2api::status(nullptr, false, false, {});
    EXPECT_EQ(s.status, 200);
    EXPECT_EQ(s.body["trigger"].asString(), "manual");   // no source says a night ended
    EXPECT_TRUE(s.body["night_sources"].empty());
    // D5a: the owner's choice, carried as it is set.
    EXPECT_EQ(s.body["read_mode"].asString(), "daily");
    EXPECT_EQ(s.body["read_at"].asString(), "11:00");
    EXPECT_EQ(s.body["next_read_at"].asInt64(), 0);
    ds2api::Trigger t;
    t.read_mode = "manual";
    t.read_at = "08:30";
    t.next_read_at = 1893492000;
    const auto chosen = ds2api::status(nullptr, true, true, t);
    EXPECT_EQ(chosen.body["read_mode"].asString(), "manual");
    EXPECT_EQ(chosen.body["read_at"].asString(), "08:30");
    EXPECT_EQ(chosen.body["next_read_at"].asInt64(), 1893492000);
    EXPECT_FALSE(s.body["compiled"].asBool());
    EXPECT_FALSE(s.body["paired"].asBool());
    EXPECT_EQ(s.body["pairing"]["state"].asString(), "idle");
    EXPECT_EQ(ds2api::pairStart(nullptr, false, true).body["result"].asString(), "not_compiled");
    EXPECT_EQ(ds2api::pairStart(r_.get(), true, false).body["result"].asString(), "disabled");
    EXPECT_EQ(ds2api::read(r_.get(), true, false, [] { return true; }).status, 409);
}

TEST_F(Ds2Api, PairThroughTheCode) {
    auto s = ds2api::pairStart(r_.get(), true, true);
    EXPECT_EQ(s.status, 200);
    EXPECT_TRUE(s.body["ok"].asBool());
    EXPECT_EQ(ds2api::pairStart(r_.get(), true, true).status, 409);
    waitState("confirm");
    auto st = statusBody();
    EXPECT_EQ(st["pairing"]["state"].asString(), "confirm");
    EXPECT_EQ(st["pairing"]["code"].asString(), "042517");
    EXPECT_FALSE(st["os_dialog"].asBool());

    EXPECT_EQ(ds2api::pairConfirm(r_.get(), Json::Value("yes")).status, 400);
    Json::Value yes;
    yes["match"] = true;
    EXPECT_EQ(ds2api::pairConfirm(r_.get(), yes).status, 200);
    EXPECT_EQ(ds2api::pairConfirm(r_.get(), yes).status, 409);   // no code waits now
    waitState("paired");
    st = statusBody();
    EXPECT_TRUE(st["paired"].asBool());
    EXPECT_EQ(st["machine"]["name"].asString(), "PR BT 0001");
    EXPECT_TRUE(st["last_read"].isNull());
    EXPECT_EQ(st["trigger"].asString(), "night_end");
    EXPECT_EQ(st["night_sources"][0].asString(), "sleep_bridge");
    // D9: the import date defaults to the day of pairing.
    EXPECT_EQ(st["import_from"].asString(), Ds2Store::localDate(std::time(nullptr)));
    EXPECT_FALSE(st["first_read_done"].asBool());

    Json::Value bad;
    bad["date"] = "2030-13-01";
    EXPECT_EQ(ds2api::setImportFrom(r_.get(), bad).status, 400);
    Json::Value day;
    day["date"] = "2030-01-02";
    auto set = ds2api::setImportFrom(r_.get(), day);
    EXPECT_EQ(set.status, 200);
    EXPECT_EQ(statusBody()["import_from"].asString(), "2030-01-02");
}

TEST_F(Ds2Api, ImportFromNeedsAPairing) {
    Json::Value day;
    day["date"] = "2030-01-02";
    EXPECT_EQ(ds2api::setImportFrom(r_.get(), day).status, 409);
}

TEST_F(Ds2Api, ReadStartsInTheBackground) {
    EXPECT_EQ(ds2api::read(r_.get(), true, true, [] { return true; }).body["result"].asString(), "not_paired");
    ds2api::pairStart(r_.get(), true, true);
    waitState("confirm");
    Json::Value yes;
    yes["match"] = true;
    ds2api::pairConfirm(r_.get(), yes);
    waitState("paired");
    while (r_->busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));

    bool started = false;
    auto a = ds2api::read(r_.get(), true, true, [&] { return started = true; });
    EXPECT_EQ(a.status, 202);
    EXPECT_EQ(a.body["result"].asString(), "reading");
    EXPECT_TRUE(started);
    EXPECT_EQ(ds2api::read(r_.get(), true, true, [] { return false; }).status, 409);

    t_->machine->addNight(42, 1893456000, 600);
    ASSERT_TRUE(r_->read().ok);
    auto st = statusBody();
    EXPECT_EQ(st["machine"]["serial"].asString(), "TEST0000000002");
    EXPECT_EQ(st["machine"]["model"].asString(), "520X110C");
    EXPECT_EQ(st["last_read"]["records"].asInt(), 2);
    EXPECT_EQ(st["last_read"]["error"].asString(), "");
}

TEST_F(Ds2Api, Forget) {
    auto f = ds2api::forget(r_.get());
    EXPECT_TRUE(f.body["ok"].asBool());
    EXPECT_FALSE(f.body["unpaired_os"].asBool());   // nothing was paired
    EXPECT_EQ(ds2api::pairCancel(r_.get()).body["state"].asString(), "cancelled");
}
