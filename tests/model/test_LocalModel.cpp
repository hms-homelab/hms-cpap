//
// SDD-013: the local sleep model's harness, with no model file.
//
// The fact block, the generation contract (validator, one retry at a lower
// temperature, the template), the model file's download / load-from-file
// flow with its hash check, and the nightly job writing a summary with source
// "local" when a night closes. A scripted ILocalModel stands in for the model.
//
#include "AiFactsEngines.h"

#include "model/LocalModel.h"
#include "model/LocalModelService.h"
#include "model/ModelFile.h"
#include "model/NarrationService.h"
#include "services/UpdateService.h"
#include "utils/AppConfig.h"

#include <algorithm>
#include <deque>
#include <fstream>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap;
using namespace hms_cpap::model;
using namespace ai_facts_test;
namespace fs = std::filesystem;

namespace {

/// Answers from a script, and remembers what it was asked.
class ScriptedModel : public ILocalModel {
public:
    std::deque<std::optional<std::string>> replies;
    std::vector<double> temperatures;
    std::vector<std::string> prompts;
    std::vector<std::string>* prompt_sink = nullptr;   ///< also told each prompt
    bool is_ready = true;

    bool ready() const override { return is_ready; }
    std::string whyNotReady() const override { return is_ready ? "" : "scripted: not ready"; }
    std::optional<std::string> generate(const std::string& prompt, const GenerateParams& p) override {
        prompts.push_back(prompt);
        if (prompt_sink) prompt_sink->push_back(prompt);
        temperatures.push_back(p.temperature);
        if (replies.empty()) return std::nullopt;
        auto r = replies.front();
        replies.pop_front();
        return r;
    }
};

NightFacts aNight() {
    NightFacts n;
    n.sleep_day = "2099-07-01";
    n.index = 2.5;
    n.obstructive = 6;
    n.central = 3;
    n.hypopnea = 6;
    n.total = 15;
    n.usage_hours = 6.25;
    n.leak_p95 = 18.4;
    n.pressure_p95 = 11.2;
    return n;
}

std::string writeFile(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
    return p.string();
}

std::string tempDir(const std::string& tag) {
    const auto d = fs::temp_directory_path() /
                   ("hms_sdd013_" + tag + "_" + std::to_string(::getpid()) + "_" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(d);
    fs::create_directories(d);
    return d.string();
}

}  // namespace

// -- The fact block ------------------------------------------------------------

TEST(LocalModelFactBlock, IsTheNightsDayThenTheSharedRenderingAndStable) {
    const auto n = aNight();
    const auto block = factBlock(n);
    EXPECT_EQ(block.rfind("Night: 2099-07-01\n", 0), 0u) << block;
    EXPECT_EQ(block, "Night: 2099-07-01\n" + night_facts::renderNight(n))
        << "the block is NightFacts' own rendering, not a second one";
    EXPECT_EQ(block, factBlock(aNight())) << "the same night gives the same block";
}

// -- The night the model is given ----------------------------------------------

TEST(LocalModelNight, TheStrNamesTheModeAndWithoutOneAStoredZeroIsNoMode) {
    SessionMetrics m;
    m.sleep_day = "2099-07-01";
    m.usage_hours = 6.0;
    m.therapy_mode = 0;   // what every ResMed session stores
    auto mode = [&](STRDailyRecord::Family family, int str_mode) {
        STRDailyRecord s;
        s.family = family;
        s.mode = str_mode;
        return modelNight(m, &s, std::nullopt).therapy_mode.value_or("");
    };
    EXPECT_EQ(mode(STRDailyRecord::Family::AutoSet, 1), "APAP");
    EXPECT_EQ(mode(STRDailyRecord::Family::BiLevel, 8), "VAuto");
    EXPECT_EQ(mode(STRDailyRecord::Family::Asv, 7), "ASV (Fixed EPAP)");
    EXPECT_EQ(mode(STRDailyRecord::Family::Cpap, 0), "CPAP");

    const auto none = modelNight(m, nullptr, std::nullopt);
    EXPECT_FALSE(none.therapy_mode) << "0 with no STR to name it is no mode, not CPAP";
    EXPECT_EQ(factBlock(none).find("Therapy mode"), std::string::npos);

    m.therapy_mode = 1;   // a parser that does fill the session's mode keeps it
    EXPECT_EQ(modelNight(m, nullptr, std::nullopt).therapy_mode.value_or(""), "APAP");
}

// -- The validator -------------------------------------------------------------

TEST(LocalModelValidator, AcceptsFiguresFromTheBlockAndRejectsInventedTransposedAndWrongDates) {
    const auto block = factBlock(aNight());
    EXPECT_TRUE(violations("On 2099-07-01 you used it for 6.25 hours with an AHI of 2.5.", block).empty());
    EXPECT_TRUE(violations("A good night with few events.", block).empty()) << "no numbers passes";
    EXPECT_TRUE(violations("About 6.3 hours of use.", block).empty()) << "a figure said with fewer places";

    EXPECT_FALSE(violations("Your AHI was 4.1.", block).empty()) << "an invented figure";
    EXPECT_FALSE(violations("You used it for 6.52 hours.", block).empty()) << "a transposed figure";
    const auto wrong_day = violations("On 2099-07-02 your AHI was 2.5.", block);
    ASSERT_FALSE(wrong_day.empty()) << "a wrong date";
    EXPECT_NE(std::find(wrong_day.begin(), wrong_day.end(), "2099-07-02"), wrong_day.end());
}

// Item 8 (Albin 2026-11-21, "keep 8h in the summary"): the local narrator's
// block keeps "% of 8h target", the line narrator-v1 was trained on (given a
// 4h line it still judged against 8 h); the dashboard's usage target is its
// own setting. A text that contradicts met / not met against the BLOCK's
// target never passes, so the model's normal wording does.
TEST(LocalModelValidator, TheBlockStatesEightHoursAndTheWordingMustAgreeWithIt) {
    NightFacts n = aNight();
    n.usage_hours = 4.5;
    n.usage_percent = 56.25;
    const auto block = factBlock(n);
    EXPECT_NE(block.find("Usage: 4.50 hours (56.25% of 8h target)"), std::string::npos) << block;

    EXPECT_TRUE(violations("You used it for 4.50 hours, well short of your target.", block).empty())
        << "4.50 h is short of 8 h: the model's normal text passes";
    EXPECT_TRUE(violations("You used it for 4.50 hours.", block).empty());
    const auto met = violations("4.50 hours: target met.", block);
    ASSERT_FALSE(met.empty()) << "4.50 h is under 8 h";
    EXPECT_NE(met.back().find("8h target"), std::string::npos) << met.back();
    EXPECT_FALSE(violations("You reached your target with 4.50 hours.", block).empty());

    n.usage_hours = 8.35;
    n.usage_percent = 104.38;
    const auto over = factBlock(n);
    EXPECT_TRUE(violations("8.35 hours, past your target.", over).empty());
    EXPECT_FALSE(violations("8.35 hours, just short of your target.", over).empty()) << "8.35 h meets 8 h";
}

// KNOWN GAP, disabled until Albin decides (2026-11-21): the check asks only
// whether a number appears somewhere in the block, not whether it is the
// figure the sentence names. Seen from the narrator on a local-mode-like
// night: "1 central apnea" where the facts say 3, passed because a "1" is
// elsewhere in the block. This test states the wanted behaviour and fails
// today (run it with --gtest_also_run_disabled_tests).
TEST(LocalModelValidator, DISABLED_ACountGivenToTheWrongEventTypeIsRejected) {
    const std::string block = "Night: 2099-07-01\nCentral apneas: 3\nRERAs: 1\n";
    EXPECT_FALSE(violations("You had 1 central apnea.", block).empty())
        << "3 central apneas said as 1, the 1 being the RERA count";
}

// -- The generation contract ---------------------------------------------------

TEST(LocalModelNarration, AGroundedFirstAnswerIsKept) {
    ScriptedModel m;
    m.replies.push_back("You used it for 6.25 hours and your AHI was 2.50.");
    const auto r = narrate(aNight(), m);
    ASSERT_TRUE(r.written);
    EXPECT_EQ(r.how, "model");
    EXPECT_EQ(r.text, "You used it for 6.25 hours and your AHI was 2.50.");
    ASSERT_EQ(m.prompts.size(), 1u);
    EXPECT_EQ(m.prompts[0], factBlock(aNight())) << "the model is given the fact block, nothing else";
    EXPECT_DOUBLE_EQ(m.temperatures[0], 0.0) << "greedy, the decode narrator-v1 was measured with";
}

TEST(LocalModelNarration, ARejectedAnswerGivesTheTemplateAtOnceAndSaysWhy) {
    ScriptedModel m;
    m.replies.push_back("On 2099-07-03 you slept well.");
    m.replies.push_back("Your AHI was 2.5.");   // never asked for: no retry
    const auto r = narrate(aNight(), m);
    ASSERT_TRUE(r.written);
    EXPECT_EQ(r.how, "template");
    EXPECT_EQ(r.text, templateSummary(aNight()));
    EXPECT_NE(r.why.find("2099-07-03"), std::string::npos) << r.why;
    EXPECT_EQ(m.prompts.size(), 1u) << "a greedy retry would repeat the same text";
}

TEST(LocalModelNarration, NoTextAlsoGivesTheTemplate) {
    ScriptedModel m;   // no replies: generate() gives nothing
    const auto r = narrate(aNight(), m);
    ASSERT_TRUE(r.written);
    EXPECT_EQ(r.how, "template");
}

TEST(LocalModelNarration, AModelThatIsNotReadyWritesNothing) {
    ScriptedModel m;
    m.is_ready = false;
    const auto r = narrate(aNight(), m);
    EXPECT_FALSE(r.written);
    EXPECT_TRUE(m.prompts.empty());
    EXPECT_EQ(r.why, "scripted: not ready");
}

TEST(LocalModelNarration, TheTemplatePassesItsOwnCheckGradedAndUngraded) {
    auto n = aNight();
    n.oximetry = NightOximetryFacts{};
    n.oximetry->avg_spo2 = 94.5;
    n.oximetry->min_spo2 = 86;
    std::string bad;
    for (const auto& v : violations(templateSummary(n), factBlock(n))) bad += " " + v;
    EXPECT_EQ(bad, "") << templateSummary(n) << "\n---\n" << factBlock(n);
    // The renderer shows a 95th leak only beside the average or the maximum,
    // so the block has none here, and the template does not say one.
    EXPECT_EQ(templateSummary(n).find("Leak"), std::string::npos);
    EXPECT_NE(templateSummary(n).find("94.50%"), std::string::npos);

    NightFacts u;
    u.sleep_day = "2099-07-02";
    u.graded = false;
    u.index = 4.0;
    u.total = 24;
    const auto t = templateSummary(u);
    EXPECT_TRUE(violations(t, factBlock(u)).empty()) << t;
    EXPECT_EQ(t.find("AHI was"), std::string::npos) << "an Ungraded night has no AHI (SDD-024)";
}

// -- The runner ----------------------------------------------------------------

// The runner itself, with and without a model file, is in test_GgufModel.cpp.

// -- The model file --------------------------------------------------------------

TEST(LocalModelFile, ThePinIsThePublishedNarrator) {
    EXPECT_STREQ(kNarratorModelVersion, "narrator-v1");
    EXPECT_STREQ(kNarratorModelUrl,
                 "https://github.com/hms-homelab/hms-cpap/releases/download/narrator-v1/narrator-v1-q4_k_m.gguf");
    EXPECT_STREQ(kNarratorModelSha256, "a0d99c56137466735fa0371dd64e6bdb908b2e33cd250632670534734af48d00");
    const auto pin = pinnedModel();
    EXPECT_EQ(verifyRefusal(pin), "");
    EXPECT_EQ(downloadRefusal(pin), "");
}

TEST(LocalModelFile, WithNoModelPublishedEnablingIsRefusedWithTheReason) {
    const Pin none{kNarratorModelVersion, "", ""};
    EXPECT_NE(verifyRefusal(none).find("No local sleep model has been published yet"), std::string::npos);
    EXPECT_NE(downloadRefusal(none).find("No local sleep model has been published yet"), std::string::npos);
}

TEST(LocalModelFile, AFileWithThePinnedHashIsPutInPlaceAndAnotherIsNot) {
    const auto dir = tempDir("file");
    const auto good = writeFile(fs::path(dir) / "in" / "good.gguf", "the published model");
    const auto bad = writeFile(fs::path(dir) / "in" / "bad.gguf", "something else");
    const Pin pin{"narrator-test-v1", "", update::sha256File(good)};
    const auto models = (fs::path(dir) / "models").string();

    const auto no = installFromFile(bad, models, pin);
    EXPECT_FALSE(no.ok);
    EXPECT_NE(no.error.find("not the published model"), std::string::npos) << no.error;
    EXPECT_FALSE(fs::exists(modelPath(models, pin)));
    EXPECT_FALSE(installedAndVerified(models, pin));

    const auto yes = installFromFile(good, models, pin);
    ASSERT_TRUE(yes.ok) << yes.error;
    EXPECT_EQ(yes.path, (fs::path(models) / "narrator-test-v1" / "model.gguf").string());
    EXPECT_TRUE(installedAndVerified(models, pin));
    EXPECT_TRUE(fs::exists(good)) << "the source is left where it was";
    fs::remove_all(dir);
}

TEST(LocalModelFile, ADownloadIsCheckedAndAMismatchIsDeleted) {
    const auto dir = tempDir("dl");
    const std::string bytes = "the published model";
    const auto ref = writeFile(fs::path(dir) / "ref.gguf", bytes);
    const Pin pin{"narrator-test-v1", "https://models.example/narrator.gguf", update::sha256File(ref)};
    const auto models = (fs::path(dir) / "models").string();

    std::vector<std::string> asked;
    const auto wrong = downloadModel(models, pin, [&](const std::string& url, const std::string& path) {
        asked.push_back(url);
        std::ofstream(path, std::ios::binary) << "tampered";
        return true;
    });
    EXPECT_FALSE(wrong.ok);
    EXPECT_FALSE(fs::exists(modelPath(models, pin)));
    EXPECT_FALSE(fs::exists(modelPath(models, pin) + ".part")) << "a mismatch is deleted";
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked[0], pin.url) << "only the pinned URL is asked";

