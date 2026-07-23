// Unit tests for LiveSleepStageRunner.
//
// Drives the runner state machine deterministically with synthetic
// vitals/breathing/oximetry, using a real SleepStageClassifier backed by
// tiny in-memory-trained models (written to a unique per-pid temp dir) and a
// recording MockDatabase. MQTT is passed as nullptr (the runner guards every
// publish path with `!mqtt_ || !mqtt_->isConnected()`), so no live broker is
// touched. No live BLE/TCP/network — fully deterministic.
//
// NOTE on time: classifyLive() internally aggregates epochs over the window
// [session_start, system_clock::now()). To get a stable epoch window we anchor
// session_start at `now - N seconds`. We never assert on an exact epoch count
// that would require wall-clock equality; we assert on relative invariants
// (cursor advances, provisional flag, SQL shape, summary aggregation).

#include "EquipmentStubs.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "services/LiveSleepStageRunner.h"
#include "services/SleepStageClassifier.h"
#include "ml/SleepStageTypes.h"
#include "ml/SleepStageTrainer.h"
#include "ml/RandomForest.h"
#include "ml/StandardScaler.h"
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hms_cpap;
using namespace hms_cpap::ml;
using Clock = std::chrono::system_clock;

// ── Recording mock database ─────────────────────────────────────────────
// Captures every executeQuery() call so tests can assert on the SQL the
// runner builds (session_id, provisional flag, upsert vs insert-or-replace).
// dbType() and isConnected() are configurable to exercise both PG and SQLite
// persistence branches and the early-return-when-disconnected guard.
class RecordingDatabase : public IDatabase {
public:
    HMS_CPAP_STUB_EQUIPMENT_METHODS
    DbType type = DbType::POSTGRESQL;
    bool connected = true;
    std::vector<std::string> queries;

    DbType dbType() const override { return type; }
    bool connect() override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return connected; }

    Json::Value executeQuery(const std::string& sql,
                             const std::vector<std::string>& = {}) override {
        queries.push_back(sql);
        return Json::Value(Json::arrayValue);
    }

    // ── Unused pure-virtuals (stubbed) ──
    bool saveSession(const CPAPSession&) override { return true; }
    bool sessionExists(const std::string&, const std::chrono::system_clock::time_point&) override { return false; }
    std::optional<std::chrono::system_clock::time_point> getLastSessionStart(const std::string&) override { return std::nullopt; }
    std::optional<std::chrono::system_clock::time_point> getSessionStartForSleepDay(const std::string&, const std::string&, bool) override { return std::nullopt; }
    std::optional<SessionMetrics> getSessionMetrics(const std::string&, const std::chrono::system_clock::time_point&) override { return std::nullopt; }
    bool markSessionCompleted(const std::string&, const std::chrono::system_clock::time_point&) override { return true; }
    bool reopenSession(const std::string&, const std::chrono::system_clock::time_point&) override { return true; }
    int deleteSessionsByDateFolder(const std::string&, const std::string&) override { return 0; }
    bool isForceCompleted(const std::string&, const std::chrono::system_clock::time_point&) override { return false; }
    bool setForceCompleted(const std::string&, const std::chrono::system_clock::time_point&) override { return true; }
    std::map<std::string, int> getCheckpointFileSizes(const std::string&, const std::chrono::system_clock::time_point&) override { return {}; }
    std::map<std::string, int> getCheckpointFilesByFolder(const std::string&, const std::string&) override { return {}; }
    bool updateCheckpointFileSizes(const std::string&, const std::chrono::system_clock::time_point&, const std::map<std::string, int>&) override { return true; }
    bool updateDeviceLastSeen(const std::string&) override { return true; }
    bool saveSTRDailyRecords(const std::vector<STRDailyRecord>&) override { return true; }
    std::optional<std::string> getLastSTRDate(const std::string&) override { return std::nullopt; }
    bool aggregateDailySummaryFromSessions(const std::string&) override { return true; }
    std::optional<SessionMetrics> getNightlyMetrics(const std::string&, const std::chrono::system_clock::time_point&) override { return std::nullopt; }
    std::vector<SessionMetrics> getMetricsForDateRange(const std::string&, int) override { return {}; }
    bool saveSummary(const std::string&, const std::string&, const std::string&, const std::string&, int, double, double, double, const std::string&) override { return true; }
    void* rawConnection() override { return nullptr; }
    bool saveOximetrySession(const std::string&, const cpapdash::parser::OximetrySession&) override { return true; }
    bool oximetrySessionExists(const std::string&, const std::string&) override { return false; }
    bool saveLiveOximetrySample(const std::string&, const std::string&, int, int, int) override { return true; }
    OxiSummary getOximetrySummary(const std::string&, const std::string&, const std::string&) override { return {}; }
    OxiRangeSummary getOximetryRangeSummary(const std::string&, const std::string&, const std::string&) override { return {}; }
    std::vector<OxiNightlyPoint> getOximetryNightlySpo2(const std::string&, const std::string&, const std::string&) override { return {}; }
};

