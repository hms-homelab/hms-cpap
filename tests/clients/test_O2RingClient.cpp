#include <gtest/gtest.h>
#include "clients/O2RingClient.h"

using namespace hms_cpap;

// Test the LiveReading struct logic directly (no HTTP needed)

TEST(O2RingLiveReading, ActiveWithValidData) {
    O2RingClient::LiveReading r;
    r.spo2 = 96;
    r.hr = 72;
    r.motion = 3;
    r.vibration = 0;
    r.active = true;
    r.valid = r.active && r.spo2 > 0 && r.hr > 0;

    EXPECT_TRUE(r.active);
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.spo2, 96);
    EXPECT_EQ(r.hr, 72);
}

TEST(O2RingLiveReading, InactiveWithRingOff) {
    // Mule responds: ring reachable but off finger
    O2RingClient::LiveReading r;
    r.spo2 = 255;
    r.hr = 255;
    r.motion = 0;
    r.vibration = 0;
    r.active = false;
    r.valid = r.active && r.spo2 > 0 && r.hr > 0;

    EXPECT_FALSE(r.active);
    EXPECT_FALSE(r.valid);
    // Reachable: spo2 != 0 (255 is the invalid marker, not zero)
    bool reachable = (r.spo2 != 0 || r.active);
    EXPECT_TRUE(reachable);
}

TEST(O2RingLiveReading, UnreachableMuleTimeout) {
    // Mule timed out — getLive() returns all zeros
    O2RingClient::LiveReading r;
    r.spo2 = 0;
    r.hr = 0;
    r.motion = 0;
    r.vibration = 0;
    r.active = false;
    r.valid = false;

    EXPECT_FALSE(r.active);
    EXPECT_FALSE(r.valid);
    bool reachable = (r.spo2 != 0 || r.active);
    EXPECT_FALSE(reachable);
}

TEST(O2RingLiveReading, ActiveButInvalidReadings) {
    // Ring on finger but sensor hasn't stabilized yet
    O2RingClient::LiveReading r;
    r.spo2 = 0;
    r.hr = 0;
    r.active = true;
    r.valid = r.active && r.spo2 > 0 && r.hr > 0;

    EXPECT_TRUE(r.active);
    EXPECT_FALSE(r.valid);
}