    const auto failed = downloadModel(models, pin, [](const std::string&, const std::string&) { return false; });
    EXPECT_FALSE(failed.ok);
    EXPECT_NE(failed.error.find("load the model from a file"), std::string::npos) << failed.error;

    const auto ok = downloadModel(models, pin, [&](const std::string&, const std::string& path) {
        std::ofstream(path, std::ios::binary) << bytes;
        return true;
    });
    ASSERT_TRUE(ok.ok) << ok.error;
    EXPECT_TRUE(installedAndVerified(models, pin));

    const Pin plain{"v", "http://models.example/x.gguf", pin.sha256};
    EXPECT_FALSE(downloadRefusal(plain).empty()) << "https only";
    fs::remove_all(dir);
}

// -- The nightly job, on a real database ----------------------------------------

class LocalModelServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = tempDir("svc");
        db_path_ = (fs::path(dir_) / "cpap.db").string();
        std::string skip;
        db_ = openEngine(Engine::SQLite, db_path_, skip);
        ASSERT_TRUE(db_) << skip;
        config_.local_model.model_dir = (fs::path(dir_) / "models").string();
        // The switch as a user left it off: these tests turn it on through
        // Settings. The default (D-701-2) has its own tests below.
        config_.local_model.enabled = false;
        config_.local_model.set_by_user = true;
        model_bytes_ = "the published model";
        ref_ = writeFile(fs::path(dir_) / "ref.gguf", model_bytes_);
    }
    void TearDown() override {
        svc_.reset();
        db_.reset();
        fs::remove_all(dir_);
    }

    Pin testPin() const { return {"narrator-test-v1", "https://models.example/n.gguf", update::sha256File(ref_)}; }

    /// The service with [pin]; every model it makes answers [reply].
    void make(const Pin& pin, std::optional<std::string> reply = std::string("Your AHI was 1.00."),
              bool ready = true) {
        LocalModelService::Deps d;
        d.db = db_;
        d.config = &config_;
        d.device_id = device_;
        d.pin = pin;
        d.bundled_dirs = bundled_dirs_;
        d.make_model = [reply, ready, this](const std::string& path) {
            made_paths_.push_back(path);
            auto m = std::make_unique<ScriptedModel>();
            m->is_ready = ready;
            m->prompt_sink = &prompts_;
            m->replies.push_back(reply);
            m->replies.push_back(reply);
            return std::unique_ptr<ILocalModel>(std::move(m));
        };
        d.download = [this](const std::string&, const std::string& path) {
            ++downloads_;
            std::ofstream(path, std::ios::binary) << model_bytes_;
            return true;
        };
        svc_ = std::make_unique<LocalModelService>(std::move(d));
    }

    Json::Value summaries() {
        return db_->executeQuery(
            "SELECT period, range_start, summary_text, source FROM cpap_summaries WHERE device_id = ?",
            {device_});
    }

    std::string dir_, db_path_, ref_, model_bytes_;
    std::string device_ = "sdd013_dev";
    std::shared_ptr<IDatabase> db_;
    AppConfig config_;
    std::unique_ptr<LocalModelService> svc_;
    std::vector<std::string> made_paths_;
    std::vector<std::string> prompts_;   ///< every prompt any model was given
    std::vector<std::string> bundled_dirs_;   ///< none unless a test puts one there
    int downloads_ = 0;
};

