// SDD-073: the desktop Sleep Bridge setup steps as data (BridgeSetup.h) and
// their words in five languages (BridgeText.h).

#include "BridgeSetup.h"
#include "BridgeText.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace cpapdash::supervisor;

namespace {

nlohmann::json status(long long started, bool online, long long stated, const char* verdict = nullptr,
                      const char* format = "") {
    nlohmann::json b = {{"sender", "TAP-S3-00000041"},
                        {"online", online},
                        {"session_started_at", started},
                        {"card_stated_at", stated},
                        {"card_format", format}};
    b["learned"] = verdict ? nlohmann::json{{"verdict", verdict}} : nlohmann::json(nullptr);
    return {{"bridges", nlohmann::json::array({b})}};
}

}  // namespace

TEST(BridgeSetup, TheAddressIsTheOneOnTheDefaultRoute) {
    const auto c = chooseBridgeAddress({"10.211.55.2", "192.168.1.20", "127.0.0.1", "169.254.3.4"}, "192.168.1.20");
    EXPECT_EQ(c.primary, "192.168.1.20");
    ASSERT_EQ(c.others.size(), 1u);
    EXPECT_EQ(c.others[0], "10.211.55.2") << "loopback and link-local are never offered";
}

TEST(BridgeSetup, WithNoDefaultRouteThePrivateAddressIsChosen) {
    const auto c = chooseBridgeAddress({"203.0.113.9", "192.168.1.20"}, "");
    EXPECT_EQ(c.primary, "192.168.1.20");
    ASSERT_EQ(c.others.size(), 1u);
    EXPECT_EQ(c.others[0], "203.0.113.9");
    EXPECT_EQ(chooseBridgeAddress({"192.168.1.20"}, "10.0.0.9").primary, "192.168.1.20")
        << "a route address this computer does not list is not trusted";
    EXPECT_TRUE(chooseBridgeAddress({"127.0.0.1"}, "").primary.empty());
}

TEST(BridgeSetup, ThePortIsMentionedOnlyWhenChanged) {
    EXPECT_FALSE(mentionPort(8894));
    EXPECT_TRUE(mentionPort(9000));
}

TEST(BridgeSetup, ACpapCardIsKnownByItsFiles) {
    EXPECT_TRUE(looksLikeCpapCard({"STR.edf", "DATALOG", "SETTINGS"}));
    EXPECT_TRUE(looksLikeCpapCard({"str.EDF", "datalog"}));
    EXPECT_TRUE(looksLikeCpapCard({"Identification.tgt"}));
    EXPECT_TRUE(looksLikeCpapCard({"Identification.json", "DATALOG"}));
    EXPECT_FALSE(looksLikeCpapCard({"DATALOG"}));
    EXPECT_FALSE(looksLikeCpapCard({"DCIM", "MISC"})) << "a camera's card";
    EXPECT_FALSE(looksLikeCpapCard({}));
}

TEST(BridgeSetup, TheBridgeArrivesByItsOwnHello) {
    const nlohmann::json hello = {{"t", "sleepbridge"}, {"event", "hello"}, {"sender", "TAP-S3-00000041"},
                                  {"status", status(100, true, 0)}};
    EXPECT_EQ(arrivedBridge(hello, 50), "TAP-S3-00000041");
    for (const char* e : {"card", "closed", "rebuilt"}) {
        nlohmann::json f = hello;
        f["event"] = e;
        EXPECT_EQ(arrivedBridge(f, 50), "") << e << " is not an arrival";
    }
    EXPECT_EQ(arrivedBridge({{"t", "cpapdash"}, {"event", "state"}}, 50), "");
}

TEST(BridgeSetup, ASnapshotCountsABridgeThatSaidHelloSinceTheStepsOpened) {
    auto snap = [](const nlohmann::json& st) {
        return nlohmann::json{{"t", "sleepbridge"}, {"event", "snapshot"}, {"sender", ""}, {"status", st}};
    };
    EXPECT_EQ(arrivedBridge(snap(status(100, true, 0)), 100), "TAP-S3-00000041");
    EXPECT_EQ(arrivedBridge(snap(status(99, true, 0)), 100), "") << "online from before: not this setup's";
    EXPECT_EQ(arrivedBridge(snap(status(120, false, 0)), 100), "") << "gone again";
    EXPECT_EQ(arrivedBridge(snap(nlohmann::json::object()), 100), "");
}

