//
// SDD-013 D-701-3: a night landed, said once, by every path that stores one.
//
#include <gtest/gtest.h>

#include "services/NightLanded.h"
#include "services/SyncFolderState.h"
#include "utils/MachineZone.h"

#include <ctime>
#include <string>
#include <vector>

using namespace hms_cpap;

namespace {

struct Heard {
    std::vector<std::string> folders;
    std::vector<bool> with_str;
};

class NightLandedTest : public ::testing::Test {
protected:
    void SetUp() override {
        night_landed::clear();
        night_landed::listen([this](const std::string& folder, const std::optional<STRDailyRecord>& str) {
            heard_.folders.push_back(folder);
            heard_.with_str.push_back(str.has_value());
        });
    }
    void TearDown() override { night_landed::clear(); }
    Heard heard_;
};

std::chrono::system_clock::time_point label(int y, int mo, int d, int h) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    return wall::point(wall::labelOf(t));
}

}  // namespace

TEST_F(NightLandedTest, ANightIsToldItsFolderAndItsStrRow) {
    STRDailyRecord str;
    night_landed::announce("20261120", str);
    night_landed::announce("2026-11-21");
    ASSERT_EQ(heard_.folders.size(), 2u);
    EXPECT_EQ(heard_.folders[0], "20261120");
    EXPECT_TRUE(heard_.with_str[0]);
    EXPECT_EQ(heard_.folders[1], "20261121") << "a dashed day is the same night";
    EXPECT_FALSE(heard_.with_str[1]);
}

TEST_F(NightLandedTest, WhatIsNotADayIsNotAnnounced) {
    night_landed::announce("");
    night_landed::announce("2026112");
    night_landed::announce("not-a-day");
    EXPECT_TRUE(heard_.folders.empty());
}

TEST_F(NightLandedTest, AnImportsNightsAreToldOnceEachOldestFirst) {
    night_landed::announceDays({"2026-11-21", "20261120", "2026-11-20"});
    ASSERT_EQ(heard_.folders.size(), 2u);
    EXPECT_EQ(heard_.folders[0], "20261120");
    EXPECT_EQ(heard_.folders[1], "20261121");
}

TEST_F(NightLandedTest, SessionStartsAreToldByTheirNightNoonToNoon) {
    // Two sessions of the night of the 20th (one after midnight) and one of
    // the 21st.
    night_landed::announceStarts({label(2026, 11, 20, 23), label(2026, 11, 21, 3), label(2026, 11, 21, 22)});
    ASSERT_EQ(heard_.folders.size(), 2u);
    EXPECT_EQ(heard_.folders[0], "20261120");
    EXPECT_EQ(heard_.folders[1], "20261121");
    EXPECT_EQ(heard_.folders[0], strDayForSessionStart(label(2026, 11, 21, 3)));
}

TEST_F(NightLandedTest, EveryListenerIsToldAndAFailingOneStopsNobody) {
    int second = 0;
    night_landed::listen([](const std::string&, const std::optional<STRDailyRecord>&) {
        throw std::runtime_error("boom");
    });
    night_landed::listen([&second](const std::string&, const std::optional<STRDailyRecord>&) { ++second; });
    night_landed::announce("20261120");
    EXPECT_EQ(heard_.folders.size(), 1u);
    EXPECT_EQ(second, 1);
}

TEST(NightLandedNoListener, WithNoListenerNothingHappens) {
    night_landed::clear();
    night_landed::announce("20261120");   // nothing to tell, nothing breaks
    SUCCEED();
}