TEST_F(LocalModelServiceTest, WithNoModelPublishedTheSwitchIsRefusedAndNothingChanges) {
    make(Pin{kNarratorModelVersion, "", ""});
    const auto why = svc_->requestEnable();
    EXPECT_NE(why.find("No local sleep model has been published yet"), std::string::npos) << why;
    EXPECT_FALSE(config_.local_model.enabled);
    EXPECT_EQ(downloads_, 0);
    const auto s = svc_->status();
    EXPECT_EQ(s["state"].asString(), "not_published");
    EXPECT_FALSE(s["published"].asBool());
    EXPECT_FALSE(s["enabled"].asBool());

    const auto upload = writeFile(fs::path(dir_) / "upload.gguf", model_bytes_);
    EXPECT_FALSE(svc_->installFile(upload).empty()) << "a file cannot be checked against nothing";
    EXPECT_FALSE(fs::exists(upload)) << "the refused upload is not kept";
}

TEST_F(LocalModelServiceTest, TheFirstEnableDownloadsChecksAndTurnsItOn) {
    make(testPin());
    EXPECT_EQ(svc_->requestEnable(), "");
    EXPECT_EQ(svc_->status()["state"].asString(), "downloading");
    EXPECT_FALSE(config_.local_model.enabled) << "on only once the model is in place";

    svc_->handle(LocalModelService::Event::Download);
    EXPECT_EQ(downloads_, 1);
    EXPECT_TRUE(config_.local_model.enabled);
    EXPECT_TRUE(installedAndVerified(config_.local_model.model_dir, testPin()));
    EXPECT_EQ(svc_->status()["state"].asString(), "ready");

    // Off and on again: the model is there, nothing is downloaded.
    svc_->disable();
    EXPECT_FALSE(config_.local_model.enabled);
    EXPECT_EQ(svc_->requestEnable(), "");
    EXPECT_TRUE(config_.local_model.enabled);
    EXPECT_EQ(downloads_, 1);
}

