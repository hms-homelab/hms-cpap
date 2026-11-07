// SDD-064 D5: the automatic DreamStation 2 read runs after the night, in the
// hours after read_at, and never while a mask is on.

#include "services/Ds2Schedule.h"

#include <gtest/gtest.h>

using namespace hms_cpap::ds2sched;

namespace {
// A day's local midnight, and times of that day, through the same local clock.
long long at(int day, int h, int m) {
    const long long base = todayAt(1893456000 + 86400LL * day + 43200, 0);   // that local day's 00:00
    return base + h * 3600LL + m * 60LL;
}
}  // namespace

TEST(Ds2Schedule, TimeOfDay) {
    EXPECT_EQ(parseTimeOfDay("10:00"), 600);
    EXPECT_EQ(parseTimeOfDay("00:00"), 0);
    EXPECT_EQ(parseTimeOfDay("23:59"), 23 * 60 + 59);
    EXPECT_FALSE(parseTimeOfDay("24:00"));
    EXPECT_FALSE(parseTimeOfDay("9:00"));
    EXPECT_FALSE(parseTimeOfDay("10:60"));
    EXPECT_FALSE(parseTimeOfDay(""));
}

TEST(Ds2Schedule, DueOnceAfterReadAt) {
    Input in;
    in.read_at = "10:00";
    in.now = at(1, 9, 59);
    in.last_read_at = at(0, 10, 5);
    auto d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_EQ(d.next_at, at(1, 10, 0));

    in.now = at(1, 10, 1);
    d = decide(in);
    EXPECT_TRUE(d.due);

    in.last_read_at = at(1, 10, 2);
    in.now = at(1, 11, 0);
    d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_EQ(d.next_at, at(2, 10, 0));
}

TEST(Ds2Schedule, NeverWhileAMaskIsOn) {
    Input in;
    in.now = at(1, 10, 30);
    in.mask_on = true;
    const auto d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_NE(d.why.find("mask"), std::string::npos);
    EXPECT_EQ(d.next_at, at(1, 11, 30));
}

TEST(Ds2Schedule, NotLateInTheEveningEvenIfNeverRead) {
    // Paired at 11 PM: the day's read waits for tomorrow's 10:00.
    Input in;
    in.now = at(1, 23, 0);
    const auto d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_EQ(d.next_at, at(2, 10, 0));
    // Before read_at the night is not over either.
    in.now = at(2, 6, 0);
    EXPECT_FALSE(decide(in).due);
    in.now = at(2, 15, 59);
    EXPECT_TRUE(decide(in).due);
    in.now = at(2, 16, 0);
    EXPECT_FALSE(decide(in).due);
}

TEST(Ds2Schedule, AFailedTryWaitsAnHour) {
    Input in;
    in.now = at(1, 10, 20);
    in.last_attempt_at = at(1, 10, 1);
    auto d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_EQ(d.next_at, at(1, 11, 1));
    in.now = at(1, 11, 2);
    EXPECT_TRUE(decide(in).due);
}

TEST(Ds2Schedule, ABadTimeFallsBackToTen) {
    Input in;
    in.read_at = "nonsense";
    in.now = at(1, 9, 0);
    EXPECT_FALSE(decide(in).due);
    in.now = at(1, 10, 0);
    EXPECT_TRUE(decide(in).due);
}
