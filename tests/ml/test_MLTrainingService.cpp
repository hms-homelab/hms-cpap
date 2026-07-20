#include "EquipmentStubs.h"
#include <gtest/gtest.h>

#include "services/MLTrainingService.h"
#include "database/IDatabase.h"
#include "ml/FeatureEngine.h"
#include "ml/RandomForest.h"
#include "ml/StandardScaler.h"

#include <nlohmann/json.hpp>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace hms_cpap;

// ---------------------------------------------------------------------------
// FakeDb: an in-memory IDatabase that only implements isConnected() and
// executeQuery(). Everything else is a stub. This lets us drive the
// MLTrainingService data-loading + feature-engineering paths deterministically
// without a real PostgreSQL/SQLite backend.
// ---------------------------------------------------------------------------
class FakeDb : public IDatabase {
public:
    HMS_CPAP_STUB_EQUIPMENT_METHODS
    bool connected_ = true;
    Json::Value rows_{Json::arrayValue};
    int query_count_ = 0;
    std::string last_sql_;

    DbType dbType() const override { return DbType::POSTGRESQL; }

    bool connect() override { connected_ = true; return true; }
    void disconnect() override { connected_ = false; }
    bool isConnected() const override { return connected_; }

    Json::Value executeQuery(const std::string& sql,
                             const std::vector<std::string>& /*params*/ = {}) {
        ++query_count_;
        last_sql_ = sql;
        return rows_;
    }

    // --- Unused pure-virtual stubs ------------------------------------------
    bool saveSession(const CPAPSession&) override { return true; }
    bool sessionExists(const std::string&,
                       const std::chrono::system_clock::time_point&) override { return false; }
    std::optional<std::chrono::system_clock::time_point>
        getLastSessionStart(const std::string&) override { return std::nullopt; }
    std::optional<std::chrono::system_clock::time_point>
        getSessionStartForSleepDay(const std::string&, const std::string&, bool) override {
            return std::nullopt; }
    std::optional<SessionMetrics> getSessionMetrics(
        const std::string&, const std::chrono::system_clock::time_point&) override {
            return std::nullopt; }
    bool markSessionCompleted(const std::string&,
                              const std::chrono::system_clock::time_point&) override { return true; }
    bool reopenSession(const std::string&,
                       const std::chrono::system_clock::time_point&) override { return true; }
    int deleteSessionsByDateFolder(const std::string&, const std::string&) override { return 0; }
    bool isForceCompleted(const std::string&,
                          const std::chrono::system_clock::time_point&) override { return false; }
    bool setForceCompleted(const std::string&,
                           const std::chrono::system_clock::time_point&) override { return true; }
    std::map<std::string, int> getCheckpointFileSizes(
        const std::string&, const std::chrono::system_clock::time_point&) override { return {}; }
    bool updateCheckpointFileSizes(
        const std::string&, const std::chrono::system_clock::time_point&,
        const std::map<std::string, int>&) override { return true; }
    std::map<std::string, int> getCheckpointFilesByFolder(
        const std::string&, const std::string&) override { return {}; }
    bool updateDeviceLastSeen(const std::string&) override { return true; }
    bool saveSTRDailyRecords(const std::vector<STRDailyRecord>&) override { return true; }
    std::optional<std::string> getLastSTRDate(const std::string&) override { return std::nullopt; }
    std::optional<SessionMetrics> getNightlyMetrics(
        const std::string&, const std::chrono::system_clock::time_point&) override {
            return std::nullopt; }
    std::vector<SessionMetrics> getMetricsForDateRange(const std::string&, int) override {
        return {}; }
    bool saveSummary(const std::string&, const std::string&, const std::string&,
                     const std::string&, int, double, double, double,
                     const std::string&) override { return true; }
    bool saveOximetrySession(const std::string&,
                             const cpapdash::parser::OximetrySession&) override { return true; }
    bool oximetrySessionExists(const std::string&, const std::string&) override { return false; }
    bool saveLiveOximetrySample(const std::string&, const std::string&,
                                int, int, int) override { return true; }
    OxiSummary getOximetrySummary(const std::string&, const std::string&,
                                  const std::string&) override { return {}; }
    OxiRangeSummary getOximetryRangeSummary(const std::string&, const std::string&,
                                            const std::string&) override { return {}; }
    std::vector<OxiNightlyPoint> getOximetryNightlySpo2(const std::string&, const std::string&,
                                                        const std::string&) override { return {}; }
    void* rawConnection() override { return nullptr; }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Unique temp dir per pid + test invocation, removed in dtor.
class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("hms_ml_test_" + tag + "_" + std::to_string(::getpid()) +
                 "_" + std::to_string(++counter));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    std::string str() const { return path_.string(); }

private:
    fs::path path_;
};

