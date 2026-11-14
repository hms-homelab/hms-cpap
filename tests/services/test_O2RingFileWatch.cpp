#include <gtest/gtest.h>

#include "services/O2RingFileWatch.h"

TEST(O2RingFileWatch, NothingIsAskedWhileTheRingHasNotBeenWorn) {
    O2RingFileWatch w;
    for (int i = 0; i < 5; ++i) EXPECT_FALSE(w.shouldCollect(false, true));
    EXPECT_FALSE(w.owed());
}

TEST(O2RingFileWatch, NothingIsAskedWhileTheRingIsOnTheFinger) {
    O2RingFileWatch w;
    for (int i = 0; i < 5; ++i) EXPECT_FALSE(w.shouldCollect(true, true));
}

TEST(O2RingFileWatch, TheEndOfASessionOwesTheFileAndTheSamePollAsksForIt) {
    O2RingFileWatch w;
    EXPECT_FALSE(w.shouldCollect(true, true));
    EXPECT_TRUE(w.shouldCollect(false, true));    // the end of the session
    EXPECT_TRUE(w.owed());
    EXPECT_TRUE(w.firstTry());
}

TEST(O2RingFileWatch, AFileThatIsNotThereYetIsAskedForOnTheNextPolls) {
    O2RingFileWatch w;
    w.shouldCollect(true, true);
    EXPECT_TRUE(w.shouldCollect(false, true));
    w.collected(false);                           // nothing listed yet
    EXPECT_TRUE(w.shouldCollect(false, true));    // the next poll asks again
    EXPECT_FALSE(w.firstTry());
    w.collected(false);
    EXPECT_TRUE(w.shouldCollect(false, true));
    EXPECT_EQ(w.tries(), 3);
}

TEST(O2RingFileWatch, ItGivesUpAfterThreeAsksUntilTheNextSessionEnds) {
    O2RingFileWatch w;
    w.shouldCollect(true, true);
    for (int i = 0; i < O2RingFileWatch::kMaxTries; ++i) {
        ASSERT_TRUE(w.shouldCollect(false, true));
        w.collected(false);
    }
    for (int i = 0; i < 20; ++i) EXPECT_FALSE(w.shouldCollect(false, true));
    EXPECT_FALSE(w.owed());
    // Worn and taken off again: owed afresh, with a fresh three asks.
    EXPECT_FALSE(w.shouldCollect(true, true));
    EXPECT_TRUE(w.shouldCollect(false, true));
    EXPECT_TRUE(w.firstTry());
}

TEST(O2RingFileWatch, ANewFileSavedClearsTheDebt) {
    O2RingFileWatch w;
    w.shouldCollect(true, true);
    ASSERT_TRUE(w.shouldCollect(false, true));
    w.collected(true);
    EXPECT_FALSE(w.owed());
    for (int i = 0; i < 20; ++i) EXPECT_FALSE(w.shouldCollect(false, true));
}

TEST(O2RingFileWatch, AnUnreachableRingIsNotAskedAndTheEndIsNotLost) {
    O2RingFileWatch w;
    w.shouldCollect(true, true);
    // Out of range right after the session: no answer, nothing lost.
    for (int i = 0; i < 40; ++i) EXPECT_FALSE(w.shouldCollect(false, false));
    EXPECT_TRUE(w.shouldCollect(false, true));
    EXPECT_TRUE(w.firstTry());
}

TEST(O2RingFileWatch, AnUnreachablePollDoesNotSpendAnAsk) {
    O2RingFileWatch w;
    w.shouldCollect(true, true);
    ASSERT_TRUE(w.shouldCollect(false, true));
    w.collected(false);
    for (int i = 0; i < 10; ++i) EXPECT_FALSE(w.shouldCollect(false, false));
    EXPECT_TRUE(w.shouldCollect(false, true));
    EXPECT_EQ(w.tries(), 2);
}

TEST(O2RingFileWatch, WearingItAgainWhileTheFileIsOwedStartsAnotherSession) {
    O2RingFileWatch w;
    w.shouldCollect(true, true);
    ASSERT_TRUE(w.shouldCollect(false, true));
    w.collected(false);
    EXPECT_FALSE(w.shouldCollect(true, true));    // back on the finger: no asking
    EXPECT_TRUE(w.shouldCollect(false, true));    // ended again: a fresh debt
    EXPECT_TRUE(w.firstTry());
}
