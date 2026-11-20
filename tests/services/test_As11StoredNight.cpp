// SDD-070: which minutes of a night are filled from the machine's stored pages.

#include <gtest/gtest.h>
#include "services/As11StoredNight.h"

#include <filesystem>

namespace fs = std::filesystem;
using namespace hms_cpap;
using cpapdash::parser::AirSense11PageChunk;
using cpapdash::parser::AirSense11PageSignal;

namespace {

constexpr int64_t kMin = 60 * 1000;
constexpr int64_t kT0 = 1793595317496LL;   // the bench night's first stored sample

AirSense11PageChunk chunk(AirSense11PageSignal s, int64_t start, int64_t iv, std::vector<int32_t> raw) {
    AirSense11PageChunk c;
    c.signal = s;
    c.start_ms = start;
    c.interval_ms = iv;
    c.end_ms = start + iv * static_cast<int64_t>(raw.size() - 1);
    c.raw = std::move(raw);
    return c;
}

}  // namespace

TEST(As11StoredNight, AFortyMinuteStreamHoleIsTheOnlyGap) {
    const std::vector<StoredSpan> on = {{kT0, kT0 + 300 * kMin}};
    const std::vector<StoredSpan> streamed = {{kT0, kT0 + 100 * kMin}, {kT0 + 140 * kMin, kT0 + 300 * kMin}};
    const auto gaps = gapsToFill(on, streamed);
    ASSERT_EQ(gaps.size(), 1u);
    EXPECT_EQ(gaps[0].start_ms, kT0 + 100 * kMin);
    EXPECT_EQ(gaps[0].end_ms, kT0 + 140 * kMin);
}

TEST(As11StoredNight, AFullyStreamedNightHasNoGap) {
    const std::vector<StoredSpan> on = {{kT0, kT0 + 300 * kMin}};
    EXPECT_TRUE(gapsToFill(on, {{kT0 - kMin, kT0 + 301 * kMin}}).empty());
}

TEST(As11StoredNight, ANightNotStreamedIsOneGapPerMaskOn) {
    const std::vector<StoredSpan> on = {{kT0, kT0 + 60 * kMin}, {kT0 + 90 * kMin, kT0 + 200 * kMin}};
    const auto gaps = gapsToFill(on, {});
    ASSERT_EQ(gaps.size(), 2u);
    EXPECT_EQ(gaps[1].start_ms, kT0 + 90 * kMin);
}

TEST(As11StoredNight, AHoleShorterThanAMinuteIsAPause) {
    const std::vector<StoredSpan> on = {{kT0, kT0 + 300 * kMin}};
    const std::vector<StoredSpan> streamed = {{kT0, kT0 + 100 * kMin},
                                              {kT0 + 100 * kMin + 30000, kT0 + 300 * kMin}};
    EXPECT_TRUE(gapsToFill(on, streamed).empty());
}

TEST(As11StoredNight, RunsHoldOnlyTheGapAndSplitWhereSamplesStop) {
    using S = AirSense11PageSignal;
    // 10 samples at 160 ms, then a 2-sample hole, then 5 more.
    const auto f1 = chunk(S::Flow, kT0, 160, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    const auto f2 = chunk(S::Flow, kT0 + 12 * 160, 160, {11, 12, 13, 14, 15});
    const auto m1 = chunk(S::MaskPressure, kT0, 160, {50, 50, 50, 50, 50, 50, 50, 50, 50, 50});
    const auto m2 = chunk(S::MaskPressure, kT0 + 12 * 160, 160, {60, 60, 60, 60, 60});
    // The gap starts at the third sample.
    const std::vector<StoredSpan> gaps = {{kT0 + 2 * 160, kT0 + 100 * 160}};
    const auto runs = storedRuns({f1, f2}, {m1, m2}, gaps);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].start_ms, kT0 + 2 * 160);
    EXPECT_EQ(runs[0].a.size(), 8u);
    EXPECT_NEAR(runs[0].a.front(), 3 * 0.02, 1e-12);   // flow counts x 0.02 L/s
    EXPECT_NEAR(runs[0].b.front(), 50 * 0.2, 1e-12);   // mask counts x 0.2 cmH2O
    EXPECT_EQ(runs[1].start_ms, kT0 + 12 * 160);
    EXPECT_EQ(runs[1].a.size(), 5u);
}