// Build a Json row matching the columns loadTrainingData() reads. Values are
// strings (executeQuery returns everything as strings, exactly like the real
// PostgreSQL/SQLite backends do).
static Json::Value makeRow(const std::string& date, double duration_min, double ahi,
                           double leak95, double press95) {
    Json::Value r;
    r["record_date"]      = date;
    r["duration_minutes"] = std::to_string(duration_min);
    r["ahi"]              = std::to_string(ahi);
    r["hi"]               = std::to_string(ahi * 0.5);
    r["oai"]              = std::to_string(ahi * 0.3);
    r["cai"]              = std::to_string(ahi * 0.2);
    r["mask_press_50"]    = std::to_string(press95 - 1.0);
    r["mask_press_95"]    = std::to_string(press95);
    r["mask_press_max"]   = std::to_string(press95 + 1.0);
    r["leak_50"]          = std::to_string(leak95 * 0.5);
    r["leak_95"]          = std::to_string(leak95);
    r["leak_max"]         = std::to_string(leak95 * 1.5);
    r["resp_rate_50"]     = std::to_string(15.0);
    r["tid_vol_50"]       = std::to_string(450.0);
    r["min_vent_50"]      = std::to_string(6.5);
    return r;
}

// A deterministic 20-day dataset of plausible CPAP daily summaries.
static Json::Value makeDataset(int n_days = 20) {
    Json::Value rows(Json::arrayValue);
    for (int i = 0; i < n_days; ++i) {
        // record_date 2024-01-01 .. (use day-of-month; stays valid for n<=28)
        char buf[16];
        std::snprintf(buf, sizeof(buf), "2024-01-%02d", i + 1);
        double ahi      = 4.0 + (i % 5) * 0.7;       // 4.0 .. 6.8 cyclic
        double leak95   = 10.0 + (i % 4) * 3.0;      // 10 .. 19 cyclic
        double press95  = 9.0 + (i % 3) * 0.5;       // 9.0 .. 10.0
        double duration = 360.0 + (i % 6) * 20.0;    // 6h .. 7.6h
        rows.append(makeRow(buf, duration, ahi, leak95, press95));
    }
    return rows;
}

