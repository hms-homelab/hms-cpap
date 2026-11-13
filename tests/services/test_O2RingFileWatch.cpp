#include <gtest/gtest.h>

#include "services/O2RingFileWatch.h"

TEST(O2RingFileWatch, NothingIsAskedWhileTheRingHasNotBeenWorn) {
    O2RingFileWatch w;
    for (int i = 0; i < 5; ++i) EXPECT_FALSE(w.shouldCollect(false, true));
}

TEST(O2RingFileWatch, NothingIsAskedWhileTheRingIsOnTheFinger) {
    O2RingFileWatch w;
    for (int i = 0; i < 5; ++i) EXPECT_FALSE(w.shouldCollect(true, true));
}

TEST(O2RingFileWatch, ASessionThatEndsIsAskedForOnce) {
    O2RingFileWatch w;
    EXPECT_FALSE(w.shouldCollect(true, true));
    EXPECT_TRUE(w.shouldCollect(false, true));    // the end of the session
    for (int i = 0; i < 20; ++i) EXPECT_FALSE(w.shouldCollect(false, true));
}

TEST(O2RingFileWatch, AnUnreachableRingIsNotAskedAndTheEndIsNotLost) {
    O2RingFileWatch w;
    w.shouldCollect(true, true);
    // Out of range right after the session: no answer, nothing lost.
    for (int i = 0; i < 40; ++i) EXPECT_FALSE(w.shouldCollect(false, false));
    EXPECT_TRUE(w.shouldCollect(false, true));
    EXPECT_FALSE(w.shouldCollect(false, true));
}

TEST(O2RingFileWatch, WearingItAgainStartsAnotherSession) {
    O2RingFileWatch w;
    w.shouldCollect(true, true);
    EXPECT_TRUE(w.shouldCollect(false, true));
    EXPECT_FALSE(w.shouldCollect(true, true));    // back on the finger
    EXPECT_TRUE(w.shouldCollect(false, true));    // ended again
}

TEST(O2RingFileWatch, TheRingIsGivenTimeToListItsFile) {
    EXPECT_GE(O2RingFileWatch::kSettleSeconds, 10);
}