TEST(As11StoredNight, APageIsKeptOnceAndReadBackInOrder) {
    const fs::path root = fs::temp_directory_path() / "as11_stored_night_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const std::vector<uint8_t> p0 = {1, 2, 3}, p1 = {4, 5};
    ASSERT_TRUE(keepStoredPage(root.string(), "20261103", "Leak0p5Hz", 0, p0));
    ASSERT_TRUE(keepStoredPage(root.string(), "20261103", "Leak0p5Hz", 1, p1));
    ASSERT_TRUE(keepStoredPage(root.string(), "20261103", "Leak0p5Hz", 0, p0));   // same bytes: no-op
    const auto pages = keptStoredPages(root.string(), "20261103", "Leak0p5Hz");
    ASSERT_EQ(pages.size(), 2u);
    EXPECT_EQ(pages[0], p0);
    EXPECT_EQ(pages[1], p1);
    EXPECT_FALSE(storedNightDone(root.string(), "20261103"));
    ASSERT_TRUE(markStoredNightDone(root.string(), "20261103"));
    EXPECT_TRUE(storedNightDone(root.string(), "20261103"));
    fs::remove_all(root, ec);
}

namespace {

constexpr int64_t kHour = 60 * kMin;

fs::path freshRoot(const char* name) {
    const fs::path root = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    return root;
}

// A machine answering pages: each signal spool has a page every 3 h from
// the night's start, its cursor the next one; the events spool one page.
// Every ask is logged as "<spool> <from>".
struct FakeSpools {
    std::vector<std::string> asks;
    std::string fail_spool;   // this spool fails on its second page
    StoredPagePull pull() {
        return [this](const std::string& spool, const std::string& from, std::vector<uint8_t>* page,
                      std::string* next, std::string* why) {
            asks.push_back(spool + " " + from);
            int n = 0;
            for (const auto& a : asks) if (a.rfind(spool + " ", 0) == 0) n++;
            if (spool == fail_spool && n == 2) { *why = "machine disconnected mid-spool"; return false; }
            *page = {static_cast<uint8_t>(n), static_cast<uint8_t>(spool.size())};
            next->clear();
            if (spool != kStoredEventsSpool) *next = storedFromDateTime(kT0 + n * 3 * kHour);
            return true;
        };
    }
};

}  // namespace

TEST(As11StoredNight, AFromDateTimeIsTheInstantInUtcToTheMillisecond) {
    EXPECT_EQ(storedFromDateTime(kT0), "2026-11-02T04:55:17.496Z");
    EXPECT_EQ(storedFromDateTime(kT0 - 496), "2026-11-02T04:55:17.000Z");
}