TEST_F(LocalModelServiceTest, LoadingAModelFromAFileChecksItAndTurnsItOn) {
    make(testPin());
    const auto wrong = writeFile(fs::path(dir_) / "upload1.gguf", "not it");
    ASSERT_EQ(svc_->installFile(wrong), "");
    svc_->handle(LocalModelService::Event::InstallFile, wrong);
    EXPECT_FALSE(config_.local_model.enabled);
    EXPECT_EQ(svc_->status()["state"].asString(), "error");
    EXPECT_NE(svc_->status()["message"].asString().find("not the published model"), std::string::npos);
    EXPECT_FALSE(fs::exists(wrong)) << "the upload is not kept";

    const auto right = writeFile(fs::path(dir_) / "upload2.gguf", model_bytes_);
    ASSERT_EQ(svc_->installFile(right), "");
    svc_->handle(LocalModelService::Event::InstallFile, right);
    EXPECT_TRUE(config_.local_model.enabled);
    EXPECT_EQ(svc_->status()["state"].asString(), "ready");
    EXPECT_EQ(downloads_, 0) << "an offline install needs no network";
}

// An open Settings page is told the outcome (it kept "Checking the model
// file..." until a reload): once per loaded file, refused or put in place.
TEST_F(LocalModelServiceTest, ALoadedFileTellsTheOutcomeToTheListener) {
    make(testPin());
    std::vector<Json::Value> told;
    svc_->setStateListener([&told](const Json::Value& v) { told.push_back(v); });
    const auto wrong = writeFile(fs::path(dir_) / "upload1.gguf", "not it");
    ASSERT_EQ(svc_->installFile(wrong), "");
    EXPECT_TRUE(told.empty()) << "nothing to say while it is being checked";
    svc_->handle(LocalModelService::Event::InstallFile, wrong);
    ASSERT_EQ(told.size(), 1u);
    EXPECT_EQ(told[0]["state"].asString(), "error");
    EXPECT_FALSE(told[0]["busy"].asBool());

    const auto right = writeFile(fs::path(dir_) / "upload2.gguf", model_bytes_);
    ASSERT_EQ(svc_->installFile(right), "");
    svc_->handle(LocalModelService::Event::InstallFile, right);
    ASSERT_EQ(told.size(), 2u);
    EXPECT_NE(told[1]["state"].asString(), "installing");
    EXPECT_EQ(told[1]["state"].asString(), svc_->status()["state"].asString());
}

