//
// test_DailyOnlyNights.cpp: issue #74 (SDD-082 section 8), end to end.
//
// A night with no session files but a daily record, myAir's or the card's
// STR row, gets a summary from the local model (its template: narrator-v1 was
// trained on full nights) and from the configured LLM, when its data lands:
// myAir data stored (MyAirService::store), or an STR-only night closed
// (night_landed::saveStrDaysAndAnnounce). Nothing runs on a clock. Every
// number in each summary is one of the night's daily facts, and the same data
// landing again writes nothing new.
//
// On a real database: SQLite always, MySQL and PostgreSQL where the repo's DB
// tests run (MYSQL_TEST_HOST, PGHOST). The local model is a scripted stand-in
// for narrator-v1 (the model's own run is the release proof, SDD-082 8.4);
// the LLM is a fake that answers from the facts it is given.
//
// SAFETY: rows are dated 2099 and namespaced to a per-process device id; the
// myAir table is account-level, so its 2099 rows are deleted in TearDown.
//
#include <gtest/gtest.h>

#include "AiFactsEngines.h"
#include "model/LocalModel.h"
#include "model/LocalModelService.h"
#include "model/ModelFile.h"
#include "model/NarrationService.h"
#include "services/AiGrounding.h"
#include "services/DailyOnlySummary.h"
#include "services/MyAirService.h"
#include "services/NightLanded.h"
#include "services/UpdateService.h"
#include "utils/AppConfig.h"

#include <deque>
#include <fstream>
#include <regex>
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

class StandInModel : public ILocalModel {
public:
    int calls = 0;
    int* sink = nullptr;   ///< also counted here (the service frees its model)
    bool ready() const override { return true; }
    std::string whyNotReady() const override { return ""; }
    std::optional<std::string> generate(const std::string&, const GenerateParams&) override {
        ++calls;   // a summary-only night must never reach the model
        if (sink) ++*sink;
        return std::string("A full night's text.");
    }
};

/// A fake LLM: it says the usage and the AHI it finds in the facts, as a
/// model following the prompt would, and counts its calls.
struct FakeLlm {
    int calls = 0;
    std::vector<std::string> prompts;
    std::optional<std::string> operator()(const std::string& prompt) {
        ++calls;
        prompts.push_back(prompt);
        std::smatch u, a;
        const std::regex usage(R"(Usage: ([0-9.]+) hours)"), ahi(R"(AHI: ([0-9.]+) events/hour)");
        std::string text = "This was a summary-only night.";
        if (std::regex_search(prompt, u, usage)) text += " You used the machine for " + u[1].str() + " hours.";
        if (std::regex_search(prompt, a, ahi)) text += " Your AHI was " + a[1].str() + " events per hour.";
        return text;
    }
};

std::string tempDir(const std::string& tag) {
    const auto d = fs::temp_directory_path() / ("hms_issue74_" + tag + "_" + std::to_string(::getpid()));
    fs::create_directories(d);
    return d.string();
}

class DailyOnlyNightTest : public ::testing::TestWithParam<Engine> {
protected:
    void SetUp() override {
        dir_ = tempDir(engineName(GetParam()));
        std::string skip;
        db_ = openEngine(GetParam(), (fs::path(dir_) / "cpap.db").string(), skip);
        if (!db_) GTEST_SKIP() << skip;
        device_ = "issue74_" + std::to_string(::getpid());
        clean();

        // The local model, on as a user would turn it on, with a stand-in
        // model file checked against its own pin.
        config_.local_model.model_dir = (fs::path(dir_) / "models").string();
        config_.local_model.enabled = false;
        config_.local_model.set_by_user = true;
        const auto ref = (fs::path(dir_) / "ref.gguf").string();
        std::ofstream(ref, std::ios::binary) << "stand-in model";
        LocalModelService::Deps d;
        d.db = db_;
        d.config = &config_;
        d.device_id = device_;
        d.pin = Pin{"narrator-test-v1", "https://models.example/n.gguf", update::sha256File(ref)};
        d.bundled_dirs = {};
        d.make_model = [this](const std::string&) {
            auto m = std::make_unique<StandInModel>();
            m->sink = &model_calls_;
            return std::unique_ptr<ILocalModel>(std::move(m));
        };
        d.download = [ref](const std::string&, const std::string& path) {
            fs::copy_file(ref, path, fs::copy_options::overwrite_existing);
            return true;
        };
        svc_ = std::make_unique<LocalModelService>(std::move(d));
        ASSERT_EQ(svc_->requestEnable(), "");
        svc_->handle(LocalModelService::Event::Download);
        ASSERT_TRUE(config_.local_model.enabled);

        // The events, as main wires them, but run where they are told.
        night_landed::clear();
        night_landed::listen([this](const std::string& folder, const std::optional<STRDailyRecord>& str) {
            ++announced_;
            svc_->handle(LocalModelService::Event::NightClosed, folder, str);
            daily_only::summarize(*db_, device_, folder, "Summarize this night.\n{metrics}",
                                  [this](const std::string& p) { return llm_(p); });
        });
    }