// Train a tiny RandomForest + scaler on a synthesized feature set and write the
// four model JSON files MLTrainingService::loadModels() expects into model_dir.
// This exercises the exact on-disk format used by saveModels()/loadModels().
static void writeTrainedModels(const std::string& model_dir, const Json::Value& rows) {
    // Reconstruct DailyRecords the same way loadTrainingData would, then build
    // features so the persisted scaler/feature dimensions match prediction time.
    std::vector<ml::DailyRecord> recs;
    for (const auto& row : rows) {
        ml::DailyRecord rec;
        rec.record_date      = row["record_date"].asString();
        rec.duration_minutes = std::stod(row["duration_minutes"].asString());
        rec.ahi              = std::stod(row["ahi"].asString());
        rec.hi               = std::stod(row["hi"].asString());
        rec.oai              = std::stod(row["oai"].asString());
        rec.cai              = std::stod(row["cai"].asString());
        rec.mask_press_50    = std::stod(row["mask_press_50"].asString());
        rec.mask_press_95    = std::stod(row["mask_press_95"].asString());
        rec.mask_press_max   = std::stod(row["mask_press_max"].asString());
        rec.leak_50          = std::stod(row["leak_50"].asString());
        rec.leak_95          = std::stod(row["leak_95"].asString());
        rec.leak_max         = std::stod(row["leak_max"].asString());
        rec.resp_rate_50     = std::stod(row["resp_rate_50"].asString());
        rec.tid_vol_50       = std::stod(row["tid_vol_50"].asString());
        rec.min_vent_50      = std::stod(row["min_vent_50"].asString());
        recs.push_back(std::move(rec));
    }

    auto fe = ml::FeatureEngine::build(recs);
    EXPECT_FALSE(fe.X.empty()) << "feature matrix must be non-empty for model fixtures";

    ml::RandomForest::Params small;
    small.n_estimators = 5;   // keep fixtures tiny & fast
    small.max_depth = 4;

    auto write_one = [&](const std::string& name, bool classification, int n_classes,
                         const std::vector<double>& y_reg, const std::vector<int>& y_clf) {
        ml::StandardScaler scaler;
        auto Xs = scaler.fitTransform(fe.X);

        ml::RandomForest rf(small);
        if (classification) {
            rf.fitClassification(Xs, y_clf, n_classes);
        } else {
            rf.fitRegression(Xs, y_reg);
        }

        nlohmann::json j;
        j["model"] = rf.toJson();
        j["scaler"] = scaler.toJson();
        j["features"] = fe.feature_names;
        j["trained_at"] = "2024-01-20T00:00:00Z";

        std::ofstream ofs(model_dir + "/" + name + ".json");
        ofs << j.dump(2);
    };

    write_one("ahi_forecaster",       false, 0, fe.target_ahi,   {});
    write_one("compliance_predictor", false, 0, fe.target_hours, {});
    write_one("mask_fit_predictor",   true,  2, {}, fe.target_high_leak);
    write_one("anomaly_detector",     true,  4, {}, fe.target_anomaly);
}

static MLTrainingService::Config baseConfig(const std::string& model_dir) {
    MLTrainingService::Config cfg;
    cfg.enabled = true;
    cfg.schedule = "weekly";
    cfg.model_dir = model_dir;
    cfg.min_days = 7;
    cfg.max_training_days = 0;
    cfg.device_id = "cpap_test";
    return cfg;
}

// ===========================================================================
// getStatus()
// ===========================================================================

TEST(MLTrainingServiceTest, StatusNotConfiguredWhenDisabled) {
    MLTrainingService::Config cfg;  // enabled defaults to false
    auto db = std::make_shared<FakeDb>();
    MLTrainingService svc(cfg, db, nullptr);

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "not_configured");
    // Disabled config short-circuits before any other field is set.
    EXPECT_FALSE(status.isMember("schedule"));
}

TEST(MLTrainingServiceTest, StatusIdleWhenEnabledButUntrained) {
    TempDir td("idle");
    auto cfg = baseConfig(td.str());
    cfg.schedule = "daily";
    cfg.min_days = 21;
    auto db = std::make_shared<FakeDb>();
    MLTrainingService svc(cfg, db, nullptr);

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "idle");
    EXPECT_TRUE(status["last_trained"].isNull());
    EXPECT_FALSE(status["models_loaded"].asBool());
    EXPECT_EQ(status["schedule"].asString(), "daily");
    EXPECT_EQ(status["min_days"].asInt(), 21);
    // No predictions yet.
    EXPECT_FALSE(status.isMember("predictions"));
}

// ===========================================================================
// getLastResult()
// ===========================================================================

TEST(MLTrainingServiceTest, LastResultDefaultsToUnsuccessfulEmpty) {
    TempDir td("lastresult");
    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    MLTrainingService svc(cfg, db, nullptr);

    auto res = svc.getLastResult();
    EXPECT_FALSE(res.success);
    EXPECT_TRUE(res.timestamp.empty());
    EXPECT_TRUE(res.error.empty());
    EXPECT_TRUE(res.models.empty());
}

// ===========================================================================
// predictLatest() guard branches
// ===========================================================================

