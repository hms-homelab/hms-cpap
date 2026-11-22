#include <gtest/gtest.h>

#include "mqtt/OximetryFigures.h"

using namespace hms_cpap;

TEST(OximetryFigures, TheCountIsReadBackFromTheOdiAndTheSessionLength) {
    // Last night's ring file: 102 desaturations over 27246 s.
    const double odi = 102 / (27246 / 3600.0);   // 13.48
    EXPECT_EQ(desatCountOf(odi, 27246), 102);
    EXPECT_EQ(desatCountOf(odi, 27246), desatCountOf(13.5, 27246));   // the one-decimal ODI gives the same count
}

TEST(OximetryFigures, ANightWithNoLengthOrNoOdiCountsNothing) {
    EXPECT_EQ(desatCountOf(13.5, 0), 0);
    EXPECT_EQ(desatCountOf(0.0, 28000), 0);
    EXPECT_EQ(desatCountOf(-1.0, 28000), 0);
}

TEST(OximetryFigures, TheOdiIsPublishedToOneDecimal) {
    EXPECT_EQ(odiText(13.4803), "13.5");
    EXPECT_EQ(odiText(0.0), "0.0");
    EXPECT_EQ(odiText(3.54), "3.5");
}