    void TearDown() override {
        night_landed::clear();
        svc_.reset();
        if (db_) clean();
        db_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    void clean() {
        cleanup(*db_, device_);
        const auto t = db_->dbType();
        db_->executeQuery("DELETE FROM cpap_summaries WHERE device_id = " + sql::param(1, t), {device_});
        db_->executeQuery("DELETE FROM cpap_myair_records WHERE record_date >= " + sql::castDate(1, t),
                          {"2099-01-01"});
    }

    Json::Value summaries(const std::string& day, const std::string& source) {
        const auto t = db_->dbType();
        return db_->executeQuery(
            "SELECT summary_text, source, facts FROM cpap_summaries WHERE device_id = " + sql::param(1, t) +
                " AND range_start = " + sql::castDate(2, t) + " AND source = " + sql::param(3, t),
            {device_, day, source});
    }

    static MyAirSleepRecord myairNight(const std::string& day, double minutes, double ahi) {
        MyAirSleepRecord r;
        r.start_date = day;
        r.total_usage_min = minutes;
        r.ahi = ahi;
        r.sleep_score = 84;
        r.usage_score = 68;
        r.ahi_score = 5;
        r.mask_score = 5;
        r.leak_score = 6;
        r.mask_pair_count = 2;
        r.leak_percentile = 11.4;
        return r;
    }

    STRDailyRecord strDay(int day, double minutes) {
        STRDailyRecord r;
        r.device_id = device_;
        r.record_date = localAt(2099, 7, day, 12);
        r.duration_minutes = minutes;
        r.ahi = 2.1;
        r.hi = 1.4;
        r.oai = 0.5;
        r.cai = 0.2;
        r.uai = 0.0;
        r.rin = 0.3;
        r.leak_50 = 3.2;
        r.leak_95 = 12.0;
        r.mask_press_50 = 8.1;
        r.mask_press_95 = 10.6;
        r.mode = 1;
        r.epr_level = 3;
        r.mask_events = 4;
        r.family = STRDailyRecord::Family::AutoSet;
        r.min_pressure = 4;
        r.max_pressure = 20;
        return r;
    }

    /// Every number [text] states is one of [day]'s daily facts.
    void expectGrounded(const std::string& text, const std::string& day) {
        const auto night = night_facts::loadDailyOnlyNight(*db_, device_, day);
        ASSERT_TRUE(night) << day;
        const auto block = factBlock(*night);
        EXPECT_TRUE(violations(text, block).empty())
            << engineName(GetParam()) << ": " << text << "\n--- facts ---\n" << block;
    }

    std::string dir_, device_;
    std::shared_ptr<IDatabase> db_;
    AppConfig config_;
    std::unique_ptr<LocalModelService> svc_;
    int model_calls_ = 0;
    FakeLlm llm_;
    int announced_ = 0;
};

}  // namespace