TEST_F(LocalModelServiceTest, AClosedNightIsSummarizedWithSourceLocal) {
    NightSpec n;
    n.hypopneas = 6;   // 6 events over 6 hours: AHI 1.00
    saveNight(*db_, device_, localAt(2099, 7, 1, 23), n);

    make(testPin());
    ASSERT_EQ(svc_->requestEnable(), "");
    svc_->handle(LocalModelService::Event::Download);
    ASSERT_TRUE(config_.local_model.enabled);

    svc_->handle(LocalModelService::Event::NightClosed, "20990701");
    const auto rows = summaries();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["period"].asString(), "daily");
    EXPECT_EQ(rows[0]["range_start"].asString(), "2099-07-01");
    EXPECT_EQ(rows[0]["summary_text"].asString(), "Your AHI was 1.00.");
    EXPECT_EQ(rows[0]["source"].asString(), "local");
    ASSERT_FALSE(made_paths_.empty());
    EXPECT_EQ(made_paths_.back(), modelPath(config_.local_model.model_dir, testPin()));

    const auto last = svc_->status()["last"];
    EXPECT_EQ(last["night"].asString(), "2099-07-01");
    EXPECT_EQ(last["how"].asString(), "model");
}

TEST_F(LocalModelServiceTest, TheClosedNightsStrRowReachesTheModel) {
    NightSpec n;
    n.hypopneas = 6;
    saveNight(*db_, device_, localAt(2099, 7, 1, 23), n);
    make(testPin());
    ASSERT_EQ(svc_->requestEnable(), "");
    svc_->handle(LocalModelService::Event::Download);

    STRDailyRecord str;
    str.family = STRDailyRecord::Family::AutoSet;
    str.mode = 1;
    str.ahi = 1.0;
    str.mask_events = 2;
    str.leak_95 = 7.2;
    str.mask_press_95 = 10.4;
    svc_->handle(LocalModelService::Event::NightClosed, "20990701", str);
    ASSERT_FALSE(prompts_.empty());
    EXPECT_NE(prompts_.back().find("Therapy mode: APAP"), std::string::npos) << prompts_.back();
    EXPECT_NE(prompts_.back().find("ResMed official daily summary"), std::string::npos);
    EXPECT_NE(prompts_.back().find("95th leak: 7.20 L/min"), std::string::npos);

    // A night the card has no STR row for: no STR block, and no "CPAP".
    svc_->handle(LocalModelService::Event::NightClosed, "20990701");
    EXPECT_EQ(prompts_.back().find("ResMed official daily summary"), std::string::npos);
    EXPECT_EQ(prompts_.back().find("Therapy mode: CPAP"), std::string::npos) << prompts_.back();
}

TEST_F(LocalModelServiceTest, NothingIsWrittenWhenOffOrWhenTheModelCannotRun) {
    NightSpec n;
    n.hypopneas = 6;
    saveNight(*db_, device_, localAt(2099, 7, 1, 23), n);

    make(testPin());
    svc_->handle(LocalModelService::Event::NightClosed, "20990701");
    EXPECT_EQ(summaries().size(), 0u) << "off: the close is not acted on";

    make(testPin(), std::string("Your AHI was 1.00."), /*ready=*/false);
    ASSERT_EQ(svc_->requestEnable(), "");
    svc_->handle(LocalModelService::Event::Download);
    svc_->handle(LocalModelService::Event::NightClosed, "20990701");
    EXPECT_EQ(summaries().size(), 0u) << "a model that cannot run writes no summary";
    EXPECT_EQ(svc_->status()["state"].asString(), "no_runtime");
    EXPECT_EQ(svc_->status()["last"]["why"].asString(), "scripted: not ready");
}

TEST_F(LocalModelServiceTest, ANightNotInTheDatabaseIsLeftAndSaidSo) {
    make(testPin());
    ASSERT_EQ(svc_->requestEnable(), "");
    svc_->handle(LocalModelService::Event::Download);
    svc_->handle(LocalModelService::Event::NightClosed, "20990705");
    EXPECT_EQ(summaries().size(), 0u);
    EXPECT_NE(svc_->status()["last"]["why"].asString().find("2099-07-05"), std::string::npos);
}

// -- D-701-3: a night announced again is not summarized twice -------------------

