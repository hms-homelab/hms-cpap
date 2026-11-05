// The computer is kept awake while a night streams over Bluetooth, and only
// then: a hold taken when the stream starts is released exactly once when it
// ends, and a hold the OS refused is never released.
#include <gtest/gtest.h>

#include "utils/KeepAwake.h"

#include <string>

using namespace hms_cpap;

namespace {

struct Counts {
    int takes = 0;
    int releases = 0;
    std::string why;
};

KeepAwake::Ops fake(Counts* c, bool grant) {
    KeepAwake::Ops ops;
    ops.name = "fake";
    ops.take = [c, grant](const std::string& why, std::string* error) {
        c->takes++;
        c->why = why;
        if (!grant && error) *error = "refused";
        return grant;
    };
    ops.release = [c] { c->releases++; };
    return ops;
}

} // namespace

TEST(KeepAwake, HeldForItsLifetimeAndReleasedOnce) {
    Counts c;
    {
        KeepAwake awake("a night streams", fake(&c, true));
        EXPECT_TRUE(awake.held());
        EXPECT_EQ(c.takes, 1);
        EXPECT_EQ(c.releases, 0);
        EXPECT_EQ(c.why, "a night streams");
    }
    EXPECT_EQ(c.releases, 1);
}

TEST(KeepAwake, AHoldTheSystemRefusedIsNeverReleased) {
    Counts c;
    {
        KeepAwake awake("a night streams", fake(&c, false));
        EXPECT_FALSE(awake.held());
    }
    EXPECT_EQ(c.takes, 1);
    EXPECT_EQ(c.releases, 0);
}

TEST(KeepAwake, NothingOnAnOsWithoutAMechanism) {
    KeepAwake awake("a night streams", KeepAwake::Ops{});
    EXPECT_FALSE(awake.held());
}

TEST(KeepAwake, TheTestBinaryNeverTouchesTheOs) {
    // hostOps() is the no-op here: only the hms_cpap executable builds the
    // OS calls in.
    KeepAwake awake("a test");
    EXPECT_FALSE(awake.held());
}