TEST_P(DailyOnlyNightTest, AMyAirOnlyNightIsSummarizedWhenMyAirDataLands) {
    MyAirService myair(db_, config_);
    std::string err;
    ASSERT_EQ(myair.store({myairNight("2099-07-02", 390, 1.2)}, err), 1) << err;
    EXPECT_EQ(announced_, 1) << "myAir data landing is the event";

    // The local model: its template, source local, every number a fact.
    const auto local = summaries("2099-07-02", "local");
    ASSERT_EQ(local.size(), 1u) << engineName(GetParam());
    const std::string text = local[0u]["summary_text"].asString();
    EXPECT_NE(text.find("Summary only"), std::string::npos) << text;
    EXPECT_NE(text.find("6.50 hours"), std::string::npos) << text;
    EXPECT_NE(text.find("AHI was 1.20"), std::string::npos) << text;
    EXPECT_NE(text.find("myAir score was 84"), std::string::npos) << text;
    expectGrounded(text, "2099-07-02");
    EXPECT_EQ(svc_->status()["last"]["how"].asString(), "template");
    EXPECT_EQ(model_calls_, 0) << "narrator-v1 never sees a summary-only night";

    // The configured LLM: the summary-only facts, marked as such.
    const auto llm = summaries("2099-07-02", "llm");
    ASSERT_EQ(llm.size(), 1u) << engineName(GetParam());
    ASSERT_EQ(llm_.calls, 1);
    EXPECT_NE(llm_.prompts[0].find("Summary-only night"), std::string::npos) << llm_.prompts[0];
    EXPECT_NE(llm_.prompts[0].find("myAir score: 84 of 100"), std::string::npos) << llm_.prompts[0];
    expectGrounded(llm[0u]["summary_text"].asString(), "2099-07-02");

    // The same data again: nothing announced, nothing written, no LLM call.
    ASSERT_EQ(myair.store({myairNight("2099-07-02", 390, 1.2)}, err), 1) << err;
    EXPECT_EQ(announced_, 1) << "the hourly refetch of the same figures is not an event";
    // Even announced again (a restart, a second listener), the facts decide.
    night_landed::announce("20990702");
    EXPECT_EQ(summaries("2099-07-02", "local").size(), 1u);
    EXPECT_EQ(summaries("2099-07-02", "llm").size(), 1u);
    EXPECT_EQ(llm_.calls, 1) << "skipped before the LLM: same facts";

    // New figures for the night: one new summary each, replacing the old.
    ASSERT_EQ(myair.store({myairNight("2099-07-02", 420, 0.9)}, err), 1) << err;
    EXPECT_EQ(announced_, 3);
    const auto again = summaries("2099-07-02", "local");
    ASSERT_EQ(again.size(), 1u);
    EXPECT_NE(again[0u]["summary_text"].asString().find("7.00 hours"), std::string::npos);
    EXPECT_EQ(summaries("2099-07-02", "llm").size(), 1u);
    EXPECT_EQ(llm_.calls, 2);
}

TEST_P(DailyOnlyNightTest, AnStrOnlyNightIsSummarizedWhenItCloses) {
    ASSERT_TRUE(night_landed::saveStrDaysAndAnnounce(*db_, {strDay(3, 400)}));
    EXPECT_EQ(announced_, 1);
    const auto local = summaries("2099-07-03", "local");
    ASSERT_EQ(local.size(), 1u) << engineName(GetParam());
    const std::string text = local[0u]["summary_text"].asString();
    EXPECT_NE(text.find("machine's own record"), std::string::npos) << text;
    EXPECT_NE(text.find("6.67 hours"), std::string::npos) << text;
    EXPECT_NE(text.find("AHI was 2.10"), std::string::npos) << text;
    expectGrounded(text, "2099-07-03");
    ASSERT_EQ(summaries("2099-07-03", "llm").size(), 1u);
    EXPECT_NE(llm_.prompts.back().find("Therapy mode: APAP"), std::string::npos) << llm_.prompts.back();
    EXPECT_NE(llm_.prompts.back().find("hypopnea=1.40"), std::string::npos) << llm_.prompts.back();

    // The card read again, whole history: the same night is not an event.
    ASSERT_TRUE(night_landed::saveStrDaysAndAnnounce(*db_, {strDay(3, 400)}));
    EXPECT_EQ(announced_, 1);
    EXPECT_EQ(summaries("2099-07-03", "local").size(), 1u);
    EXPECT_EQ(llm_.calls, 1);
}

