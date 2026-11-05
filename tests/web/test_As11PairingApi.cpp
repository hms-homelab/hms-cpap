#include <gtest/gtest.h>

#include "web/As11PairingApi.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace hms_cpap;
using hms_cpap::as11::Result;

// SDD-057: the Settings page's AirSense 11 pairing endpoints, answered over a
// fake pairing so no web server and no Bluetooth are needed.

namespace {

struct FakePairing : IAirSense11Pairing {
    Result start_result = Result::WaitCode;
    Result confirm_result = Result::Ok;
    bool pending = false;
    bool is_paired = false;
    std::string error;
    std::string confirmed_code;
    int starts = 0, confirms = 0, cancels = 0;

    Result pairStart() override {
        starts++;
        pending = start_result == Result::WaitCode;
        return start_result;
    }
    Result pairConfirm(const std::string& code) override {
        confirms++;
        confirmed_code = code;
        pending = false;
        if (confirm_result == Result::Ok) is_paired = true;
        return confirm_result;
    }
    void pairCancel() override { cancels++; pending = false; }
    bool pairPending() const override { return pending; }
    bool paired() const override { return is_paired; }
    std::string lastError() const override { return error; }
    Presence presence() const override { return link; }
    bool busy() const override { return pending || pulling; }
    Presence link;
    bool pulling = false;
};

} // namespace

TEST(As11PairingApi, StatusCarriesTheLinkAsItIsNow) {
    FakePairing p;
    auto r = as11api::status(&p, true, true);
    EXPECT_FALSE(r.body["connected"].asBool());
    EXPECT_TRUE(r.body["therapy"].isNull()) << "no therapy state without a link";
    EXPECT_EQ(r.body["last_contact"].asInt64(), 0);
    p.link.connected    = true;
    p.link.therapy      = true;
    p.link.streaming    = true;
    p.link.last_contact = 1762300000;
    r = as11api::status(&p, true, true);
    EXPECT_TRUE(r.body["connected"].asBool());
    EXPECT_TRUE(r.body["therapy"].asBool());
    EXPECT_TRUE(r.body["streaming"].asBool());
    EXPECT_EQ(r.body["last_contact"].asInt64(), 1762300000);
}

TEST(As11PairingApi, ForgetRemovesThePairingButNotWhileBusy) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("as11_forget_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "as11.json").string();
    { std::ofstream(path) << R"({"clientId":"x","masterPairKey":"00"})"; }
    FakePairing p;
    p.pulling = true;
    auto r = as11api::forget(&p, path, false);
    EXPECT_EQ(r.status, 409);
    EXPECT_TRUE(std::filesystem::exists(path));
    p.pulling = false;
    r = as11api::forget(&p, path, /*pull_due=*/true);
    EXPECT_EQ(r.status, 409) << "a pull asked and not run yet is busy too";
    r = as11api::forget(&p, path, false);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body["removed"].asBool());
    EXPECT_FALSE(std::filesystem::exists(path));
    // Again: nothing to remove, still fine. And with no client (Bluetooth off).
    EXPECT_FALSE(as11api::forget(&p, path, false).body["removed"].asBool());
    EXPECT_EQ(as11api::forget(nullptr, path, false).status, 200);
    std::filesystem::remove_all(dir);
}

TEST(As11PairingApi, StatusSaysWhatTheHostCanDo) {
    auto r = as11api::status(nullptr, false, false);
    EXPECT_EQ(r.status, 200);
    EXPECT_FALSE(r.body["compiled"].asBool());
    EXPECT_EQ(r.body["backend"].asString(), "");
    EXPECT_FALSE(r.body["available"].asBool());
    EXPECT_FALSE(r.body["paired"].asBool());

    FakePairing p;
    p.is_paired = true;
    r = as11api::status(&p, true, true);
    EXPECT_TRUE(r.body["available"].asBool());
    EXPECT_TRUE(r.body["paired"].asBool());
    EXPECT_FALSE(r.body["pairing"].asBool());
}

TEST(As11PairingApi, NothingHappensWithoutBluetoothOrWhenOff) {
    FakePairing p;
    auto r = as11api::start(&p, false, true);
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.body["result"].asString(), "not_compiled");

    r = as11api::start(&p, true, false);
    EXPECT_EQ(r.status, 503);
    EXPECT_EQ(r.body["result"].asString(), "disabled");

    r = as11api::start(nullptr, true, true);   // on, but not saved yet
    EXPECT_EQ(r.body["result"].asString(), "disabled");
    EXPECT_EQ(p.starts, 0);
}