TEST_F(LocalModelServiceTest, ANightAnnouncedAgainWithTheSameFactsIsNotSummarizedTwice) {
    NightSpec n;
    n.hypopneas = 6;
    saveNight(*db_, device_, localAt(2099, 7, 1, 23), n);
    make(testPin());
    ASSERT_EQ(svc_->requestEnable(), "");
    svc_->handle(LocalModelService::Event::Download);

    svc_->handle(LocalModelService::Event::NightClosed, "20990701");
    ASSERT_EQ(summaries().size(), 1u);
    const auto asked = prompts_.size();
    const auto block = storedFactBlock(*db_, device_, "2099-07-01");
    EXPECT_EQ(block.rfind("Night: 2099-07-01\n", 0), 0u) << "the row keeps the block it was written from";
    EXPECT_EQ(block, prompts_.back()) << "the very block the model was given";

    // A re-read, a second upload of the same card: the same facts.
    svc_->handle(LocalModelService::Event::NightClosed, "20990701");
    EXPECT_EQ(prompts_.size(), asked) << "the model is not run again";
    EXPECT_EQ(summaries().size(), 1u);
    EXPECT_EQ(svc_->status()["last_skipped"].asString(), "2099-07-01");
    EXPECT_EQ(svc_->status()["last"]["how"].asString(), "model") << "Settings still shows the night written";

    // The STR arrives: the facts changed, so the night is written again, and
    // the new summary replaces the old one.
    STRDailyRecord str;
    str.family = STRDailyRecord::Family::AutoSet;
    str.mode = 1;
    str.ahi = 1.0;
    svc_->handle(LocalModelService::Event::NightClosed, "20990701", str);
    EXPECT_EQ(prompts_.size(), asked + 1);
    EXPECT_EQ(svc_->status()["last"]["how"].asString(), "model");
    const auto rows = summaries();
    ASSERT_EQ(rows.size(), 1u) << "a night has one local summary";
    EXPECT_NE(storedFactBlock(*db_, device_, "2099-07-01"), block);
    EXPECT_NE(storedFactBlock(*db_, device_, "2099-07-01").find("Therapy mode: APAP"), std::string::npos);
}

TEST_F(LocalModelServiceTest, ALocalSummaryFromBeforeTheFactsColumnIsWrittenAgainAndReplaced) {
    NightSpec n;
    n.hypopneas = 6;
    saveNight(*db_, device_, localAt(2099, 7, 1, 23), n);
    // What 7.0.0 stored: a local summary with no fact block.
    ASSERT_TRUE(db_->saveSummary(device_, "daily", "2099-07-01", "2099-07-01", 1, 1.0, 6.0, 100.0,
                                 "Written by 7.0.0.", kLocalSummarySource));
    // And an LLM summary of the same night, which is not the model's to touch.
    ASSERT_TRUE(db_->saveSummary(device_, "daily", "2099-07-01", "2099-07-01", 1, 1.0, 6.0, 100.0,
                                 "From the configured LLM."));
    make(testPin());
    ASSERT_EQ(svc_->requestEnable(), "");
    svc_->handle(LocalModelService::Event::Download);
    svc_->handle(LocalModelService::Event::NightClosed, "20990701");

    const auto rows = summaries();
    ASSERT_EQ(rows.size(), 2u);
    int local = 0;
    for (const auto& r : rows) {
        if (r["source"].asString() != "local") {
            EXPECT_EQ(r["summary_text"].asString(), "From the configured LLM.");
            continue;
        }
        ++local;
        EXPECT_EQ(r["summary_text"].asString(), "Your AHI was 1.00.");
    }
    EXPECT_EQ(local, 1);
}

// -- D-701-1 / D-701-2: the bundled model, on by default --------------------------

TEST_F(LocalModelServiceTest, ABundledModelIsFoundAndAFreshInstallComesUpOnWithNoDownload) {
    AppConfig fresh;
    fresh.local_model.model_dir = config_.local_model.model_dir;
    config_ = fresh;
    ASSERT_TRUE(config_.local_model.enabled) << "on is the default";
    ASSERT_FALSE(config_.local_model.set_by_user);

    const auto bundle = (fs::path(dir_) / "app" / "models").string();
    writeFile(fs::path(modelPath(bundle, testPin())), model_bytes_);
    bundled_dirs_ = {(fs::path(dir_) / "nothing-here").string(), bundle};
    make(testPin());
    EXPECT_FALSE(svc_->status()["enabled"].asBool()) << "not on before the model is found";
    EXPECT_EQ(svc_->status()["state"].asString(), "installing");

    svc_->handle(LocalModelService::Event::Startup);
    const auto s = svc_->status();
    EXPECT_TRUE(s["enabled"].asBool());
    EXPECT_TRUE(s["bundled"].asBool());
    EXPECT_TRUE(s["installed"].asBool());
    EXPECT_EQ(s["state"].asString(), "ready");
    EXPECT_EQ(downloads_, 0);
    EXPECT_FALSE(fs::exists(modelPath(config_.local_model.model_dir, testPin())))
        << "the bundled copy is read where it is, never copied";
    ASSERT_FALSE(made_paths_.empty());
    EXPECT_EQ(made_paths_.back(), modelPath(bundle, testPin()));
    EXPECT_FALSE(config_.local_model.set_by_user) << "a default is not the user's choice";

    // And a night that lands is summarized from it.
    NightSpec n;
    n.hypopneas = 6;
    saveNight(*db_, device_, localAt(2099, 7, 1, 23), n);
    svc_->handle(LocalModelService::Event::NightClosed, "20990701");
    EXPECT_EQ(summaries().size(), 1u);
    EXPECT_EQ(made_paths_.back(), modelPath(bundle, testPin()));
}

TEST_F(LocalModelServiceTest, ABundleThatIsNotThePinnedModelIsNotUsed) {
    config_ = AppConfig{};
    config_.local_model.model_dir = (fs::path(dir_) / "models").string();
    const auto bundle = (fs::path(dir_) / "app" / "models").string();
    writeFile(fs::path(modelPath(bundle, testPin())), "a different file");
    bundled_dirs_ = {bundle};
    make(testPin());
    svc_->handle(LocalModelService::Event::Startup);
    EXPECT_FALSE(svc_->status()["installed"].asBool());
    EXPECT_FALSE(svc_->status()["enabled"].asBool()) << "no verified model: off, not an error";
    EXPECT_EQ(svc_->status()["state"].asString(), "off");
    EXPECT_FALSE(config_.local_model.enabled);
    EXPECT_FALSE(config_.local_model.set_by_user);
}

