//
// test_FailureLogThrottle.cpp — repeated identical failures must collapse.
//
// Regression cover for the offline-device log storm: hms-cpap retried an
// unreachable Mule roughly once a minute and logged every attempt, producing
// hundreds of identical lines (102 for the DATALOG listing plus 101 each for
// the O2Ring live read and its duplicate caller log) that buried real signal.
//
#include <gtest/gtest.h>
#include "utils/FailureLogThrottle.h"

#include <chrono>

using namespace hms_cpap;
using namespace std::chrono_literals;

namespace {

// Deterministic clock: tests advance time explicitly, never sleep.
struct FakeClock {
    FailureLogThrottle::TimePoint now{FailureLogThrottle::Clock::now()};
    void advance(std::chrono::seconds s) { now += s; }
};

FailureLogThrottle makeThrottle(FakeClock& clk, std::chrono::seconds interval = 15min) {
    return FailureLogThrottle(interval, [&clk] { return clk.now; });
}

const std::string kErr = "HTTP GET failed (http://device/dir): Could not connect to server";

} // namespace

TEST(FailureLogThrottle, FirstFailureIsAlwaysLogged) {
    FakeClock clk;
    auto t = makeThrottle(clk);
    auto d = t.onFailure(kErr);
    EXPECT_TRUE(d.log) << "an outage must be visible immediately";
    EXPECT_EQ(d.message, kErr);
    EXPECT_TRUE(t.failing());
}

TEST(FailureLogThrottle, IdenticalRepeatsAreSuppressed) {
    FakeClock clk;
    auto t = makeThrottle(clk);
    EXPECT_TRUE(t.onFailure(kErr).log);           // first: logged

    int logged = 0;
    for (int i = 0; i < 100; ++i) {               // ~100 poll cycles, 1 min apart
        clk.advance(60s);
        if (t.onFailure(kErr).log) ++logged;
    }
    // 100 minutes at a 15-minute summary interval => at most a handful of lines,
    // versus 100 before the fix.
    EXPECT_LE(logged, 7);
    EXPECT_GE(logged, 1) << "must still surface periodic summaries";
    EXPECT_EQ(t.consecutive(), 101u);
}

TEST(FailureLogThrottle, SummaryEmittedOncePerInterval) {
    FakeClock clk;
    auto t = makeThrottle(clk, 15min);
    t.onFailure(kErr);

    clk.advance(14min);
    EXPECT_FALSE(t.onFailure(kErr).log) << "before the interval, stay quiet";

    clk.advance(2min);                            // now past 15 min
    auto d = t.onFailure(kErr);
    ASSERT_TRUE(d.log);
    EXPECT_NE(d.message.find("still failing"), std::string::npos);
    EXPECT_NE(d.message.find("suppressed"), std::string::npos);
}

TEST(FailureLogThrottle, DifferentErrorLogsImmediately) {
    FakeClock clk;
    auto t = makeThrottle(clk);
    t.onFailure(kErr);
    clk.advance(60s);
    // A genuinely different failure is new information — never suppress it.
    auto d = t.onFailure("HTTP GET failed (http://device/dir): Timeout was reached");
    EXPECT_TRUE(d.log);
}

TEST(FailureLogThrottle, RecoveryIsAnnouncedWithStreak) {
    FakeClock clk;
    auto t = makeThrottle(clk);
    t.onFailure(kErr);
    for (int i = 0; i < 4; ++i) { clk.advance(60s); t.onFailure(kErr); }

    auto d = t.onSuccess();
    ASSERT_TRUE(d.log);
    EXPECT_NE(d.message.find("recovered after 5"), std::string::npos);
    EXPECT_FALSE(t.failing());
}

TEST(FailureLogThrottle, SuccessWithoutPriorFailureIsSilent) {
    FakeClock clk;
    auto t = makeThrottle(clk);
    EXPECT_FALSE(t.onSuccess().log) << "healthy polling must not log at all";
}

TEST(FailureLogThrottle, RecoversThenSuppressesAgainOnNewOutage) {
    FakeClock clk;
    auto t = makeThrottle(clk);
    t.onFailure(kErr);
    t.onSuccess();

    // A second outage starts a fresh streak: first line logged, repeats quiet.
    EXPECT_TRUE(t.onFailure(kErr).log);
    clk.advance(60s);
    EXPECT_FALSE(t.onFailure(kErr).log);
}

TEST(FailureLogThrottle, SingularWordingForOneFailure) {
    FakeClock clk;
    auto t = makeThrottle(clk);
    t.onFailure(kErr);
    auto d = t.onSuccess();
    ASSERT_TRUE(d.log);
    EXPECT_NE(d.message.find("recovered after 1 failure"), std::string::npos);
}