TEST(MLTrainingServiceTest, PredictLatestReturnsDefaultsWhenModelsNotLoaded) {
    TempDir td("noload");
    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    db->rows_ = makeDataset();  // plenty of data, but no trained/loaded models
    MLTrainingService svc(cfg, db, nullptr);

    auto preds = svc.predictLatest();
    // Default-constructed Predictions
    EXPECT_DOUBLE_EQ(preds.predicted_ahi, 0.0);
    EXPECT_DOUBLE_EQ(preds.predicted_hours, 0.0);
    EXPECT_DOUBLE_EQ(preds.leak_risk_pct, 0.0);
    EXPECT_EQ(preds.anomaly_class, "NORMAL");
    // Guard returns before touching the DB.
    EXPECT_EQ(db->query_count_, 0);
}

// ===========================================================================
// Model persistence round-trip via start() -> loadModels() -> predictLatest()
// ===========================================================================

TEST(MLTrainingServiceTest, StartLoadsModelsAndProducesPredictions) {
    TempDir td("loadok");
    auto rows = makeDataset();
    writeTrainedModels(td.str(), rows);

    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    db->rows_ = rows;
    MLTrainingService svc(cfg, db, nullptr);

    // start() should: create model_dir, load the 4 fixtures, run predictLatest()
    // once, then spawn the worker thread (which will NOT train within the test
    // window because models are loaded and last_train_time_ is set to "now").
    svc.start();
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "idle");
    EXPECT_TRUE(status["models_loaded"].asBool());

    // predictLatest() ran during start(); the DB was queried for features.
    EXPECT_GE(db->query_count_, 1);

    // Predictions should have been populated and surfaced in status.
    ASSERT_TRUE(status.isMember("predictions"));
    const auto& p = status["predictions"];
    // predicted_hours derives from compliance regressor on ~6-7.6h durations.
    EXPECT_GT(p["predicted_hours"].asDouble(), 0.0);
    // anomaly_class must be one of the known labels.
    std::string ac = p["anomaly_class"].asString();
    EXPECT_TRUE(ac == "NORMAL" || ac == "AHI_ANOMALY" ||
                ac == "LEAK_ANOMALY" || ac == "PRESSURE_ANOMALY");
    // leak risk is a percentage in [0,100].
    EXPECT_GE(p["leak_risk_pct"].asDouble(), 0.0);
    EXPECT_LE(p["leak_risk_pct"].asDouble(), 100.0);
}

TEST(MLTrainingServiceTest, PredictLatestAfterLoadIsCallableDirectly) {
    TempDir td("predict");
    auto rows = makeDataset();
    writeTrainedModels(td.str(), rows);

    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    db->rows_ = rows;
    MLTrainingService svc(cfg, db, nullptr);

    svc.start();
    svc.stop();

    // Calling predictLatest() again post-load must not throw and must produce
    // a finite AHI estimate (full predict path: scaler.transformRow + RF).
    MLTrainingService::Predictions preds;
    ASSERT_NO_THROW({ preds = svc.predictLatest(); });
    EXPECT_TRUE(std::isfinite(preds.predicted_ahi));
    EXPECT_TRUE(std::isfinite(preds.predicted_hours));
}

// ===========================================================================
// loadModels() failure branches (start() must remain safe)
// ===========================================================================

TEST(MLTrainingServiceTest, StartWithNoModelFilesDoesNotLoadAndDoesNotCrash) {
    TempDir td("missing");  // empty dir, no model files
    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    db->rows_ = makeDataset();
    MLTrainingService svc(cfg, db, nullptr);

    svc.start();
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_FALSE(status["models_loaded"].asBool());
    // No predictions since models never loaded.
    EXPECT_FALSE(status.isMember("predictions"));
}

TEST(MLTrainingServiceTest, StartWithCorruptModelFileFailsLoadGracefully) {
    TempDir td("corrupt");
    auto rows = makeDataset();
    writeTrainedModels(td.str(), rows);

    // Corrupt one of the four required files; loadModels() ANDs all four, so
    // overall load must fail and models_loaded_ stays false.
    {
        std::ofstream ofs(td.str() + "/anomaly_detector.json");
        ofs << "{ this is not valid json ]";
    }

    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    db->rows_ = rows;
    MLTrainingService svc(cfg, db, nullptr);

    svc.start();
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_FALSE(status["models_loaded"].asBool());
}