TEST_P(DailyOnlyNightTest, ANightWithSessionsIsNotASummaryOnlyNight) {
    NightSpec n;
    n.hypopneas = 6;
    saveNight(*db_, device_, localAt(2099, 7, 4, 23), n);
    ASSERT_TRUE(night_landed::saveStrDaysAndAnnounce(*db_, {strDay(4, 360)}));
    EXPECT_EQ(announced_, 0) << "its sessions' close announces it, not the STR";
    EXPECT_FALSE(night_facts::loadDailyOnlyNight(*db_, device_, "2099-07-04"));
    const auto r = daily_only::summarize(*db_, device_, "2099-07-04", "{metrics}",
                                         [this](const std::string& p) { return llm_(p); });
    EXPECT_FALSE(r.written);
    EXPECT_EQ(llm_.calls, 0);
}

INSTANTIATE_TEST_SUITE_P(Engines, DailyOnlyNightTest,
                         ::testing::Values(Engine::SQLite, Engine::MySQL, Engine::Postgres),
                         engineParamName);

// ---- The real narrator, opt in ------------------------------------------------
//
// HMS_CPAP_NARRATOR_MODEL=<build>/models/narrator-v1/model.gguf (the pinned
// file -DBUNDLE_NARRATOR_MODEL=ON puts there) runs the service as main builds
// it: the bundled model found and verified at Startup, the default turning it
// on, myAir's fetch stubbed with synthetic nights and synced (the real store
// and the real event), an STR-only night closing, and a full session night
// beside them for the model's own text. HMS_CPAP_ISSUE74_DB keeps the SQLite
// file for a scratch hms-cpap to open. Texts and timings are printed.

TEST(NarratorDailyOnly, TheRealModelServiceSummarizesMyAirAndStrOnlyNights) {
    const char* model_env = std::getenv("HMS_CPAP_NARRATOR_MODEL");
    if (!model_env || !*model_env) GTEST_SKIP() << "HMS_CPAP_NARRATOR_MODEL names no model file";
    const fs::path model_file(model_env);
    const std::string models_root = model_file.parent_path().parent_path().string();
    const char* keep = std::getenv("HMS_CPAP_ISSUE74_DB");
    const std::string db_path = keep && *keep ? keep : (fs::path(tempDir("real")) / "cpap.db").string();
    std::string skip;
    auto db = openEngine(Engine::SQLite, db_path, skip);
    ASSERT_TRUE(db) << skip;
    const std::string device = "scratch";
    // A scratch file of its own (openEngine starts it empty), so these
    // synthetic nights carry 2026 dates: narrator-v1 never saw a year like
    // 2099, wrote "2029", and the facts check sent that night to the template.

    AppConfig config;
    config.local_model.model_dir = (fs::path(db_path).parent_path() / "models").string();
    config.myair.enabled = true;            // synthetic: no account, the fetch is stubbed
    config.myair.username = "synthetic";
    config.myair.refresh_token = "synthetic";
    LocalModelService::Deps d;
    d.db = db;
    d.config = &config;
    d.device_id = device;
    d.bundled_dirs = {models_root};
    LocalModelService svc(std::move(d));
    svc.handle(LocalModelService::Event::Startup);
    ASSERT_EQ(svc.status()["state"].asString(), "ready") << svc.status().toStyledString();

    std::vector<std::pair<std::string, double>> timings;
    night_landed::clear();
    night_landed::listen([&](const std::string& folder, const std::optional<STRDailyRecord>& str) {
        const auto t0 = std::chrono::steady_clock::now();
        svc.handle(LocalModelService::Event::NightClosed, folder, str);
        timings.push_back({folder, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()});
    });

    // A full night, for the model's own text beside the summary-only ones.
    NightSpec full;
    full.obstructive = 3;
    full.hypopneas = 4;
    full.unclassified = 1;
    saveNight(*db, device, localAt(2026, 10, 1, 23), full);
    night_landed::announce("20261001");

    MyAirService myair(db, config);
    myair.setFetch([](std::vector<MyAirSleepRecord>& out, std::string&) {
        MyAirSleepRecord a;
        a.start_date = "2026-10-02"; a.total_usage_min = 412; a.ahi = 1.6; a.sleep_score = 88;
        a.usage_score = 70; a.ahi_score = 5; a.mask_score = 5; a.leak_score = 8; a.mask_pair_count = 1;
        a.leak_percentile = 9.6;
        MyAirSleepRecord b = a;
        b.start_date = "2026-10-03"; b.total_usage_min = 205; b.ahi = 3.4; b.sleep_score = 61;
        b.usage_score = 36; b.mask_pair_count = 3; b.leak_percentile = 22.8;
        out = {a, b};
        return true;
    });
    std::string err;
    ASSERT_EQ(myair.syncNow(err), 2) << err;

    STRDailyRecord s;
    s.device_id = device;
    s.record_date = localAt(2026, 10, 4, 12);
    s.duration_minutes = 371;
    s.ahi = 2.4; s.hi = 1.5; s.oai = 0.6; s.cai = 0.3; s.rin = 0.2;
    s.leak_50 = 2.8; s.leak_95 = 14.2; s.mask_press_50 = 8.4; s.mask_press_95 = 11.1;
    s.mode = 1; s.epr_level = 3; s.mask_events = 2;
    s.family = STRDailyRecord::Family::AutoSet; s.min_pressure = 5; s.max_pressure = 15;
    ASSERT_TRUE(night_landed::saveStrDaysAndAnnounce(*db, {s}));

    // The same myAir data again: nothing announced.
    const auto announced = timings.size();
    ASSERT_EQ(myair.syncNow(err), 2) << err;
    EXPECT_EQ(timings.size(), announced);

    for (const char* day : {"2026-10-01", "2026-10-02", "2026-10-03", "2026-10-04"}) {
        const auto rows = db->executeQuery(
            "SELECT summary_text, facts FROM cpap_summaries WHERE device_id = ? AND source = 'local'"
            " AND range_start = ?", {device, day});
        ASSERT_EQ(rows.size(), 1u) << day;
        const std::string text = rows[0u]["summary_text"].asString();
        EXPECT_TRUE(violations(text, rows[0u]["facts"].asString()).empty()) << day << ": " << text;
        std::cout << "\n=== " << day << " ===\n--- facts ---\n" << rows[0u]["facts"].asString()
                  << "--- local summary ---\n" << text << "\n";
    }
    for (const auto& [folder, secs] : timings) std::cout << "night " << folder << ": " << secs << " s\n";
    std::cout << "last: " << svc.status()["last"].toStyledString();
    night_landed::clear();
}

