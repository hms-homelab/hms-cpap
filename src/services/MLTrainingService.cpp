#include "services/MLTrainingService.h"
#include "mqtt_client.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace hms_cpap {

// ---------------------------------------------------------------------------
// Anomaly class labels (aligned with FeatureEngine::Result::anomaly_labels)
// ---------------------------------------------------------------------------
static const std::vector<std::string> kAnomalyLabels = {
    "NORMAL", "AHI_ANOMALY", "LEAK_ANOMALY", "PRESSURE_ANOMALY"
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MLTrainingService::MLTrainingService(Config config,
                                     std::shared_ptr<IDatabase> db,
                                     std::shared_ptr<hms::MqttClient> mqtt)
    : config_(std::move(config)),
      db_(std::move(db)),
      mqtt_(std::move(mqtt)),
      last_train_time_(std::chrono::system_clock::time_point{}) {}

MLTrainingService::~MLTrainingService() {
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void MLTrainingService::start() {
    if (!config_.enabled) {
        spdlog::info("MLTrainingService: disabled by config");
        return;
    }

    // Ensure model directory exists
    if (!config_.model_dir.empty()) {
        std::error_code ec;
        fs::create_directories(config_.model_dir, ec);
        if (ec) {
            spdlog::error("MLTrainingService: failed to create model_dir '{}': {}",
                          config_.model_dir, ec.message());
        }
    }

    // Try to load existing models from disk
    if (loadModels()) {
        spdlog::info("MLTrainingService: loaded 4 models from {}", config_.model_dir);
    } else {
        spdlog::info("MLTrainingService: no existing models found — will train on first run");
    }

    // Subscribe to MQTT commands
    subscribeToCommands();

    // Spawn worker thread
    running_ = true;
    worker_thread_ = std::thread(&MLTrainingService::runLoop, this);
    spdlog::info("MLTrainingService: started (schedule={})", config_.schedule);
}

void MLTrainingService::stop() {
    if (!running_) return;
    running_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    spdlog::info("MLTrainingService: stopped");
}

void MLTrainingService::triggerTraining() {
    train_requested_ = true;
    spdlog::info("MLTrainingService: manual training triggered");
}

// ---------------------------------------------------------------------------
// Status accessors
// ---------------------------------------------------------------------------

MLTrainingService::TrainingResult MLTrainingService::getLastResult() const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    return last_result_;
}

Json::Value MLTrainingService::getStatus() const {
    Json::Value status;

    if (!config_.enabled) {
        status["status"] = "not_configured";
        return status;
    }

    {
        std::lock_guard<std::mutex> lock(models_mutex_);
        status["models_loaded"] = models_loaded_;
    }

    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        if (last_result_.timestamp.empty()) {
            status["status"] = "idle";
            status["last_trained"] = Json::nullValue;
        } else {
            status["status"] = last_result_.success ? "idle" : "error";
            status["last_trained"] = last_result_.timestamp;
            if (!last_result_.error.empty()) {
                status["last_error"] = last_result_.error;
            }

            Json::Value models_arr(Json::arrayValue);
            for (const auto& m : last_result_.models) {
                Json::Value mj;
                mj["name"] = m.name;
                mj["primary_metric"] = m.primary_metric;
                mj["secondary_metric"] = m.secondary_metric;
                mj["samples_used"] = m.samples_used;
                models_arr.append(mj);
            }
            status["models"] = models_arr;
        }
    }

    status["schedule"] = config_.schedule;
    status["min_days"] = config_.min_days;

    // Include last predictions
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        if (last_predictions_.predicted_ahi > 0 || last_predictions_.predicted_hours > 0) {
            Json::Value preds;
            preds["predicted_ahi"] = last_predictions_.predicted_ahi;
            preds["predicted_hours"] = last_predictions_.predicted_hours;
            preds["leak_risk_pct"] = last_predictions_.leak_risk_pct;
            preds["anomaly_class"] = last_predictions_.anomaly_class;
            status["predictions"] = preds;
        }
    }

    return status;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void MLTrainingService::runLoop() {
    spdlog::info("MLTrainingService: worker thread running");

    while (running_) {
        // Sleep in 1-second increments so we can react to stop quickly
        for (int i = 0; i < 60 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_) break;

        bool should_train = train_requested_.exchange(false) || shouldTrainNow();
        if (!should_train) continue;

        spdlog::info("MLTrainingService: starting training pipeline");
        publishTrainingStatus("training");

        try {
            auto result = runTrainingPipeline();

            {
                std::lock_guard<std::mutex> lock(result_mutex_);
                last_result_ = result;
            }

            if (result.success) {
                saveModels();
                last_train_time_ = std::chrono::system_clock::now();
                spdlog::info("MLTrainingService: training complete — {} models trained",
                             result.models.size());

                for (const auto& m : result.models) {
                    spdlog::info("  {} — primary={:.4f} secondary={:.4f} samples={}",
                                 m.name, m.primary_metric, m.secondary_metric, m.samples_used);
                }

                publishTrainingStatus("complete");

                // Run predictions after successful training
                auto preds = predictLatest();
                publishPredictions(preds);
            } else {
                spdlog::error("MLTrainingService: training failed — {}", result.error);
                publishTrainingStatus("error");
            }
        } catch (const std::exception& e) {
            spdlog::error("MLTrainingService: training exception — {}", e.what());

            TrainingResult err_result;
            err_result.success = false;
            err_result.timestamp = currentTimestamp();
            err_result.error = e.what();
            {
                std::lock_guard<std::mutex> lock(result_mutex_);
                last_result_ = err_result;
            }
            publishTrainingStatus("error");
        }
    }

    spdlog::info("MLTrainingService: worker thread exiting");
}