// ===========================================================================
// start() with disabled config is a no-op (no thread, no dir creation needed)
// ===========================================================================

TEST(MLTrainingServiceTest, StartDisabledIsNoOp) {
    MLTrainingService::Config cfg;  // disabled
    cfg.model_dir = (fs::temp_directory_path() /
                     ("hms_ml_disabled_" + std::to_string(::getpid()))).string();
    auto db = std::make_shared<FakeDb>();
    db->rows_ = makeDataset();
    MLTrainingService svc(cfg, db, nullptr);

    svc.start();   // should return immediately without spawning a worker
    svc.stop();    // safe even though nothing was started

    EXPECT_EQ(db->query_count_, 0);
    // Directory was not created because start() bailed before create_directories.
    EXPECT_FALSE(fs::exists(cfg.model_dir));
}

// ===========================================================================
// loadModels() creates the model directory on start (enabled path)
// ===========================================================================

TEST(MLTrainingServiceTest, StartCreatesModelDirectory) {
    auto dir = (fs::temp_directory_path() /
                ("hms_ml_mkdir_" + std::to_string(::getpid()) + "_" +
                 std::to_string(::time(nullptr)))).string();
    ASSERT_FALSE(fs::exists(dir));

    auto cfg = baseConfig(dir);
    auto db = std::make_shared<FakeDb>();
    db->rows_ = makeDataset();
    MLTrainingService svc(cfg, db, nullptr);

    svc.start();
    svc.stop();

    EXPECT_TRUE(fs::exists(dir));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ===========================================================================
// EXTENDED COVERAGE
//
// The tests below extend coverage of the data-loading helpers, predict-time
// guard branches, getStatus() rendering of a populated TrainingResult, and a
// full deterministic end-to-end training run (runTrainingPipeline + saveModels)
// driven through the public triggerTraining() + worker thread. All deterministic
// and offline: nullptr MQTT (every publish/subscribe path is a guarded no-op),
// temp dirs unique per pid, synthetic in-memory rows only.
// ===========================================================================

// Build a row using *native JSON numeric* values (not strings) so we exercise
// jsonToDouble()'s isDouble()/isInt() branch as well as the string branch.
static Json::Value makeNumericRow(const std::string& date, double duration_min,
                                   double ahi, double leak95, double press95) {
    Json::Value r;
    r["record_date"]      = date;          // string (column is text)
    r["duration_minutes"] = duration_min;  // JSON double
    r["ahi"]              = ahi;            // JSON double
    r["hi"]               = ahi * 0.5;
    r["oai"]              = ahi * 0.3;
    r["cai"]              = ahi * 0.2;
    r["mask_press_50"]    = press95 - 1.0;
    r["mask_press_95"]    = press95;
    r["mask_press_max"]   = press95 + 1.0;
    r["leak_50"]          = leak95 * 0.5;
    r["leak_95"]          = leak95;
    r["leak_max"]         = leak95 * 1.5;
    r["resp_rate_50"]     = 15.0;
    r["tid_vol_50"]       = 450.0;
    r["min_vent_50"]      = 6.5;
    return r;
}

static Json::Value makeNumericDataset(int n_days = 20) {
    Json::Value rows(Json::arrayValue);
    for (int i = 0; i < n_days; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "2024-02-%02d", i + 1);
        double ahi      = 4.0 + (i % 5) * 0.7;
        double leak95   = 10.0 + (i % 4) * 3.0;
        double press95  = 9.0 + (i % 3) * 0.5;
        double duration = 360.0 + (i % 6) * 20.0;
        rows.append(makeNumericRow(buf, duration, ahi, leak95, press95));
    }
    return rows;
}

// ---------------------------------------------------------------------------
// triggerTraining(): public, no side effects observable without the worker,
// but must be callable safely (sets the request flag) and idempotent.
// ---------------------------------------------------------------------------

TEST(MLTrainingServiceExtTest, TriggerTrainingIsSafeWithoutStart) {
    TempDir td("trigger");
    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    db->rows_ = makeDataset();
    MLTrainingService svc(cfg, db, nullptr);

    // No worker started; just verify the call does not touch the DB or throw.
    ASSERT_NO_THROW(svc.triggerTraining());
    ASSERT_NO_THROW(svc.triggerTraining());  // idempotent
    EXPECT_EQ(db->query_count_, 0);
}