// ---- The rendering, without a database ---------------------------------------

TEST(DailyOnlyFacts, TheBlockSaysSummaryOnlyAndCarriesOnlyTheDailyFigures) {
    NightFacts n;
    n.sleep_day = "2099-07-02";
    n.daily_only = "myair";
    n.usage_hours = 6.5;
    n.usage_percent = 81.25;
    n.index = 1.2;
    n.leak_p95 = 11.4;
    NightMyAirFacts m;
    m.score = 84;
    m.usage_score = 68;
    m.mask_pairs = 2;
    n.myair = m;
    const auto block = factBlock(n);
    EXPECT_NE(block.find("Summary-only night: the daily figures ResMed myAir reports"), std::string::npos) << block;
    EXPECT_NE(block.find("Usage: 6.50 hours (81.25% of 8h target)"), std::string::npos) << block;
    EXPECT_NE(block.find("AHI: 1.20 events/hour"), std::string::npos);
    EXPECT_EQ(block.find("Apneas and hypopneas"), std::string::npos) << "no counts it does not have";
    EXPECT_EQ(block.find("Total events"), std::string::npos);
    EXPECT_NE(block.find("myAir score: 84 of 100 (points: usage 68)"), std::string::npos) << block;
    const auto text = templateSummary(n);
    EXPECT_TRUE(violations(text, block).empty()) << text;
    // A reply that adds a figure the night does not have is caught.
    EXPECT_FALSE(violations("You had 14 obstructive apneas.", block).empty());
}

TEST(DailyOnlyFacts, TheTemplateWritesASummaryOnlyNightAndTheModelIsNotAsked) {
    NightFacts n;
    n.sleep_day = "2099-07-02";
    n.daily_only = "str";
    n.usage_hours = 3.0;
    n.usage_percent = 37.5;
    n.index = 2.1;
    StandInModel model;
    const auto r = narrate(n, model);
    EXPECT_TRUE(r.written);
    EXPECT_EQ(r.how, "template");
    EXPECT_EQ(model.calls, 0);
    EXPECT_NE(r.text.find("Usage was 3.00 hours"), std::string::npos) << r.text;
}