// ── Fixture ─────────────────────────────────────────────────────────────
class LiveSleepStageRunnerTest : public ::testing::Test {
protected:
    std::filesystem::path temp_dir_;
    SleepStageClassifier::Config config_;

    void SetUp() override {
        // Unique per-pid temp dir to avoid cross-process collisions.
        temp_dir_ = std::filesystem::temp_directory_path() /
                    ("hms_cpap_live_runner_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(temp_dir_);

        config_.enabled = true;
        config_.live_inference = true;
        config_.model_dir = temp_dir_.string();
        config_.model_version = "live-runner-test-v1";
    }

    void TearDown() override {
        if (std::filesystem::exists(temp_dir_))
            std::filesystem::remove_all(temp_dir_);
    }

    bool writeClassifierBundle(const std::string& path,
                               const ml::RandomForest& model,
                               const ml::StandardScaler& scaler,
                               const std::vector<std::string>& features) {
        nlohmann::json j;
        j["model"] = model.toJson();
        j["scaler"] = scaler.toJson();
        j["features"] = features;
        std::ofstream ofs(path);
        if (!ofs.is_open()) return false;
        ofs << j.dump(2);
        return ofs.good();
    }

    // Train tiny deterministic 4-class models (causal + bidirectional) and
    // write the bundles the classifier expects. Mirrors the approach in
    // test_sleep_stage_classifier.cpp.
    bool createTestModels() {
        std::mt19937 rng(42);
        std::normal_distribution<double> noise(0.0, 0.3);
        double centers[4][2] = {
            {-3.0, -3.0}, { 3.0, -3.0},
            {-3.0,  3.0}, { 3.0,  3.0}
        };
        auto generateData = [&](int n_features) {
            std::vector<std::vector<double>> X;
            std::vector<int> y;
            for (int cls = 0; cls < 4; ++cls) {
                for (int i = 0; i < 30; ++i) {
                    std::vector<double> row(n_features, 0.0);
                    row[0] = centers[cls][0] + noise(rng);
                    row[1] = centers[cls][1] + noise(rng);
                    X.push_back(std::move(row));
                    y.push_back(cls);
                }
            }
            return std::make_pair(X, y);
        };

        TrainingConfig tc;
        tc.rf_params.n_estimators = 20;
        tc.rf_params.max_depth = 6;
        tc.rf_params.random_seed = 42;
        tc.cv_folds = 3;

        auto [X_bidir, y_bidir] = generateData(BIDIR_FEATURE_COUNT);
        tc.mode = FeatureMode::BIDIRECTIONAL;
        auto bidir_result = SleepStageTrainer::train(X_bidir, y_bidir, tc);
        if (!bidir_result.success) return false;
        std::string final_path = temp_dir_.string() + "/" + config_.model_version + "_final.json";
        if (!writeClassifierBundle(final_path, bidir_result.model, bidir_result.scaler,
                                   bidir_result.feature_names))
            return false;

        rng.seed(42);
        auto [X_causal, y_causal] = generateData(CAUSAL_FEATURE_COUNT);
        tc.mode = FeatureMode::CAUSAL;
        auto causal_result = SleepStageTrainer::train(X_causal, y_causal, tc);
        if (!causal_result.success) return false;
        std::string live_path = temp_dir_.string() + "/" + config_.model_version + "_live.json";
        if (!writeClassifierBundle(live_path, causal_result.model, causal_result.scaler,
                                   causal_result.feature_names))
            return false;

        return true;
    }

    std::shared_ptr<SleepStageClassifier> makeReadyClassifier() {
        auto c = std::make_shared<SleepStageClassifier>(config_);
        c->loadModels();
        return c;
    }

    // Synthetic vitals/breathing covering `duration_sec` seconds ending now.
    void makeSyntheticVitals(int duration_sec,
                             Clock::time_point start,
                             std::vector<CPAPVitals>& vitals,
                             std::vector<BreathingSummary>& breathing) {
        for (int i = 0; i < duration_sec; ++i) {
            auto ts = start + std::chrono::seconds(i);
            CPAPVitals v(ts);
            v.spo2 = 96.0;
            v.heart_rate = 65;
            vitals.push_back(v);

            BreathingSummary b(ts);
            b.respiratory_rate = 14.0;
            b.tidal_volume = 0.4;
            b.minute_ventilation = 6.0;
            b.ie_ratio = 0.4;
            b.leak_rate = 2.0;
            b.flow_p95 = 30.0;
            breathing.push_back(b);
        }
    }

    std::vector<OximetrySample> makeSyntheticOximetry(int duration_sec,
                                                      Clock::time_point start) {
        std::vector<OximetrySample> samples;
        for (int i = 0; i < duration_sec; ++i) {
            OximetrySample o{};
            o.timestamp = start + std::chrono::seconds(i);
            o.spo2 = 96;
            o.heart_rate = 65;
            o.invalid_flag = 0;
            o.motion = 0;
            o.vibration = 0;
            samples.push_back(o);
        }
        return samples;
    }

    CPAPSession makeSyntheticSession(int duration_sec) {
        CPAPSession session;
        auto start = Clock::now() - std::chrono::seconds(duration_sec);
        session.session_start = start;
        session.session_end = Clock::now();
        session.duration_seconds = duration_sec;
        makeSyntheticVitals(duration_sec, start, session.vitals, session.breathing_summary);
        return session;
    }
};

// ── Construction / reset (no classifier needed) ─────────────────────────

TEST_F(LiveSleepStageRunnerTest, ConstructsWithNullDependencies) {
    // Null classifier, null DB, null MQTT — must not crash on any callback.
    LiveSleepStageRunner runner(nullptr, nullptr, nullptr, "dev0");
    EXPECT_NO_THROW(runner.reset());
}

TEST_F(LiveSleepStageRunnerTest, BurstWithNullClassifierIsNoOp) {
    auto db = std::make_shared<RecordingDatabase>();
    LiveSleepStageRunner runner(nullptr, db, nullptr, "dev0");

    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    makeSyntheticVitals(300, Clock::now() - std::chrono::seconds(300), vitals, breathing);
    auto oxi = makeSyntheticOximetry(300, Clock::now() - std::chrono::seconds(300));

    EXPECT_NO_THROW(runner.onBurstComplete(vitals, breathing, oxi, Clock::now() - std::chrono::seconds(300), 7));
    EXPECT_TRUE(db->queries.empty());  // nothing persisted
}

TEST_F(LiveSleepStageRunnerTest, BurstWithUnreadyClassifierIsNoOp) {
    // Classifier with no models loaded → isReady()==false → early return.
    auto classifier = std::make_shared<SleepStageClassifier>(config_);
    ASSERT_FALSE(classifier->isReady());
    auto db = std::make_shared<RecordingDatabase>();
    LiveSleepStageRunner runner(classifier, db, nullptr, "dev0");

    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    makeSyntheticVitals(300, Clock::now() - std::chrono::seconds(300), vitals, breathing);
    auto oxi = makeSyntheticOximetry(300, Clock::now() - std::chrono::seconds(300));

    runner.onBurstComplete(vitals, breathing, oxi, Clock::now() - std::chrono::seconds(300), 1);
    EXPECT_TRUE(db->queries.empty());
}

// ── Live burst processing (real classifier) ─────────────────────────────

TEST_F(LiveSleepStageRunnerTest, BurstPersistsProvisionalEpochsAsUpsert) {
    if (!createTestModels()) GTEST_SKIP() << "model training unavailable in this build";
    auto classifier = makeReadyClassifier();
    if (!classifier->isReady()) GTEST_SKIP() << "classifier not ready";

    auto db = std::make_shared<RecordingDatabase>();
    db->type = DbType::POSTGRESQL;
    LiveSleepStageRunner runner(classifier, db, nullptr, "cpap_dev");

    const int dur = 600;  // ~20 epochs of 30s
    auto start = Clock::now() - std::chrono::seconds(dur);
    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    makeSyntheticVitals(dur, start, vitals, breathing);
    auto oxi = makeSyntheticOximetry(dur, start);

    runner.onBurstComplete(vitals, breathing, oxi, start, 42);

    ASSERT_FALSE(db->queries.empty()) << "expected provisional epoch inserts";
    for (const auto& sql : db->queries) {
        // PostgreSQL upsert path.
        EXPECT_NE(sql.find("INSERT INTO cpap_sleep_stages"), std::string::npos);
        EXPECT_NE(sql.find("ON CONFLICT"), std::string::npos);
        // session_id is embedded.
        EXPECT_NE(sql.find("(42,"), std::string::npos) << sql;
        // provisional=true for live bursts.
        EXPECT_NE(sql.find("true"), std::string::npos);
        // model version propagated.
        EXPECT_NE(sql.find(config_.model_version), std::string::npos);
    }
}

TEST_F(LiveSleepStageRunnerTest, SqlitePathUsesInsertOrReplace) {
    if (!createTestModels()) GTEST_SKIP() << "model training unavailable in this build";
    auto classifier = makeReadyClassifier();
    if (!classifier->isReady()) GTEST_SKIP() << "classifier not ready";

    auto db = std::make_shared<RecordingDatabase>();
    db->type = DbType::SQLITE;  // exercise the non-PG branch
    LiveSleepStageRunner runner(classifier, db, nullptr, "cpap_dev");

    const int dur = 600;
    auto start = Clock::now() - std::chrono::seconds(dur);
    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    makeSyntheticVitals(dur, start, vitals, breathing);
    auto oxi = makeSyntheticOximetry(dur, start);

    runner.onBurstComplete(vitals, breathing, oxi, start, 9);

    ASSERT_FALSE(db->queries.empty());
    for (const auto& sql : db->queries) {
        EXPECT_NE(sql.find("INSERT OR REPLACE INTO cpap_sleep_stages"), std::string::npos);
        EXPECT_EQ(sql.find("ON CONFLICT"), std::string::npos);  // not the PG path
        EXPECT_NE(sql.find("(9,"), std::string::npos) << sql;
    }
}

TEST_F(LiveSleepStageRunnerTest, EpochCursorAdvancesAcrossBursts) {
    if (!createTestModels()) GTEST_SKIP() << "model training unavailable in this build";
    auto classifier = makeReadyClassifier();
    if (!classifier->isReady()) GTEST_SKIP() << "classifier not ready";

    auto db = std::make_shared<RecordingDatabase>();
    LiveSleepStageRunner runner(classifier, db, nullptr, "cpap_dev");

    const int dur = 600;
    auto start = Clock::now() - std::chrono::seconds(dur);
    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    makeSyntheticVitals(dur, start, vitals, breathing);
    auto oxi = makeSyntheticOximetry(dur, start);

    // First burst processes all available epochs.
    runner.onBurstComplete(vitals, breathing, oxi, start, 1);
    size_t after_first = db->queries.size();
    ASSERT_GT(after_first, 0u);

    // Second burst with the SAME data: cursor already advanced past every
    // epoch, so classifyLive returns nothing new → no additional inserts.
    runner.onBurstComplete(vitals, breathing, oxi, start, 1);
    EXPECT_EQ(db->queries.size(), after_first)
        << "cursor should suppress re-processing of already-classified epochs";
}

TEST_F(LiveSleepStageRunnerTest, ResetRestartsEpochCursor) {
    if (!createTestModels()) GTEST_SKIP() << "model training unavailable in this build";
    auto classifier = makeReadyClassifier();
    if (!classifier->isReady()) GTEST_SKIP() << "classifier not ready";

    auto db = std::make_shared<RecordingDatabase>();
    LiveSleepStageRunner runner(classifier, db, nullptr, "cpap_dev");

    const int dur = 600;
    auto start = Clock::now() - std::chrono::seconds(dur);
    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    makeSyntheticVitals(dur, start, vitals, breathing);
    auto oxi = makeSyntheticOximetry(dur, start);

    runner.onBurstComplete(vitals, breathing, oxi, start, 1);
    size_t after_first = db->queries.size();
    ASSERT_GT(after_first, 0u);

    // After reset the cursor is 0 again → re-processing produces fresh inserts.
    runner.reset();
    db->queries.clear();
    runner.onBurstComplete(vitals, breathing, oxi, start, 1);
    EXPECT_EQ(db->queries.size(), after_first)
        << "reset should allow re-classification of the same window";
}

TEST_F(LiveSleepStageRunnerTest, BurstDoesNotPersistWhenDbDisconnected) {
    if (!createTestModels()) GTEST_SKIP() << "model training unavailable in this build";
    auto classifier = makeReadyClassifier();
    if (!classifier->isReady()) GTEST_SKIP() << "classifier not ready";

    auto db = std::make_shared<RecordingDatabase>();
    db->connected = false;  // persistEpochs early-returns
    LiveSleepStageRunner runner(classifier, db, nullptr, "cpap_dev");

    const int dur = 600;
    auto start = Clock::now() - std::chrono::seconds(dur);
    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    makeSyntheticVitals(dur, start, vitals, breathing);
    auto oxi = makeSyntheticOximetry(dur, start);

    // Classifier still runs (cursor advances internally) but no SQL emitted.
    runner.onBurstComplete(vitals, breathing, oxi, start, 1);
    EXPECT_TRUE(db->queries.empty());
}

TEST_F(LiveSleepStageRunnerTest, BurstSucceedsWithNullDbAndNullMqtt) {
    if (!createTestModels()) GTEST_SKIP() << "model training unavailable in this build";
    auto classifier = makeReadyClassifier();
    if (!classifier->isReady()) GTEST_SKIP() << "classifier not ready";

    // No DB, no MQTT — runner must still drive the classifier without crashing.
    LiveSleepStageRunner runner(classifier, nullptr, nullptr, "cpap_dev");

    const int dur = 600;
    auto start = Clock::now() - std::chrono::seconds(dur);
    std::vector<CPAPVitals> vitals;
    std::vector<BreathingSummary> breathing;
    makeSyntheticVitals(dur, start, vitals, breathing);
    auto oxi = makeSyntheticOximetry(dur, start);

    EXPECT_NO_THROW(runner.onBurstComplete(vitals, breathing, oxi, start, 1));
}

// ── Session completion (final inference) ────────────────────────────────

TEST_F(LiveSleepStageRunnerTest, SessionCompletePersistsFinalNonProvisional) {
    if (!createTestModels()) GTEST_SKIP() << "model training unavailable in this build";
    auto classifier = makeReadyClassifier();
    if (!classifier->isReady()) GTEST_SKIP() << "classifier not ready";

    auto db = std::make_shared<RecordingDatabase>();
    db->type = DbType::POSTGRESQL;
    LiveSleepStageRunner runner(classifier, db, nullptr, "cpap_dev");

    const int dur = 900;  // 15 min → ~30 epochs
    auto session = makeSyntheticSession(dur);
    auto oxi = makeSyntheticOximetry(dur, *session.session_start);

    runner.onSessionComplete(session, oxi, 123);

    ASSERT_FALSE(db->queries.empty()) << "expected final epoch inserts";
    for (const auto& sql : db->queries) {
        EXPECT_NE(sql.find("INSERT INTO cpap_sleep_stages"), std::string::npos);
        EXPECT_NE(sql.find("(123,"), std::string::npos) << sql;
        // Final epochs are non-provisional.
        EXPECT_NE(sql.find("false"), std::string::npos);
        EXPECT_EQ(sql.find(", true,"), std::string::npos)
            << "final epochs must not be marked provisional: " << sql;
    }
}

TEST_F(LiveSleepStageRunnerTest, SessionCompleteWithUnreadyClassifierIsNoOp) {
    auto classifier = std::make_shared<SleepStageClassifier>(config_);
    ASSERT_FALSE(classifier->isReady());
    auto db = std::make_shared<RecordingDatabase>();
    LiveSleepStageRunner runner(classifier, db, nullptr, "cpap_dev");

    auto session = makeSyntheticSession(900);
    auto oxi = makeSyntheticOximetry(900, *session.session_start);

    runner.onSessionComplete(session, oxi, 5);
    EXPECT_TRUE(db->queries.empty());
}

TEST_F(LiveSleepStageRunnerTest, SessionCompleteWithEmptySessionDoesNotCrash) {
    if (!createTestModels()) GTEST_SKIP() << "model training unavailable in this build";
    auto classifier = makeReadyClassifier();
    if (!classifier->isReady()) GTEST_SKIP() << "classifier not ready";

    auto db = std::make_shared<RecordingDatabase>();
    LiveSleepStageRunner runner(classifier, db, nullptr, "cpap_dev");

    CPAPSession empty;  // no start time, no vitals
    std::vector<OximetrySample> no_oxi;
    EXPECT_NO_THROW(runner.onSessionComplete(empty, no_oxi, 99));
    EXPECT_TRUE(db->queries.empty());  // no epochs → nothing persisted
}

// ── SleepStageSummary aggregation (pure, deterministic) ─────────────────
// publishElapsedRollup / publishFinalSummary delegate to
// SleepStageSummary::fromEpochs; assert that the aggregation the runner
// relies on is correct (30s/epoch → minutes, efficiency, latencies).

static SleepStageEpoch makeEpoch(SleepStage stage, int index) {
    SleepStageEpoch e;
    // Fixed epoch (deterministic): a known epoch start with index offset.
    e.epoch_start = Clock::time_point(std::chrono::seconds(1700000000)) +
                    std::chrono::seconds(index * EPOCH_DURATION_SEC);
    e.epoch_duration_sec = EPOCH_DURATION_SEC;
    e.stage = stage;
    e.confidence = 0.9;
    e.provisional = true;
    e.model_version = "test";
    return e;
}

TEST(SleepStageSummaryAggregation, CountsStagesAndComputesEfficiency) {
    std::vector<SleepStageEpoch> epochs;
    // 4 WAKE, 8 LIGHT, 4 DEEP, 4 REM = 20 epochs = 10 minutes total.
    int idx = 0;
    for (int i = 0; i < 4; ++i) epochs.push_back(makeEpoch(SleepStage::WAKE, idx++));
    for (int i = 0; i < 8; ++i) epochs.push_back(makeEpoch(SleepStage::LIGHT, idx++));
    for (int i = 0; i < 4; ++i) epochs.push_back(makeEpoch(SleepStage::DEEP, idx++));
    for (int i = 0; i < 4; ++i) epochs.push_back(makeEpoch(SleepStage::REM, idx++));

    auto s = SleepStageSummary::fromEpochs(epochs);
    EXPECT_EQ(s.total_epochs, 20);
    EXPECT_EQ(s.wake_minutes, 2);   // 4 epochs / 2
    EXPECT_EQ(s.light_minutes, 4);  // 8 / 2
    EXPECT_EQ(s.deep_minutes, 2);   // 4 / 2
    EXPECT_EQ(s.rem_minutes, 2);    // 4 / 2
    // total_min = 10, sleep_min = 8 → 80%
    EXPECT_DOUBLE_EQ(s.sleep_efficiency_pct, 80.0);
}

TEST(SleepStageSummaryAggregation, RemAndDeepLatency) {
    std::vector<SleepStageEpoch> epochs;
    int idx = 0;
    // 2 WAKE then LIGHT (first sleep at idx 2), DEEP at idx 6, REM at idx 10.
    for (int i = 0; i < 2; ++i) epochs.push_back(makeEpoch(SleepStage::WAKE, idx++));
    for (int i = 0; i < 4; ++i) epochs.push_back(makeEpoch(SleepStage::LIGHT, idx++));
    for (int i = 0; i < 4; ++i) epochs.push_back(makeEpoch(SleepStage::DEEP, idx++));
    for (int i = 0; i < 4; ++i) epochs.push_back(makeEpoch(SleepStage::REM, idx++));

    auto s = SleepStageSummary::fromEpochs(epochs);
    // first_sleep idx=2, first_deep idx=6 → (6-2)/2 = 2 min.
    EXPECT_EQ(s.first_deep_min, 2);
    // first_rem idx=10 → (10-2)/2 = 4 min.
    EXPECT_EQ(s.rem_latency_min, 4);
}

TEST(SleepStageSummaryAggregation, EmptyEpochsYieldsZeroEfficiency) {
    auto s = SleepStageSummary::fromEpochs({});
    EXPECT_EQ(s.total_epochs, 0);
    EXPECT_DOUBLE_EQ(s.sleep_efficiency_pct, 0.0);
    EXPECT_EQ(s.rem_latency_min, 0);
    EXPECT_EQ(s.first_deep_min, 0);
}

TEST(SleepStageSummaryAggregation, AllWakeIsZeroEfficiency) {
    std::vector<SleepStageEpoch> epochs;
    for (int i = 0; i < 10; ++i) epochs.push_back(makeEpoch(SleepStage::WAKE, i));
    auto s = SleepStageSummary::fromEpochs(epochs);
    EXPECT_EQ(s.wake_minutes, 5);
    EXPECT_DOUBLE_EQ(s.sleep_efficiency_pct, 0.0);  // no sleep epochs
    EXPECT_EQ(s.rem_latency_min, 0);                // never entered sleep
}

// ── OximetryService — pure parse/aggregation paths (VLD) ────────────────
// Deterministic VLD parsing the OximetryService relies on. Avoids live
// BLE/mule/network entirely — feeds bytes straight to the parser.

TEST(OximetryParseAggregation, ParsesMultiSampleFile) {
    std::vector<uint8_t> data(40 + 15, 0);
    data[0] = 3; data[1] = 0;                 // version 3
    data[2] = 0xEA; data[3] = 0x07;           // year 2026
    data[4] = 5; data[5] = 10;                // month/day
    data[6] = 23; data[7] = 30; data[8] = 0;  // hh:mm:ss
    data[18] = 12; data[19] = 0;              // duration 12s

    // 3 samples, 4s interval each.
    uint8_t spo2[] = {97, 95, 93};
    uint8_t hr[]   = {60, 62, 64};
    for (int i = 0; i < 3; ++i) {
        data[40 + i*5 + 0] = spo2[i];
        data[40 + i*5 + 1] = hr[i];
        data[40 + i*5 + 2] = 0;  // valid
        data[40 + i*5 + 3] = 0;  // motion
        data[40 + i*5 + 4] = 0;  // vibration
    }

    auto session = cpapdash::parser::VLDParser::parse(data.data(), data.size(), "multi.vld");
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->samples.size(), 3u);
    EXPECT_EQ(session->duration_seconds, 12);
    EXPECT_DOUBLE_EQ(session->sample_interval, 4.0);
    EXPECT_EQ(session->samples[0].spo2, 97);
    EXPECT_EQ(session->samples[2].heart_rate, 64);
    EXPECT_EQ(session->metrics.valid_samples, 3);
    EXPECT_DOUBLE_EQ(session->metrics.min_spo2, 93);
    EXPECT_DOUBLE_EQ(session->metrics.max_spo2, 97);
}