TEST_F(LocalModelServiceTest, WithNoModelAnywhereANeverTouchedInstallIsOffAndTheSwitchStillDownloads) {
    config_ = AppConfig{};
    config_.local_model.model_dir = (fs::path(dir_) / "models").string();
    make(testPin());
    svc_->handle(LocalModelService::Event::Startup);
    EXPECT_EQ(svc_->status()["state"].asString(), "off");
    EXPECT_EQ(svc_->requestEnable(), "");
    svc_->handle(LocalModelService::Event::Download);
    EXPECT_EQ(downloads_, 1) << "the download stays the fallback";
    EXPECT_TRUE(config_.local_model.enabled);
    EXPECT_TRUE(config_.local_model.set_by_user);
}

TEST_F(LocalModelServiceTest, TheUsersOffIsKeptOverTheBundleAndSaved) {
    const auto cfg_path = (fs::path(dir_) / "config.json").string();
    const auto bundle = (fs::path(dir_) / "app" / "models").string();
    writeFile(fs::path(modelPath(bundle, testPin())), model_bytes_);
    bundled_dirs_ = {bundle};

    config_ = AppConfig{};
    config_.local_model.model_dir = (fs::path(dir_) / "models").string();
    make(testPin());
    svc_->handle(LocalModelService::Event::Startup);
    ASSERT_TRUE(svc_->status()["enabled"].asBool());
    svc_->disable();
    EXPECT_TRUE(config_.local_model.set_by_user);
    ASSERT_TRUE(config_.save(cfg_path));

    // The next start (an update): read back, it stays off.
    AppConfig again;
    ASSERT_TRUE(AppConfig::load(cfg_path, again));
    EXPECT_FALSE(again.local_model.enabled);
    EXPECT_TRUE(again.local_model.set_by_user);
    config_ = again;
    make(testPin());
    svc_->handle(LocalModelService::Event::Startup);
    EXPECT_FALSE(svc_->status()["enabled"].asBool());
    EXPECT_EQ(svc_->status()["state"].asString(), "off");
    EXPECT_TRUE(svc_->status()["installed"].asBool()) << "the model is there for when it is switched on";
    EXPECT_EQ(svc_->requestEnable(), "");
    EXPECT_TRUE(svc_->status()["enabled"].asBool());
    EXPECT_EQ(downloads_, 0);
}

// 7.0.0 wrote "enabled": false into every config it saved, touched or not.
TEST_F(LocalModelServiceTest, A700ConfigThatNeverUsedTheSwitchComesUpOn) {
    const auto cfg_path = writeFile(fs::path(dir_) / "config.json",
                                    R"({"local_model": {"enabled": false, "model_dir": ""}})");
    AppConfig old;
    ASSERT_TRUE(AppConfig::load(cfg_path, old));
    ASSERT_FALSE(old.local_model.enabled);
    ASSERT_FALSE(old.local_model.set_by_user);
    old.local_model.model_dir = (fs::path(dir_) / "models").string();
    config_ = old;

    const auto bundle = (fs::path(dir_) / "app" / "models").string();
    writeFile(fs::path(modelPath(bundle, testPin())), model_bytes_);
    bundled_dirs_ = {bundle};
    make(testPin());
    svc_->handle(LocalModelService::Event::Startup);
    EXPECT_TRUE(svc_->status()["enabled"].asBool());
    EXPECT_TRUE(config_.local_model.enabled);
    EXPECT_FALSE(config_.local_model.set_by_user);
}

TEST_F(LocalModelServiceTest, A700ConfigThatDownloadedTheModelAndSwitchedItOffStaysOff) {
    AppConfig old;   // as 7.0.0 saved it: off, never marked
    old.local_model.enabled = false;
    old.local_model.model_dir = (fs::path(dir_) / "models").string();
    config_ = old;
    // Only an enable (a download, a loaded file) puts the model here.
    writeFile(fs::path(modelPath(config_.local_model.model_dir, testPin())), model_bytes_);
    const auto bundle = (fs::path(dir_) / "app" / "models").string();
    writeFile(fs::path(modelPath(bundle, testPin())), model_bytes_);
    bundled_dirs_ = {bundle};
    make(testPin());
    svc_->handle(LocalModelService::Event::Startup);
    EXPECT_FALSE(svc_->status()["enabled"].asBool());
    EXPECT_FALSE(config_.local_model.enabled);
    EXPECT_TRUE(config_.local_model.set_by_user) << "now recorded as the user's choice";
}

TEST_F(LocalModelServiceTest, A700ConfigThatWasOnStaysOn) {
    AppConfig old;
    old.local_model.enabled = true;
    old.local_model.model_dir = (fs::path(dir_) / "models").string();
    config_ = old;
    writeFile(fs::path(modelPath(config_.local_model.model_dir, testPin())), model_bytes_);
    make(testPin());
    svc_->handle(LocalModelService::Event::Startup);
    EXPECT_TRUE(svc_->status()["enabled"].asBool());
    EXPECT_FALSE(svc_->status()["bundled"].asBool()) << "the copy it downloaded";
    EXPECT_EQ(svc_->status()["state"].asString(), "ready");
}