TEST(As11PairingApi, TwoStepsToPaired) {
    FakePairing p;
    auto r = as11api::start(&p, true, true);
    EXPECT_EQ(r.status, 200);
    EXPECT_TRUE(r.body["ok"].asBool());
    EXPECT_EQ(r.body["state"].asString(), "wait_code");

    r = as11api::confirm(&p, true, true, " 48 29 ");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body["state"].asString(), "paired");
    EXPECT_EQ(p.confirmed_code, "4829");
    EXPECT_TRUE(as11api::status(&p, true, true).body["paired"].asBool());
}

TEST(As11PairingApi, AFailedStartSaysWhy) {
    FakePairing p;
    p.start_result = Result::Connect;
    p.error = "machine not advertising";
    auto r = as11api::start(&p, true, true);
    EXPECT_FALSE(r.body["ok"].asBool());
    EXPECT_EQ(r.body["state"].asString(), "failed");
    EXPECT_EQ(r.body["result"].asString(), "connect");
    EXPECT_EQ(r.body["error"].asString(), "machine not advertising");
}

TEST(As11PairingApi, TheHostsOwnRadioOffIsNotTheMachineAway) {
    // rf-kill on the computer: a distinct code, so the page does not say
    // "check that the machine is on and close by".
    EXPECT_STREQ(as11api::resultCode(Result::RadioOff), "radio_off");
    FakePairing p;
    p.start_result = Result::RadioOff;
    p.error = "Bluetooth is switched off by rf-kill on this computer";
    auto r = as11api::start(&p, true, true);
    EXPECT_FALSE(r.body["ok"].asBool());
    EXPECT_EQ(r.body["state"].asString(), "failed");
    EXPECT_EQ(r.body["result"].asString(), "radio_off");
    EXPECT_EQ(r.body["error"].asString(), p.error);
    EXPECT_FALSE(as11api::status(&p, true, true).body["pairing"].asBool());
}

TEST(As11PairingApi, AWrongCodeIsAVerifyFailure) {
    FakePairing p;
    p.confirm_result = Result::Verify;
    as11api::start(&p, true, true);
    auto r = as11api::confirm(&p, true, true, "1234");
    EXPECT_EQ(r.body["result"].asString(), "verify");
    EXPECT_FALSE(as11api::status(&p, true, true).body["paired"].asBool());
}

TEST(As11PairingApi, ATypoKeepsTheExchangeOpen) {
    FakePairing p;
    as11api::start(&p, true, true);
    for (const char* bad : {"", "12a4", "123", "123456789"}) {
        auto r = as11api::confirm(&p, true, true, bad);
        EXPECT_EQ(r.status, 400) << bad;
        EXPECT_EQ(r.body["result"].asString(), "bad_code");
    }
    EXPECT_EQ(p.confirms, 0);
    EXPECT_TRUE(p.pending);
}

TEST(As11PairingApi, ConfirmWithoutAStartIsAConflict) {
    FakePairing p;
    auto r = as11api::confirm(&p, true, true, "4829");
    EXPECT_EQ(r.status, 409);
    EXPECT_EQ(r.body["result"].asString(), "no_pairing");
    EXPECT_EQ(p.confirms, 0);
}

TEST(As11PairingApi, CancelClosesTheExchange) {
    FakePairing p;
    as11api::start(&p, true, true);
    auto r = as11api::cancel(&p);
    EXPECT_TRUE(r.body["ok"].asBool());
    EXPECT_EQ(p.cancels, 1);
    EXPECT_FALSE(p.pending);
    EXPECT_TRUE(as11api::cancel(nullptr).body["ok"].asBool());
}

TEST(As11PairingApi, EveryResultHasAStableCode) {
    EXPECT_STREQ(as11api::resultCode(Result::Ok), "ok");
    EXPECT_STREQ(as11api::resultCode(Result::WaitCode), "wait_code");
    EXPECT_STREQ(as11api::resultCode(Result::NotPaired), "not_paired");
    EXPECT_STREQ(as11api::resultCode(Result::Connect), "connect");
    EXPECT_STREQ(as11api::resultCode(Result::Verify), "verify");
    EXPECT_STREQ(as11api::resultCode(Result::Protocol), "protocol");
    EXPECT_STREQ(as11api::resultCode(Result::Spool), "spool");
    EXPECT_EQ(as11api::normaliseCode("0042"), "0042");
}
