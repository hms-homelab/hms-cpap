#pragma once

#include "ml/RandomForest.h"
#include "ml/StandardScaler.h"
#include "ml/FeatureEngine.h"
#include "ml/CrossValidator.h"
#include "database/IDatabase.h"

#include <json/json.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declare to avoid including full MQTT headers
namespace hms { class MqttClient; }

namespace hms_cpap {

class MLTrainingService {
public:
    struct Config {
        bool enabled = false;
        std::string schedule = "weekly";
        std::string model_dir;
        int min_days = 30;
        int max_training_days = 0;  // 0 = all data
        std::string device_id;
    };

    struct ModelMetrics {
        std::string name;
        double primary_metric = 0;    // R2 or accuracy
        double secondary_metric = 0;  // MAE or F1
        int samples_used = 0;
    };

    struct TrainingResult {
        bool success = false;
        std::string timestamp;
        std::string error;
        std::vector<ModelMetrics> models;
    };

    struct Predictions {
        double predicted_ahi = 0;
        double predicted_hours = 0;
        double leak_risk_pct = 0;
        std::string anomaly_class = "NORMAL";
    };

    MLTrainingService(Config config,
                      std::shared_ptr<IDatabase> db,
                      std::shared_ptr<hms::MqttClient> mqtt);
    ~MLTrainingService();

    void start();
    void stop();

    void triggerTraining();
    TrainingResult getLastResult() const;
    Json::Value getStatus() const;

    // Run prediction on latest data
    Predictions predictLatest();

private:
    Config config_;
    std::shared_ptr<IDatabase> db_;
    std::shared_ptr<hms::MqttClient> mqtt_;

    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> train_requested_{false};

    // Models
    ml::RandomForest ahi_model_, mask_model_, compliance_model_, anomaly_model_;
    ml::StandardScaler ahi_scaler_, mask_scaler_, compliance_scaler_, anomaly_scaler_;
    std::vector<std::string> ahi_features_, mask_features_, compliance_features_, anomaly_features_;
    bool models_loaded_ = false;
    mutable std::mutex models_mutex_;

    TrainingResult last_result_;
    Predictions last_predictions_;
    mutable std::mutex result_mutex_;
    std::chrono::system_clock::time_point last_train_time_;

    void runLoop();
    bool shouldTrainNow() const;
    std::vector<ml::DailyRecord> loadTrainingData();
    TrainingResult runTrainingPipeline();
    void saveModels();
    bool loadModels();
    void subscribeToCommands();

    // MQTT publishing
    void publishTrainingStatus(const std::string& status);
    void publishPredictions(const Predictions& preds);

    // Helpers
    std::string modelPath(const std::string& name) const;
    static std::string currentTimestamp();
};

}  // namespace hms_cpap
