#include <gtest/gtest.h>

#include "mqtt/OximetryFigures.h"

using namespace hms_cpap;

TEST(OximetryFigures, TheOdiIsPublishedToOneDecimal) {
    EXPECT_EQ(odiText(13.4803), "13.5");
    EXPECT_EQ(odiText(0.0), "0.0");
    EXPECT_EQ(odiText(3.54), "3.5");
}
