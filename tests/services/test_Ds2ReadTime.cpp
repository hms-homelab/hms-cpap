// SDD-064 D5a: the owner's choice of when the DreamStation 2 is read: once a
// day at a time, or only when asked; never while a mask is on.

#include "services/Ds2ReadTime.h"

#include <gtest/gtest.h>

using namespace hms_cpap::ds2time;

namespace {
// Times of a local day, through the same local clock the code uses.
long long at(int day, int h, int m) {
    const long long midnight = todayAt(1893456000 + 86400LL * day + 43200, 0);
    return midnight + h * 3600LL + m * 60LL;
}
Input daily(long long now) {
    Input in;
    in.now = now;
    in.read_mode = "daily";
    in.read_at = "11:00";
    in.paired_at = at(0, 9, 0);
    return in;
}
}  // namespace

TEST(Ds2ReadTime, TimeOfDayAndMode) {
    EXPECT_EQ(parseTimeOfDay("11:00"), 660);
    EXPECT_EQ(parseTimeOfDay("00:00"), 0);
    EXPECT_EQ(parseTimeOfDay("23:59"), 23 * 60 + 59);
    EXPECT_FALSE(parseTimeOfDay("24:00"));
    EXPECT_FALSE(parseTimeOfDay("9:00"));
    EXPECT_FALSE(parseTimeOfDay("11:60"));
    EXPECT_FALSE(parseTimeOfDay(""));
    EXPECT_EQ(normaliseMode("manual"), "manual");
    EXPECT_EQ(normaliseMode("daily"), "daily");
    EXPECT_EQ(normaliseMode("weekly"), "daily");
    EXPECT_EQ(normaliseMode(""), "daily");
    EXPECT_EQ(todayAt(at(3, 17, 5), 11 * 60), at(3, 11, 0));
}

TEST(Ds2ReadTime, OnceADayAtTheChosenTime) {
    auto in = daily(at(1, 10, 59));
    in.last_attempt_at = at(0, 11, 0);
    auto d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_EQ(d.next_at, at(1, 11, 0));

    in.now = at(1, 11, 0);
    d = decide(in);
    EXPECT_TRUE(d.due);
    EXPECT_TRUE(d.why.empty());

    // Read: not again that day, the next is tomorrow's.
    in.last_attempt_at = at(1, 11, 0);
    in.now = at(1, 19, 0);
    d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_EQ(d.next_at, at(2, 11, 0));
}

TEST(Ds2ReadTime, AnyReadAfterTheTimeCountsForTheDay) {
    // A night's end or Read now at 11:30 already read the machine today.
    auto in = daily(at(1, 12, 0));
    in.last_attempt_at = at(1, 11, 30);
    EXPECT_FALSE(decide(in).due);
    // One before the time does not.
    in.last_attempt_at = at(1, 7, 0);
    EXPECT_TRUE(decide(in).due);
}

TEST(Ds2ReadTime, AMaskOnDelaysItToTheNextCheckAfter) {
    auto in = daily(at(1, 11, 0));
    in.mask_on = true;
    auto d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_NE(d.why.find("mask"), std::string::npos);
    // A nap until 15:40: due at the first check after, no window to miss.
    in.now = at(1, 15, 41);
    in.mask_on = false;
    EXPECT_TRUE(decide(in).due);
    in.now = at(1, 23, 50);
    EXPECT_TRUE(decide(in).due);
}

TEST(Ds2ReadTime, AMissedDayIsNotReadTheNextMorning) {
    // hms-cpap was off all of day 1: at 06:00 on day 2 it waits for 11:00.
    auto in = daily(at(2, 6, 0));
    in.last_attempt_at = at(0, 11, 0);
    auto d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_EQ(d.next_at, at(2, 11, 0));
}

TEST(Ds2ReadTime, PairedAfterTheTimeWaitsForTomorrow) {
    auto in = daily(at(1, 23, 0));
    in.paired_at = at(1, 22, 30);
    auto d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_EQ(d.next_at, at(2, 11, 0));
    // Paired in the morning, never read: due at 11:00.
    in.paired_at = at(1, 8, 0);
    in.now = at(1, 11, 5);
    EXPECT_TRUE(decide(in).due);
}

TEST(Ds2ReadTime, OnlyWhenIAskNeverReadsByTime) {
    auto in = daily(at(1, 11, 30));
    in.read_mode = "manual";
    const auto d = decide(in);
    EXPECT_FALSE(d.due);
    EXPECT_EQ(d.next_at, 0);
}

TEST(Ds2ReadTime, ABadTimeIsTheDefault) {
    auto in = daily(at(1, 10, 0));
    in.read_at = "nonsense";
    EXPECT_FALSE(decide(in).due);
    in.now = at(1, 11, 0);
    EXPECT_TRUE(decide(in).due);
}

TEST(Ds2ReadTime, AnotherTimeOfDay) {
    auto in = daily(at(1, 7, 59));
    in.read_at = "08:00";
    EXPECT_FALSE(decide(in).due);
    in.now = at(1, 8, 0);
    EXPECT_TRUE(decide(in).due);
}