bool MLTrainingService::shouldTrainNow() const {
    auto now = std::chrono::system_clock::now();

    // Never trained and no models on disk
    if (last_train_time_.time_since_epoch().count() == 0 && !models_loaded_) {
        return true;
    }

    // Determine interval from schedule
    std::chrono::hours interval(168);  // default: weekly
    if (config_.schedule == "daily") {
        interval = std::chrono::hours(24);
    } else if (config_.schedule == "weekly") {
        interval = std::chrono::hours(168);
    } else if (config_.schedule == "monthly") {
        interval = std::chrono::hours(720);
    }

    return (now - last_train_time_) >= interval;
}

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------

// executeQuery returns values as strings; safely convert to double
static double jsonToDouble(const Json::Value& row, const char* key) {
    const auto& v = row[key];
    if (v.isNull()) return 0.0;
    if (v.isDouble() || v.isInt()) return v.asDouble();
    if (v.isString()) {
        try { return std::stod(v.asString()); }
        catch (...) { return 0.0; }
    }
    return 0.0;
}

std::vector<ml::DailyRecord> MLTrainingService::loadTrainingData() {
    std::vector<ml::DailyRecord> records;

    if (!db_ || !db_->isConnected()) {
        spdlog::error("MLTrainingService: database not connected");
        return records;
    }

    std::string sql =
        "SELECT record_date, duration_minutes, ahi, hi, oai, cai, "
        "mask_press_50, mask_press_95, mask_press_max, "
        "leak_50, leak_95, leak_max, "
        "resp_rate_50, tid_vol_50, min_vent_50 "
        "FROM cpap_daily_summary "
        "WHERE duration_minutes > 60 ";

    if (config_.max_training_days > 0) {
        sql += "AND record_date >= CURRENT_DATE - INTERVAL '" +
               std::to_string(config_.max_training_days) + " days' ";
    }
    sql += "ORDER BY record_date";

    auto rows = db_->executeQuery(sql);

    for (const auto& row : rows) {
        ml::DailyRecord rec;
        rec.record_date     = row.get("record_date", "").asString();
        rec.duration_minutes = jsonToDouble(row, "duration_minutes");
        rec.ahi             = jsonToDouble(row, "ahi");
        rec.hi              = jsonToDouble(row, "hi");
        rec.oai             = jsonToDouble(row, "oai");
        rec.cai             = jsonToDouble(row, "cai");
        rec.mask_press_50   = jsonToDouble(row, "mask_press_50");
        rec.mask_press_95   = jsonToDouble(row, "mask_press_95");
        rec.mask_press_max  = jsonToDouble(row, "mask_press_max");
        rec.leak_50         = jsonToDouble(row, "leak_50");
        rec.leak_95         = jsonToDouble(row, "leak_95");
        rec.leak_max        = jsonToDouble(row, "leak_max");
        rec.resp_rate_50    = jsonToDouble(row, "resp_rate_50");
        rec.tid_vol_50      = jsonToDouble(row, "tid_vol_50");
        rec.min_vent_50     = jsonToDouble(row, "min_vent_50");

        // Parse day_of_week from record_date (YYYY-MM-DD)
        if (rec.record_date.size() >= 10) {
            std::tm tm = {};
            std::istringstream ss(rec.record_date);
            ss >> std::get_time(&tm, "%Y-%m-%d");
            if (!ss.fail()) {
                std::mktime(&tm);  // fills in tm_wday
                // tm_wday: 0=Sun..6=Sat -> convert to 0=Mon..6=Sun
                rec.day_of_week = (tm.tm_wday + 6) % 7;
            }
        }

        records.push_back(std::move(rec));
    }

    spdlog::info("MLTrainingService: loaded {} daily records from DB", records.size());
    return records;
}

