#include <gtest/gtest.h>

#include "services/MLTrainingService.h"
#include "database/IDatabase.h"
#include "ml/FeatureEngine.h"
#include "ml/RandomForest.h"
#include "ml/StandardScaler.h"

#include <nlohmann/json.hpp>
#include <json/json.h>

#include <chrono>
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
    bool connected_ = true;
    Json::Value rows_{Json::arrayValue};
    int query_count_ = 0;

    DbType dbType() const override { return DbType::POSTGRESQL; }

    bool connect() override { connected_ = true; return true; }
    void disconnect() override { connected_ = false; }
    bool isConnected() const override { return connected_; }

    Json::Value executeQuery(const std::string& /*sql*/,
                             const std::vector<std::string>& /*params*/ = {}) {
        ++query_count_;
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