// ---------------------------------------------------------------------------
// predictLatest(): models loaded but too few rows (<7) -> insufficient-data
// guard returns defaults. This branch sits *after* the models-loaded check,
// which the existing "not loaded" test cannot reach.
// ---------------------------------------------------------------------------

TEST(MLTrainingServiceExtTest, PredictLatestInsufficientDataAfterLoad) {
    TempDir td("fewrows");
    auto rows = makeDataset();            // 20 rows used to TRAIN the fixtures
    writeTrainedModels(td.str(), rows);

    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    MLTrainingService svc(cfg, db, nullptr);

    svc.start();  // loads the 4 models -> models_loaded_ = true
    // Now starve the DB: only 3 rows -> predictLatest() must bail on data.size()<7.
    Json::Value few(Json::arrayValue);
    few.append(makeRow("2024-03-01", 360, 4.0, 12.0, 9.0));
    few.append(makeRow("2024-03-02", 380, 4.5, 13.0, 9.5));
    few.append(makeRow("2024-03-03", 400, 5.0, 14.0, 10.0));
    db->rows_ = few;
    db->query_count_ = 0;

    auto preds = svc.predictLatest();
    svc.stop();

    EXPECT_DOUBLE_EQ(preds.predicted_ahi, 0.0);
    EXPECT_DOUBLE_EQ(preds.predicted_hours, 0.0);
    EXPECT_EQ(preds.anomaly_class, "NORMAL");
    // It did query the DB (got past models-loaded check) before bailing.
    EXPECT_GE(db->query_count_, 1);
}

// ---------------------------------------------------------------------------
// predictLatest(): models loaded, plenty of rows, but FeatureEngine yields an
// empty matrix because there are <8 records. Use exactly 7 valid rows: enough
// to pass the size()<7 guard (7 is not <7) yet FeatureEngine needs n>=8, so
// fe.X is empty and the "empty matrix" guard returns defaults.
// ---------------------------------------------------------------------------

TEST(MLTrainingServiceExtTest, PredictLatestEmptyFeatureMatrixGuard) {
    TempDir td("emptyfe");
    auto rows = makeDataset();
    writeTrainedModels(td.str(), rows);

    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    MLTrainingService svc(cfg, db, nullptr);
    svc.start();

    Json::Value seven(Json::arrayValue);
    for (int i = 0; i < 7; ++i) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "2024-04-%02d", i + 1);
        seven.append(makeRow(buf, 360 + i * 10, 4.0 + i * 0.1, 12.0, 9.0));
    }
    db->rows_ = seven;

    auto preds = svc.predictLatest();
    svc.stop();

    // FeatureEngine::build returns empty for n<8 -> defaults preserved.
    EXPECT_DOUBLE_EQ(preds.predicted_ahi, 0.0);
    EXPECT_DOUBLE_EQ(preds.predicted_hours, 0.0);
}

// ---------------------------------------------------------------------------
// loadTrainingData(): database not connected -> returns no records, so
// predictLatest()'s data.size()<7 guard fires with zero rows and the DB is
// never queried (isConnected() short-circuits inside loadTrainingData()).
// ---------------------------------------------------------------------------

TEST(MLTrainingServiceExtTest, PredictLatestDbDisconnectedYieldsDefaults) {
    TempDir td("disconnected");
    auto rows = makeDataset();
    writeTrainedModels(td.str(), rows);

    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    MLTrainingService svc(cfg, db, nullptr);
    svc.start();         // loads models while connected
    db->connected_ = false;  // now simulate a dropped connection
    db->query_count_ = 0;

    auto preds = svc.predictLatest();
    svc.stop();

    EXPECT_DOUBLE_EQ(preds.predicted_ahi, 0.0);
    // loadTrainingData() returns early on !isConnected(), so no query issued.
    EXPECT_EQ(db->query_count_, 0);
}