TEST(LocalModelBundle, TheBundledFoldersAreFoundFromTheExecutable) {
    const auto dirs = bundledModelDirs();
    ASSERT_EQ(dirs.size(), 3u);
    const fs::path exe = fs::path(dirs[0]).parent_path();
    EXPECT_EQ(fs::path(dirs[0]), exe / "models");
    EXPECT_EQ(fs::path(dirs[1]), (exe / ".." / "Resources" / "models").lexically_normal());
    EXPECT_EQ(fs::path(dirs[2]), (exe / ".." / "share" / "hms-cpap" / "models").lexically_normal());
    EXPECT_EQ(findBundledModel({}, pinnedModel()), "");
}

TEST(LocalModelBundle, ThePinIsTheBuildsOne) {
    // cmake/narrator-pin.cmake, compiled in: one pin for the bundle and the check.
    const auto pin = pinnedModel();
    EXPECT_EQ(pin.version, "narrator-v1");
    EXPECT_EQ(pin.sha256, "a0d99c56137466735fa0371dd64e6bdb908b2e33cd250632670534734af48d00");
    EXPECT_EQ(pin.url.rfind("https://github.com/hms-homelab/hms-cpap/releases/download/narrator-v1/", 0), 0u);
    EXPECT_TRUE(verifyRefusal(pin).empty());
    EXPECT_TRUE(downloadRefusal(pin).empty());
}

TEST_F(LocalModelServiceTest, TheConfiguredLlmPathStillStoresSourceLlm) {
    ASSERT_TRUE(db_->saveSummary(device_, "daily", "2099-07-01", "2099-07-01", 1, 1.0, 6.0, 100.0,
                                 "From the configured LLM."));
    const auto rows = summaries();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["source"].asString(), "llm");
}

// -- The source field, on every engine ------------------------------------------

class SummarySourceBackendTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        device_ = "sdd013_src_" + std::to_string(::getpid());
        path_ = (fs::temp_directory_path() / ("hms_cpap_sdd013_src_" + std::to_string(::getpid()) + ".db"))
                    .string();
        std::string skip;
        db_ = openEngine(GetParam(), path_, skip);
        if (!db_) GTEST_SKIP() << skip;
        clean();
    }
    void TearDown() override {
        if (db_) clean();
        db_.reset();
        if (GetParam() == Engine::SQLite) removeSqlite(path_);
    }
    void clean() {
        db_->executeQuery("DELETE FROM cpap_summaries WHERE device_id = " + sql::param(1, db_->dbType()),
                          {device_});
        cleanup(*db_, device_);
    }
    std::shared_ptr<IDatabase> db_;
    std::string device_, path_;
};

// D-701-3 on every engine: the row keeps its fact block, the same facts are
// skipped, changed facts replace the night's local summary.
TEST_P(SummarySourceBackendTest, ANightHasOneLocalSummaryWrittenFromItsFacts) {
    NightSpec n;
    n.hypopneas = 6;
    saveNight(*db_, device_, localAt(2099, 7, 1, 23), n);
    ScriptedModel model;
    for (int i = 0; i < 4; ++i) model.replies.push_back(std::string("Your AHI was 1.00."));

    const auto first = narrateAndStore(*db_, device_, "2099-07-01", model);
    ASSERT_TRUE(first.written) << engineName(GetParam()) << ": " << first.why;
    const auto block = storedFactBlock(*db_, device_, "2099-07-01");
    EXPECT_EQ(block, factBlock(first.night)) << engineName(GetParam());

    const auto again = narrateAndStore(*db_, device_, "2099-07-01", model);
    EXPECT_TRUE(again.skipped) << engineName(GetParam());
    EXPECT_EQ(model.prompts.size(), 1u);

    STRDailyRecord str;
    str.family = STRDailyRecord::Family::AutoSet;
    str.mode = 1;
    const auto changed = narrateAndStore(*db_, device_, "2099-07-01", model, &str);
    EXPECT_TRUE(changed.written) << engineName(GetParam());
    const auto rows = localSummaryRows(*db_, device_, "2099-07-01");
    ASSERT_EQ(rows.size(), 1u) << engineName(GetParam()) << ": the old local summary is replaced";
    EXPECT_NE(rows[0].facts, block);
    EXPECT_EQ(rows[0].facts, factBlock(changed.night));
}

TEST_P(SummarySourceBackendTest, EachSummarySaysWhatWroteIt) {
    ASSERT_TRUE(db_->saveSummary(device_, "daily", "2099-07-01", "2099-07-01", 1, 1.0, 6.0, 100.0,
                                 "From the configured LLM."));
    ASSERT_TRUE(db_->saveSummary(device_, "daily", "2099-07-02", "2099-07-02", 1, 1.0, 6.0, 100.0,
                                 "From the local model.", kLocalSummarySource));
    const auto rows = db_->executeQuery(
        "SELECT summary_text, source FROM cpap_summaries WHERE device_id = " + sql::param(1, db_->dbType()) +
            " ORDER BY range_start",
        {device_});
    ASSERT_EQ(rows.size(), 2u) << engineName(GetParam());
    EXPECT_EQ(rows[0]["source"].asString(), "llm");
    EXPECT_EQ(rows[1]["source"].asString(), "local");
}

INSTANTIATE_TEST_SUITE_P(Engines, SummarySourceBackendTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         engineParamName);