// ---------------------------------------------------------------------------
// Training pipeline
// ---------------------------------------------------------------------------

MLTrainingService::TrainingResult MLTrainingService::runTrainingPipeline() {
    TrainingResult result;
    result.timestamp = currentTimestamp();

    // Load data
    auto data = loadTrainingData();
    if (static_cast<int>(data.size()) < config_.min_days) {
        result.error = "Insufficient data: " + std::to_string(data.size()) +
                       " days (need " + std::to_string(config_.min_days) + ")";
        return result;
    }

    // Build features
    auto fe = ml::FeatureEngine::build(data);
    if (fe.X.empty()) {
        result.error = "FeatureEngine produced empty feature matrix";
        return result;
    }

    int n_samples = static_cast<int>(fe.X.size());
    spdlog::info("MLTrainingService: {} samples, {} features", n_samples, fe.feature_names.size());

    // --- 1. AHI Forecaster (regression) ---
    {
        ml::RandomForest::Params p;
        p.n_estimators = 200;
        p.max_depth = 8;
        p.min_samples_split = 5;
        p.min_samples_leaf = 3;

        auto cv = ml::CrossValidator::cvRegression(fe.X, fe.target_ahi, p, 5);
        spdlog::info("  AHI Forecaster CV: R2={:.4f}+/-{:.4f} MAE={:.4f}",
                     cv.r2_mean, cv.r2_std, cv.mae_mean);

        ml::StandardScaler scaler;
        auto X_scaled = scaler.fitTransform(fe.X);

        ml::RandomForest rf(p);
        rf.fitRegression(X_scaled, fe.target_ahi);

        std::lock_guard<std::mutex> lock(models_mutex_);
        ahi_model_ = std::move(rf);
        ahi_scaler_ = std::move(scaler);
        ahi_features_ = fe.feature_names;

        ModelMetrics mm;
        mm.name = "ahi_forecaster";
        mm.primary_metric = cv.r2_mean;
        mm.secondary_metric = cv.mae_mean;
        mm.samples_used = n_samples;
        result.models.push_back(mm);
    }

    // --- 2. Mask Fit Predictor (classification, 2 classes) ---
    {
        ml::RandomForest::Params p;
        p.n_estimators = 200;
        p.max_depth = 6;
        p.min_samples_split = 5;
        p.min_samples_leaf = 3;
        p.class_weight_balanced = true;

        int n_classes = 2;
        auto cv = ml::CrossValidator::cvClassification(fe.X, fe.target_high_leak, n_classes, p, 5);
        spdlog::info("  Mask Fit Predictor CV: Acc={:.4f}+/-{:.4f} F1={:.4f}",
                     cv.accuracy_mean, cv.accuracy_std, cv.f1_mean);

        ml::StandardScaler scaler;
        auto X_scaled = scaler.fitTransform(fe.X);

        ml::RandomForest rf(p);
        rf.fitClassification(X_scaled, fe.target_high_leak, n_classes);

        std::lock_guard<std::mutex> lock(models_mutex_);
        mask_model_ = std::move(rf);
        mask_scaler_ = std::move(scaler);
        mask_features_ = fe.feature_names;

        ModelMetrics mm;
        mm.name = "mask_fit_predictor";
        mm.primary_metric = cv.accuracy_mean;
        mm.secondary_metric = cv.f1_mean;
        mm.samples_used = n_samples;
        result.models.push_back(mm);
    }

    // --- 3. Compliance Predictor (regression on hours) ---
    {
        ml::RandomForest::Params p;
        p.n_estimators = 200;
        p.max_depth = 8;
        p.min_samples_split = 5;
        p.min_samples_leaf = 3;

        auto cv = ml::CrossValidator::cvRegression(fe.X, fe.target_hours, p, 5);
        spdlog::info("  Compliance Predictor CV: R2={:.4f}+/-{:.4f} MAE={:.4f}",
                     cv.r2_mean, cv.r2_std, cv.mae_mean);

        ml::StandardScaler scaler;
        auto X_scaled = scaler.fitTransform(fe.X);

        ml::RandomForest rf(p);
        rf.fitRegression(X_scaled, fe.target_hours);

        std::lock_guard<std::mutex> lock(models_mutex_);
        compliance_model_ = std::move(rf);
        compliance_scaler_ = std::move(scaler);
        compliance_features_ = fe.feature_names;

        ModelMetrics mm;
        mm.name = "compliance_predictor";
        mm.primary_metric = cv.r2_mean;
        mm.secondary_metric = cv.mae_mean;
        mm.samples_used = n_samples;
        result.models.push_back(mm);
    }

    // --- 4. Anomaly Detector (classification, 4 classes) ---
    {
        ml::RandomForest::Params p;
        p.n_estimators = 200;
        p.max_depth = 8;
        p.min_samples_split = 5;
        p.min_samples_leaf = 3;
        p.class_weight_balanced = true;

        int n_classes = 4;
        auto cv = ml::CrossValidator::cvClassification(fe.X, fe.target_anomaly, n_classes, p, 5);
        spdlog::info("  Anomaly Detector CV: Acc={:.4f}+/-{:.4f} F1={:.4f}",
                     cv.accuracy_mean, cv.accuracy_std, cv.f1_mean);

        ml::StandardScaler scaler;
        auto X_scaled = scaler.fitTransform(fe.X);

        ml::RandomForest rf(p);
        rf.fitClassification(X_scaled, fe.target_anomaly, n_classes);

        std::lock_guard<std::mutex> lock(models_mutex_);
        anomaly_model_ = std::move(rf);
        anomaly_scaler_ = std::move(scaler);
        anomaly_features_ = fe.feature_names;

        ModelMetrics mm;
        mm.name = "anomaly_detector";
        mm.primary_metric = cv.accuracy_mean;
        mm.secondary_metric = cv.f1_mean;
        mm.samples_used = n_samples;
        result.models.push_back(mm);
    }

    {
        std::lock_guard<std::mutex> lock(models_mutex_);
        models_loaded_ = true;
    }

    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
// Model persistence
// ---------------------------------------------------------------------------

std::string MLTrainingService::modelPath(const std::string& name) const {
    return config_.model_dir + "/" + name + ".json";
}

void MLTrainingService::saveModels() {
    // Copy metrics under result_mutex_ first to avoid nested locking
    std::vector<ModelMetrics> metrics_copy;
    {
        std::lock_guard<std::mutex> rlock(result_mutex_);
        metrics_copy = last_result_.models;
    }

    auto find_metrics = [&metrics_copy](const std::string& name) -> const ModelMetrics* {
        for (const auto& m : metrics_copy) {
            if (m.name == name) return &m;
        }
        return nullptr;
    };

    std::lock_guard<std::mutex> lock(models_mutex_);

    auto save_one = [this](const std::string& name,
                           const ml::RandomForest& rf,
                           const ml::StandardScaler& scaler,
                           const std::vector<std::string>& features,
                           const ModelMetrics* metrics) {
        nlohmann::json j;
        j["model"] = rf.toJson();
        j["scaler"] = scaler.toJson();
        j["features"] = features;
        j["trained_at"] = currentTimestamp();
        if (metrics) {
            j["metrics"] = {
                {"name", metrics->name},
                {"primary_metric", metrics->primary_metric},
                {"secondary_metric", metrics->secondary_metric},
                {"samples_used", metrics->samples_used}
            };
        }

        std::string path = modelPath(name);
        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            spdlog::error("MLTrainingService: failed to write model file '{}'", path);
            return;
        }
        ofs << j.dump(2);
        spdlog::info("MLTrainingService: saved model '{}'", path);
    };

    save_one("ahi_forecaster", ahi_model_, ahi_scaler_, ahi_features_,
             find_metrics("ahi_forecaster"));
    save_one("mask_fit_predictor", mask_model_, mask_scaler_, mask_features_,
             find_metrics("mask_fit_predictor"));
    save_one("compliance_predictor", compliance_model_, compliance_scaler_, compliance_features_,
             find_metrics("compliance_predictor"));
    save_one("anomaly_detector", anomaly_model_, anomaly_scaler_, anomaly_features_,
             find_metrics("anomaly_detector"));
}

