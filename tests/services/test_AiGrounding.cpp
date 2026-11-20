// SDD-072 D3: every number in the model's text is one it was given.
//
// A sentence with an invented number is caught and regenerated once; one that
// fails again is kept and the summary flagged as unverified (Albin's decision:
// kept, not dropped). Rounded figures pass; a summary with no numbers passes.
#include <gtest/gtest.h>

#include "services/AiGrounding.h"

#include <string>

using namespace hms_cpap;

namespace {

const std::string kGiven =
    "Usage: 6.25 hours (78.13% of 8h target)\n"
    "AHI: 1.00 events/hour\n"
    "Apneas and hypopneas (the events counted in the AHI): 6 (obstructive apnea=0, "
    "central apnea=0, clear airway apnea=0, unclassified apnea=3, hypopnea=3)\n"
    "Leak: avg=12.35 L/min\n"
    "Night 2026-11-10\n";

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

grounding::Generate never() {
    return [](const std::string&) -> std::optional<std::string> {
        ADD_FAILURE() << "nothing failed, so nothing is regenerated";
        return std::nullopt;
    };
}

}  // namespace

TEST(AiGrounding, ASentenceWithAnInventedNumberIsCaught) {
    // The issue #78 answer: six hypopneas, when the data said three.
    EXPECT_EQ(grounding::ungrounded("You had 3 hypopneas and an AHI of 4.2.", kGiven),
              (std::vector<std::string>{"4.2"}));
    EXPECT_TRUE(grounding::ungrounded("You had 3 hypopneas.", kGiven).empty());
}

TEST(AiGrounding, RoundedNumbersPass) {
    for (const char* s : {"You used it for 6.3 hours.", "About 6 hours of use.",
                          "Leak averaged 12.4 L/min, or 12 L/min.", "Your AHI was 1.",
                          "That is 78% of the target."}) {
        EXPECT_TRUE(grounding::ungrounded(s, kGiven).empty()) << s;
    }
}

TEST(AiGrounding, MorePlacesThanTheDataHasIsNotRounding) {
    // 12.35 can be said as 12.4 or 12; 12.348 is a figure nobody gave.
    EXPECT_EQ(grounding::ungrounded("Leak was 12.348 L/min.", kGiven),
              (std::vector<std::string>{"12.348"}));
    // And a whole number that no figure rounds to is caught.
    EXPECT_EQ(grounding::ungrounded("You used it for 8 hours.", kGiven).size(), 0u)
        << "8 is in the data (the 8h target)";
    EXPECT_EQ(grounding::ungrounded("You used it for 9 hours.", kGiven),
              (std::vector<std::string>{"9"}));
}

TEST(AiGrounding, ASummaryWithNoNumbersPasses) {
    const std::string summary =
        "**Overall**\n* Therapy went well and the mask sealed.\n* Keep it up.";
    const auto r = grounding::check(summary, kGiven, never());
    EXPECT_EQ(r.text, summary);
    EXPECT_EQ(r.checked, 0);
    EXPECT_EQ(r.unverified, 0);
}

TEST(AiGrounding, ListMarkersDatesAndNamesAreNotFigures) {
    EXPECT_TRUE(grounding::ungrounded("1. SpO2 and CO2 were not recorded.", kGiven).empty());
    EXPECT_TRUE(grounding::ungrounded("On 2026-11-10 the night went well.", kGiven).empty());
    EXPECT_EQ(grounding::ungrounded("On 2026-11-19 the night went well.", kGiven),
              (std::vector<std::string>{"19"}))
        << "a date not given is caught by its parts";
}

TEST(AiGrounding, AFailingSentenceIsRegeneratedOnceAndReplaced) {
    const std::string summary =
        "**Events**\n* You had 6 hypopneas. Usage was 6.25 hours.\n* Keep going.";
    int calls = 0;
    const auto r = grounding::check(summary, kGiven, [&](const std::string& prompt) {
        ++calls;
        EXPECT_TRUE(has(prompt, "You had 6 hypopneas.")) << prompt;
        EXPECT_TRUE(has(prompt, kGiven)) << "the rewrite is asked from the same data";
        return std::optional<std::string>("* You had 3 hypopneas and 3 unclassified apneas.");
    });
    // "6 hypopneas": 6 IS in the data (the six events), so this passes; the
    // check is about numbers, not their labels. Labels are D2's rule.
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(r.text, summary);

    const std::string invented = "**Events**\n* You had 7 hypopneas. Usage was 6.25 hours.";
    const auto r2 = grounding::check(invented, kGiven, [&](const std::string&) {
        ++calls;
        return std::optional<std::string>("* You had 3 hypopneas.");
    });
    EXPECT_EQ(calls, 1) << "once per failing sentence";
    EXPECT_EQ(r2.regenerated, 1);
    EXPECT_EQ(r2.unverified, 0);
    EXPECT_EQ(r2.text, "**Events**\n* You had 3 hypopneas. Usage was 6.25 hours.")
        << "the line's own bullet kept, the model's dropped";
    EXPECT_FALSE(has(r2.text, "Unverified"));
}

TEST(AiGrounding, ASentenceThatFailsTwiceIsKeptAndTheSummaryFlagged) {
    const std::string summary = "Your AHI was 4.2 last night. Usage was 6.25 hours.";
    int calls = 0;
    const auto r = grounding::check(summary, kGiven, [&](const std::string&) {
        ++calls;
        return std::optional<std::string>("Your AHI was 5.1 last night.");
    });
    EXPECT_EQ(calls, 1) << "regenerated once, not again";
    EXPECT_EQ(r.unverified, 1);
    EXPECT_EQ(r.regenerated, 0);
    EXPECT_TRUE(has(r.text, "Your AHI was 4.2 last night.")) << "kept, not dropped: " << r.text;
    EXPECT_TRUE(has(r.text, "Usage was 6.25 hours.")) << r.text;
    EXPECT_TRUE(has(r.text, "\n\n" + grounding::unverifiedNote(1))) << r.text;
}

TEST(AiGrounding, NoReplyCountsAsASecondFailure) {
    const auto r = grounding::check("Your AHI was 4.2.", kGiven,
                                    [](const std::string&) { return std::nullopt; });
    EXPECT_EQ(r.unverified, 1);
    EXPECT_TRUE(has(r.text, "Your AHI was 4.2."));
    EXPECT_TRUE(has(r.text, "Unverified: 1 sentence"));
}