TEST(As11StoredNight, APullFollowsTheMachinesCursorAndStopsPastTheNight) {
    const fs::path root = freshRoot("as11_stored_pull_test");
    FakeSpools m;
    const auto r = pullStoredNight(root.string(), "20261101", {{kT0, kT0 + 8 * kHour}}, m.pull());
    ASSERT_TRUE(r.complete) << r.why;
    // Events: one page. Each signal: pages from 0 h, 3 h and 6 h; the third
    // page's cursor (9 h) is past the night's 8 h, so no fourth is asked.
    EXPECT_EQ(r.pages, 1 + 4 * 3);
    ASSERT_EQ(m.asks.size(), 13u);
    EXPECT_EQ(m.asks[0], std::string(kStoredEventsSpool) + " " + storedFromDateTime(kT0));
    EXPECT_EQ(m.asks[1], "RespiratoryFlow6p25Hz " + storedFromDateTime(kT0));
    EXPECT_EQ(m.asks[2], "RespiratoryFlow6p25Hz " + storedFromDateTime(kT0 + 3 * kHour));
    EXPECT_EQ(m.asks[3], "RespiratoryFlow6p25Hz " + storedFromDateTime(kT0 + 6 * kHour));
    EXPECT_EQ(keptStoredPages(root.string(), "20261101", "Leak0p5Hz").size(), 3u);
    EXPECT_EQ(keptStoredPages(root.string(), "20261101", kStoredEventsSpool).size(), 1u);
    // The caller marks the night done once it is written, not the pull.
    EXPECT_FALSE(storedNightDone(root.string(), "20261101"));
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(As11StoredNight, ASpoolWithNothingStoredEndsAtOnce) {
    const fs::path root = freshRoot("as11_stored_empty_test");
    int asks = 0;
    const auto r = pullStoredNight(root.string(), "20261101", {{kT0, kT0 + kHour}},
        [&](const std::string&, const std::string&, std::vector<uint8_t>* page, std::string* next, std::string*) {
            asks++;
            page->clear();   // ERROR_DATA_UNAVAILABLE: nothing, and no cursor
            next->clear();
            return true;
        });
    EXPECT_TRUE(r.complete);
    EXPECT_EQ(r.pages, 0);
    EXPECT_EQ(asks, 5);
    EXPECT_TRUE(keptStoredPages(root.string(), "20261101", "Leak0p5Hz").empty());
}

TEST(As11StoredNight, ACursorThatDoesNotMoveEndsTheSpool) {
    const fs::path root = freshRoot("as11_stored_stuck_test");
    int asks = 0;
    const auto r = pullStoredNight(root.string(), "20261101", {{kT0, kT0 + 8 * kHour}},
        [&](const std::string&, const std::string& from, std::vector<uint8_t>* page, std::string* next, std::string*) {
            asks++;
            *page = {1};
            *next = from;
            return true;
        });
    EXPECT_TRUE(r.complete);
    EXPECT_EQ(asks, 5);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(As11StoredNight, APullCutPartWayIsNotCompleteAndIsAskedAgainFromTheStart) {
    const fs::path root = freshRoot("as11_stored_cut_test");
    FakeSpools cut;
    cut.fail_spool = "MaskPressure6p25Hz";
    const auto r = pullStoredNight(root.string(), "20261101", {{kT0, kT0 + 8 * kHour}}, cut.pull());
    EXPECT_FALSE(r.complete);
    EXPECT_NE(r.why.find("MaskPressure6p25Hz"), std::string::npos) << r.why;
    EXPECT_NE(r.why.find("disconnected"), std::string::npos) << r.why;
    EXPECT_FALSE(storedNightDone(root.string(), "20261101"));
    const auto flow = keptStoredPages(root.string(), "20261101", "RespiratoryFlow6p25Hz");
    ASSERT_EQ(flow.size(), 3u);

    // The next pull asks every spool again from the night's start; the pages
    // already kept are the same bytes.
    FakeSpools again;
    const auto r2 = pullStoredNight(root.string(), "20261101", {{kT0, kT0 + 8 * kHour}}, again.pull());
    EXPECT_TRUE(r2.complete) << r2.why;
    EXPECT_EQ(again.asks[0], std::string(kStoredEventsSpool) + " " + storedFromDateTime(kT0));
    EXPECT_EQ(keptStoredPages(root.string(), "20261101", "RespiratoryFlow6p25Hz"), flow);
    EXPECT_EQ(keptStoredPages(root.string(), "20261101", "MaskPressure6p25Hz").size(), 3u);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(As11StoredNight, ANightStartsFromItsFirstMaskOnAndEndsAfterItsLast) {
    const fs::path root = freshRoot("as11_stored_two_sessions_test");
    FakeSpools m;
    // Two mask-ons, given out of order: from the earlier, to past the later.
    const auto r = pullStoredNight(root.string(), "20261101",
                                   {{kT0 + 5 * kHour, kT0 + 7 * kHour}, {kT0, kT0 + kHour}}, m.pull());
    ASSERT_TRUE(r.complete);
    EXPECT_EQ(m.asks[1], "RespiratoryFlow6p25Hz " + storedFromDateTime(kT0));
    EXPECT_EQ(keptStoredPages(root.string(), "20261101", "RespiratoryFlow6p25Hz").size(), 3u);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(As11StoredNight, EventTypesTakeTheStreamsNames) {
    EXPECT_EQ(storedEventName(3), "CentralApneaEnd");
    EXPECT_EQ(storedEventName(4), "ObstructiveApneaEnd");
    EXPECT_EQ(storedEventName(9), "");
    EXPECT_EQ(storedSpools().front(), kStoredEventsSpool);
    EXPECT_EQ(storedSpools().size(), 5u);
}