TEST(BridgeSetup, TheMachineSeesTheCardByTheBridgesCardStatement) {
    const std::string s = "TAP-S3-00000041";
    EXPECT_EQ(cardSeen(status(100, true, 0), s, 0), CardSeen::Waiting);
    EXPECT_EQ(cardSeen(status(100, true, 90), s, 90), CardSeen::Waiting) << "the statement from before the hello";
    EXPECT_EQ(cardSeen(status(100, true, 130), s, 90), CardSeen::Seen);
    EXPECT_EQ(cardSeen(status(100, true, 130, "latched"), s, 0), CardSeen::Seen);
    EXPECT_EQ(cardSeen(status(100, true, 130, "mismatch"), s, 0), CardSeen::Mismatch);
    EXPECT_EQ(cardSeen(status(100, true, 130), "TAP-S3-00000099", 0), CardSeen::Waiting);
}

TEST(BridgeSetup, ACardRecognisedAsAnotherMachinesSetsTheFormatAgain) {
    const std::string s = "TAP-S3-00000041";
    EXPECT_EQ(formatToFix(status(1, true, 0, nullptr, ""), s, "resmed"), "") << "not recognised yet";
    EXPECT_EQ(formatToFix(status(1, true, 0, nullptr, "resmed"), s, "resmed"), "");
    EXPECT_EQ(formatToFix(status(1, true, 0, nullptr, "bmc"), s, "resmed"), "bmc");
}

TEST(BridgeText, EveryWordHasAllFiveLanguages) {
    for (int k = 0; k < static_cast<int>(Txt::Count_); k++)
        for (Lang l : {Lang::En, Lang::Es, Lang::Fr, Lang::Hu, Lang::Pt}) {
            const char* w = text(static_cast<Txt>(k), l);
            ASSERT_NE(w, nullptr);
            EXPECT_GT(std::strlen(w), 0u) << "word " << k << " language " << static_cast<int>(l);
            EXPECT_EQ(std::strstr(w, "\xE2\x80\x94"), nullptr) << "no em-dashes: word " << k;
        }
    // The placeholders agree across languages.
    for (Txt k : {Txt::StepOf, Txt::CardFound, Txt::WaitFound, Txt::WaitSending, Txt::ConnectPort})
        for (Lang l : {Lang::Es, Lang::Fr, Lang::Hu, Lang::Pt})
            EXPECT_NE(std::strstr(text(k, l), "%1"), nullptr) << static_cast<int>(k);
}

TEST(BridgeText, TheOnlineLineSaysWhereThePersonIs) {
    EXPECT_EQ(streamLine("", Saved::NotYet), Txt::StreamNoToken);
    EXPECT_EQ(streamLine("off", Saved::NotYet), Txt::StreamNoToken)
        << "the ticked switch is a choice: before a save it is not sending yet, never \"off\"";
    EXPECT_EQ(streamLine("off", Saved::On), Txt::StreamNoToken);
    EXPECT_EQ(streamLine("off", Saved::Off), Txt::StreamOff) << "only once they turned it off and saved";
    EXPECT_EQ(streamLine("no_token", Saved::On), Txt::StreamNoToken);
    EXPECT_EQ(streamLine("connecting", Saved::On), Txt::StreamConnecting);
    EXPECT_EQ(streamLine("connected", Saved::NotYet), Txt::StreamConnected);
    EXPECT_EQ(streamLine("refused", Saved::On), Txt::StreamRefused);
    EXPECT_EQ(streamLine("unavailable", Saved::On), Txt::StreamOther);
    EXPECT_STREQ(text(Txt::StreamConnected, Lang::En), "Sending your nights to CpapDash.");
    EXPECT_STREQ(text(Txt::OnlineOpen, Lang::En), "Generate a token on CpapDash.com");
}

TEST(BridgeText, TheLanguageFollowsTheLocale) {
    EXPECT_EQ(langForLocale("pt_BR"), Lang::Pt);
    EXPECT_EQ(langForLocale("hu-HU"), Lang::Hu);
    EXPECT_EQ(langForLocale("FR"), Lang::Fr);
    EXPECT_EQ(langForLocale("es_MX"), Lang::Es);
    EXPECT_EQ(langForLocale("de_DE"), Lang::En);
    EXPECT_EQ(langForLocale(""), Lang::En);
    EXPECT_STREQ(text(Txt::Skip, Lang::En), "Skip, I'll do it later");
}