// ---------------------------------------------------------------------------
// loadTrainingData(): max_training_days > 0 appends a date-window clause to the
// SQL. Verify the generated SQL the FakeDb sees, exercising that branch.
// Also verifies the day_of_week parsing path runs over valid YYYY-MM-DD dates.
// ---------------------------------------------------------------------------

TEST(MLTrainingServiceExtTest, MaxTrainingDaysAddsDateWindowToSql) {
    TempDir td("window");
    auto rows = makeDataset();
    writeTrainedModels(td.str(), rows);

    auto cfg = baseConfig(td.str());
    cfg.max_training_days = 45;   // > 0 -> INTERVAL clause should appear
    auto db = std::make_shared<FakeDb>();
    db->rows_ = rows;             // 20 rows so predict path proceeds
    MLTrainingService svc(cfg, db, nullptr);

    svc.start();   // load + predictLatest() -> loadTrainingData() issues the query
    svc.stop();

    EXPECT_NE(db->last_sql_.find("INTERVAL '45 days'"), std::string::npos)
        << "SQL was: " << db->last_sql_;
    EXPECT_NE(db->last_sql_.find("cpap_daily_summary"), std::string::npos);
    EXPECT_NE(db->last_sql_.find("ORDER BY record_date"), std::string::npos);
}

// ---------------------------------------------------------------------------
// jsonToDouble() numeric branch: feed rows whose numeric columns are real JSON
// numbers (not strings). predictLatest() must still produce finite output,
// proving the isDouble()/isInt() conversion path works.
// ---------------------------------------------------------------------------

TEST(MLTrainingServiceExtTest, NumericJsonColumnsProduceFinitePredictions) {
    TempDir td("numeric");
    auto rows = makeNumericDataset();
    writeTrainedModels(td.str(), rows);   // fixtures built from numeric rows

    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();
    db->rows_ = rows;
    MLTrainingService svc(cfg, db, nullptr);

    svc.start();
    MLTrainingService::Predictions preds;
    ASSERT_NO_THROW({ preds = svc.predictLatest(); });
    svc.stop();

    EXPECT_TRUE(std::isfinite(preds.predicted_ahi));
    EXPECT_TRUE(std::isfinite(preds.predicted_hours));
    EXPECT_TRUE(std::isfinite(preds.leak_risk_pct));
}

// ---------------------------------------------------------------------------
// jsonToDouble() robustness: a row with a NULL numeric field and a malformed
// numeric string must default to 0.0 without throwing. We only assert the
// service survives and yields finite predictions over a mostly-valid dataset
// with one degenerate trailing row.
// ---------------------------------------------------------------------------

TEST(MLTrainingServiceExtTest, MalformedAndNullColumnsTreatedAsZero) {
    TempDir td("badcols");
    auto rows = makeDataset();
    writeTrainedModels(td.str(), rows);

    auto cfg = baseConfig(td.str());
    auto db = std::make_shared<FakeDb>();

    // Take the trained dataset and corrupt one row's numeric fields:
    //  - leak_95 set to a non-numeric string  -> stod throws -> 0.0
    //  - ahi set to JSON null                  -> isNull()     -> 0.0
    Json::Value corrupted = rows;
    Json::Value& bad = corrupted[corrupted.size() - 1];
    bad["leak_95"] = "not-a-number";
    bad["ahi"]     = Json::nullValue;
    db->rows_ = corrupted;

    MLTrainingService svc(cfg, db, nullptr);
    svc.start();
    MLTrainingService::Predictions preds;
    ASSERT_NO_THROW({ preds = svc.predictLatest(); });
    svc.stop();

    EXPECT_TRUE(std::isfinite(preds.predicted_ahi));
    EXPECT_TRUE(std::isfinite(preds.predicted_hours));
}

// ---------------------------------------------------------------------------
// Full deterministic end-to-end training run.
//
// Drives the real worker loop: triggerTraining() flags a request, the worker
// (after its fixed-interval poll) runs runTrainingPipeline() over a synthetic
// 30-day dataset, persists 4 model files via saveModels(), then runs
// predictLatest(). We then assert on getStatus()'s populated-result rendering
// (status "idle"/success branch, models[] array, predictions block) and that
// the four model JSON files were written and reload cleanly in a fresh service.
//
// The worker sleeps in 1-second steps up to 60s between polls, so allow ~75s.
// ---------------------------------------------------------------------------

