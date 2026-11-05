// Issue #56: which pressure a night's row reports as the machine's.
#include <gtest/gtest.h>

#include "utils/TherapyPressure.h"

using namespace hms_cpap;

namespace {

Json::Value row(const Json::Value& therapy, const Json::Value& mask) {
    Json::Value r;
    r["therapy_press_95"] = therapy;
    r["mask_press_95"] = mask;
    return r;
}

}  // namespace

// Minimum 7, EPR 3: the mask reads about 2 under what the machine delivered.
TEST(TherapyPressureTest, TheTherapyPressureWinsWhereTheNightHasOne) {
    EXPECT_DOUBLE_EQ(machinePressure(row(8.9, 6.9), "95"), 8.9);
}

// The engines hand numbers back as strings on some backends.
TEST(TherapyPressureTest, ANumberAsAStringIsANumber) {
    EXPECT_DOUBLE_EQ(machinePressure(row("8.9", "6.9"), "95"), 8.9);
}

TEST(TherapyPressureTest, ANightWithoutOneFallsBackToTheMask) {
    EXPECT_DOUBLE_EQ(machinePressure(row(Json::nullValue, 6.9), "95"), 6.9);
    EXPECT_DOUBLE_EQ(machinePressure(row("", "6.9"), "95"), 6.9);
    // A zero is no reading of a delivered pressure; no machine delivers 0.
    EXPECT_DOUBLE_EQ(machinePressure(row(0.0, 6.9), "95"), 6.9);
}

TEST(TherapyPressureTest, ANightWithNeitherIsZero) {
    EXPECT_DOUBLE_EQ(machinePressure(row(Json::nullValue, Json::nullValue), "95"), 0.0);
    EXPECT_DOUBLE_EQ(machinePressure(Json::Value(Json::objectValue), "50"), 0.0);
}