bool MLTrainingService::loadModels() {
    if (config_.model_dir.empty()) return false;

    auto load_one = [this](const std::string& name,
                           ml::RandomForest& rf,
                           ml::StandardScaler& scaler,
                           std::vector<std::string>& features) -> bool {
        std::string path = modelPath(name);
        if (!fs::exists(path)) return false;

        std::ifstream ifs(path);
        if (!ifs.is_open()) return false;

        try {
            nlohmann::json j = nlohmann::json::parse(ifs);
            rf = ml::RandomForest::fromJson(j.at("model"));
            scaler = ml::StandardScaler::fromJson(j.at("scaler"));
            features = j.at("features").get<std::vector<std::string>>();

            // Restore last_train_time_ from the saved timestamp
            if (j.contains("trained_at")) {
                // Just mark that we have a valid model; exact time not critical
                // since shouldTrainNow() will use schedule-based checks
            }
            return true;
        } catch (const std::exception& e) {
            spdlog::warn("MLTrainingService: failed to load model '{}': {}", path, e.what());
            return false;
        }
    };

    std::lock_guard<std::mutex> lock(models_mutex_);
    bool ok = true;
    ok &= load_one("ahi_forecaster", ahi_model_, ahi_scaler_, ahi_features_);
    ok &= load_one("mask_fit_predictor", mask_model_, mask_scaler_, mask_features_);
    ok &= load_one("compliance_predictor", compliance_model_, compliance_scaler_, compliance_features_);
    ok &= load_one("anomaly_detector", anomaly_model_, anomaly_scaler_, anomaly_features_);

    if (ok) {
        models_loaded_ = true;
        // Set last_train_time_ so we don't immediately retrain
        last_train_time_ = std::chrono::system_clock::now();
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Prediction
// ---------------------------------------------------------------------------

MLTrainingService::Predictions MLTrainingService::predictLatest() {
    Predictions preds;

    {
        std::lock_guard<std::mutex> lock(models_mutex_);
        if (!models_loaded_) {
            spdlog::warn("MLTrainingService: predictLatest() called but models not loaded");
            return preds;
        }
    }

    // Load recent data (last 30 days) for feature engineering
    auto data = loadTrainingData();
    if (data.size() < 7) {
        spdlog::warn("MLTrainingService: not enough data for prediction ({} days)", data.size());
        return preds;
    }

    // Build features — FeatureEngine uses the last record's features for prediction
    auto fe = ml::FeatureEngine::build(data);
    if (fe.X.empty()) {
        spdlog::warn("MLTrainingService: FeatureEngine produced empty matrix for prediction");
        return preds;
    }

    // Use the last row (most recent day's features)
    auto last_row = fe.X.back();

    std::lock_guard<std::mutex> lock(models_mutex_);

    try {
        // AHI prediction
        auto scaled = ahi_scaler_.transformRow(last_row);
        preds.predicted_ahi = ahi_model_.predict(scaled);

        // Compliance prediction (hours)
        scaled = compliance_scaler_.transformRow(last_row);
        preds.predicted_hours = compliance_model_.predict(scaled);

        // Mask fit / leak risk
        scaled = mask_scaler_.transformRow(last_row);
        auto proba = mask_model_.predictProba(scaled);
        preds.leak_risk_pct = (proba.size() >= 2) ? proba[1] * 100.0 : 0.0;

        // Anomaly detection
        scaled = anomaly_scaler_.transformRow(last_row);
        int anomaly_class = anomaly_model_.predictClass(scaled);
        if (anomaly_class >= 0 && anomaly_class < static_cast<int>(kAnomalyLabels.size())) {
            preds.anomaly_class = kAnomalyLabels[anomaly_class];
        }
    } catch (const std::exception& e) {
        spdlog::error("MLTrainingService: prediction error — {}", e.what());
    }

    spdlog::info("MLTrainingService: predictions — AHI={:.2f} Hours={:.1f} LeakRisk={:.1f}% Anomaly={}",
                 preds.predicted_ahi, preds.predicted_hours,
                 preds.leak_risk_pct, preds.anomaly_class);

    // Store for status endpoint
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        last_predictions_ = preds;
    }

    return preds;
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------

void MLTrainingService::subscribeToCommands() {
    if (!mqtt_ || !mqtt_->isConnected()) return;

    // Train command
    std::string train_topic = "cpap/" + config_.device_id + "/command/train_models";
    mqtt_->subscribe(train_topic,
        [this](const std::string& /*topic*/, const std::string& /*payload*/) {
            spdlog::info("MLTrainingService: train command received via MQTT");
            train_requested_ = true;
        });

    // Predict on session completion
    std::string complete_topic = "cpap/" + config_.device_id + "/session/completed";
    mqtt_->subscribe(complete_topic,
        [this](const std::string& /*topic*/, const std::string& /*payload*/) {
            spdlog::info("MLTrainingService: session completed — running prediction");
            try {
                auto preds = predictLatest();
                publishPredictions(preds);
            } catch (const std::exception& e) {
                spdlog::error("MLTrainingService: prediction after session failed — {}", e.what());
            }
        });

    spdlog::info("MLTrainingService: subscribed to MQTT commands");
}

void MLTrainingService::publishTrainingStatus(const std::string& status) {
    if (!mqtt_ || !mqtt_->isConnected()) return;

    nlohmann::json j;
    j["status"] = status;
    j["timestamp"] = currentTimestamp();

    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        if (!last_result_.error.empty() && status == "error") {
            j["error"] = last_result_.error;
        }
        if (!last_result_.models.empty()) {
            nlohmann::json models_arr = nlohmann::json::array();
            for (const auto& m : last_result_.models) {
                models_arr.push_back({
                    {"name", m.name},
                    {"primary_metric", m.primary_metric},
                    {"secondary_metric", m.secondary_metric},
                    {"samples_used", m.samples_used}
                });
            }
            j["models"] = models_arr;
        }
    }

    std::string topic = "cpap/" + config_.device_id + "/ml/status";
    mqtt_->publish(topic, j.dump(), 1, true);
}

void MLTrainingService::publishPredictions(const Predictions& preds) {
    if (!mqtt_ || !mqtt_->isConnected()) return;

    nlohmann::json j;
    j["predicted_ahi"] = preds.predicted_ahi;
    j["predicted_hours"] = preds.predicted_hours;
    j["leak_risk_pct"] = preds.leak_risk_pct;
    j["anomaly_class"] = preds.anomaly_class;
    j["timestamp"] = currentTimestamp();

    std::string topic = "cpap/" + config_.device_id + "/ml/predictions";
    mqtt_->publish(topic, j.dump(), 1, true);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string MLTrainingService::currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

}  // namespace hms_cpap