TEST(MLTrainingServiceExtTest, EndToEndTrainPersistsModelsAndPopulatesStatus) {
    TempDir td("e2e");
    auto cfg = baseConfig(td.str());
    cfg.min_days = 7;          // 30-day dataset clears this easily
    auto db = std::make_shared<FakeDb>();
    db->rows_ = makeDataset(28);  // 28 valid days (max for day-of-month scheme)
    MLTrainingService svc(cfg, db, nullptr);

    // No models on disk -> start() will NOT load; worker's shouldTrainNow()
    // would also return true (never trained), but we force it explicitly.
    svc.start();
    svc.triggerTraining();

    // Wait (bounded) for training to complete: poll getLastResult() success.
    MLTrainingService::TrainingResult res;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline) {
        res = svc.getLastResult();
        if (!res.timestamp.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    svc.stop();

    ASSERT_FALSE(res.timestamp.empty()) << "training did not complete in time window";
    ASSERT_TRUE(res.success) << "training failed: " << res.error;
    ASSERT_EQ(res.models.size(), 4u);

    // Each model has the expected name and a recorded sample count.
    std::vector<std::string> names;
    for (const auto& m : res.models) {
        names.push_back(m.name);
        EXPECT_GT(m.samples_used, 0);
    }
    EXPECT_NE(std::find(names.begin(), names.end(), "ahi_forecaster"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "mask_fit_predictor"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "compliance_predictor"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "anomaly_detector"), names.end());

    // saveModels() wrote all four JSON files.
    EXPECT_TRUE(fs::exists(td.str() + "/ahi_forecaster.json"));
    EXPECT_TRUE(fs::exists(td.str() + "/mask_fit_predictor.json"));
    EXPECT_TRUE(fs::exists(td.str() + "/compliance_predictor.json"));
    EXPECT_TRUE(fs::exists(td.str() + "/anomaly_detector.json"));

    // getStatus() now renders the populated, successful result (idle branch with
    // a non-empty timestamp), the models[] array, and a predictions block.
    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "idle");
    EXPECT_TRUE(status["models_loaded"].asBool());
    EXPECT_FALSE(status["last_trained"].asString().empty());
    ASSERT_TRUE(status.isMember("models"));
    EXPECT_EQ(status["models"].size(), 4u);
    ASSERT_TRUE(status.isMember("predictions"));
    EXPECT_TRUE(std::isfinite(status["predictions"]["predicted_ahi"].asDouble()));

    // The freshly saved fixtures reload cleanly in a brand-new service instance,
    // closing the saveModels()->loadModels() round-trip over real trained output.
    auto db2 = std::make_shared<FakeDb>();
    db2->rows_ = db->rows_;
    MLTrainingService svc2(baseConfig(td.str()), db2, nullptr);
    svc2.start();
    auto status2 = svc2.getStatus();
    svc2.stop();
    EXPECT_TRUE(status2["models_loaded"].asBool());
}

// ---------------------------------------------------------------------------
// saveModels() write-failure path is covered implicitly above (success path);
// here we cover the early no-op when model_dir is empty: loadModels() must
// return false immediately without touching the filesystem.
// ---------------------------------------------------------------------------

TEST(MLTrainingServiceExtTest, StartWithEmptyModelDirDoesNotLoadModels) {
    MLTrainingService::Config cfg;
    cfg.enabled = true;          // enabled, but model_dir is empty
    cfg.schedule = "weekly";
    cfg.min_days = 7;
    cfg.device_id = "cpap_empty_dir";
    // cfg.model_dir intentionally left empty

    auto db = std::make_shared<FakeDb>();
    db->rows_ = makeDataset();
    MLTrainingService svc(cfg, db, nullptr);

    svc.start();   // loadModels() short-circuits on empty model_dir -> no load
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_FALSE(status["models_loaded"].asBool());
    EXPECT_FALSE(status.isMember("predictions"));
}