TEST(OximetryParseAggregation, RejectsTruncatedHeader) {
    std::vector<uint8_t> data(10, 0);  // too small for a 40-byte header
    data[0] = 3; data[1] = 0;
    auto session = cpapdash::parser::VLDParser::parse(data.data(), data.size());
    EXPECT_FALSE(session.has_value());
}

TEST(OximetryParseAggregation, OffWristMarkerCountsAsInvalid) {
    std::vector<uint8_t> data(40 + 10, 0);
    data[0] = 3; data[1] = 0;
    data[2] = 0xEA; data[3] = 0x07;
    data[4] = 5; data[5] = 10; data[6] = 23; data[7] = 0; data[8] = 0;
    data[18] = 8; data[19] = 0;

    // Sample 0 valid, sample 1 off-wrist (0xFF).
    data[40] = 96; data[41] = 70; data[42] = 0; data[43] = 0; data[44] = 0;
    data[45] = 0xFF; data[46] = 0xFF; data[47] = 0; data[48] = 0; data[49] = 0;

    auto session = cpapdash::parser::VLDParser::parse(data.data(), data.size());
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session->samples.size(), 2u);
    EXPECT_TRUE(session->samples[0].valid());
    EXPECT_FALSE(session->samples[1].valid());
    EXPECT_EQ(session->metrics.valid_samples, 1);
}