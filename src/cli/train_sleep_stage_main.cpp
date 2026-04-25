/**
 * HMS-CPAP Sleep Stage Training CLI
 *
 * Standalone binary that trains causal + bidirectional RandomForest models
 * for sleep stage classification from feature CSVs.
 *
 * Usage:
 *   hms_cpap_train_sleep_stage \
 *       --features-bidir features_bidir.csv \
 *       --features-causal features_causal.csv \
 *       --output-dir ~/.hms-cpap/models \
 *       --n-trees 100 --max-depth 14 --cv-folds 5 --seed 42
 */

#include "ml/SleepStageTrainer.h"
#include "ml/SleepStageTypes.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace hms_cpap::ml;

// ── Arg parsing helpers ────────────────────────────────────────────────────

struct CliArgs {
    std::string features_bidir;
    std::string features_causal;
    std::string output_dir = "models";
    int n_trees = 100;
    int max_depth = 14;
    int cv_folds = 5;
    int seed = 42;
};

static void printUsage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "Required:\n"
        << "  --features-bidir  PATH   Bidirectional features CSV\n"
        << "  --features-causal PATH   Causal features CSV\n"
        << "\n"
        << "Optional:\n"
        << "  --output-dir DIR         Model output directory  [models]\n"
        << "  --n-trees N              Number of trees          [100]\n"
        << "  --max-depth N            Max tree depth            [14]\n"
        << "  --cv-folds N             Cross-validation folds    [5]\n"
        << "  --seed N                 Random seed               [42]\n"
        << "  --help                   Show this help\n";
}

static bool parseArgs(int argc, char* argv[], CliArgs& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        }
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                spdlog::error("Missing value for {}", arg);
                std::exit(1);
            }
            return argv[++i];
        };

        if (arg == "--features-bidir")       args.features_bidir = next();
        else if (arg == "--features-causal") args.features_causal = next();
        else if (arg == "--output-dir")      args.output_dir = next();
        else if (arg == "--n-trees")         args.n_trees = std::stoi(next());
        else if (arg == "--max-depth")       args.max_depth = std::stoi(next());
        else if (arg == "--cv-folds")        args.cv_folds = std::stoi(next());
        else if (arg == "--seed")            args.seed = std::stoi(next());
        else {
            spdlog::error("Unknown argument: {}", arg);
            printUsage(argv[0]);
            return false;
        }
    }

    if (args.features_bidir.empty() || args.features_causal.empty()) {
        spdlog::error("Both --features-bidir and --features-causal are required");
        printUsage(argv[0]);
        return false;
    }
    return true;
}

// ── Printing helpers ───────────────────────────────────────────────────────

static void printClassDistribution(const std::vector<int>& y, const std::string& label) {
    std::map<int, int> counts;
    for (int v : y) ++counts[v];

    spdlog::info("{} dataset: {} epochs", label, y.size());
    const char* names[] = {"Wake", "Light", "Deep", "REM"};
    for (auto& [cls, cnt] : counts) {
        double pct = 100.0 * cnt / static_cast<double>(y.size());
        const char* name = (cls >= 0 && cls <= 3) ? names[cls] : "Unknown";
        spdlog::info("  {} ({}): {} ({:.1f}%)", name, cls, cnt, pct);
    }
}

static void printConfusionMatrix(const std::vector<std::vector<int>>& cm) {
    const char* names[] = {"Wake ", "Light", "Deep ", "REM  "};
    int n = static_cast<int>(cm.size());

    std::cout << "\nConfusion Matrix (rows=actual, cols=predicted):\n";
    std::cout << "         ";
    for (int j = 0; j < n; ++j) std::cout << std::setw(7) << names[j];
    std::cout << "\n";

    for (int i = 0; i < n; ++i) {
        std::cout << "  " << names[i] << " ";
        for (int j = 0; j < n; ++j) {
            std::cout << std::setw(7) << cm[i][j];
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

static void printMetrics(const TrainingMetrics& m,
                         const std::string& label) {
    spdlog::info("--- {} Metrics ---", label);
    spdlog::info("  Accuracy:   {:.4f}", m.accuracy);
    spdlog::info("  Macro F1:   {:.4f}", m.macro_f1);
    spdlog::info("  Wake  F1:   {:.4f}  recall: {:.4f}", m.wake_f1, m.wake_recall);
    spdlog::info("  Light F1:   {:.4f}  recall: {:.4f}", m.light_f1, m.light_recall);
    spdlog::info("  Deep  F1:   {:.4f}  recall: {:.4f}", m.deep_f1, m.deep_recall);
    spdlog::info("  REM   F1:   {:.4f}  recall: {:.4f}", m.rem_f1, m.rem_recall);
    spdlog::info("  Samples:    {}", m.n_samples);

    if (!m.confusion_matrix.empty()) {
        printConfusionMatrix(m.confusion_matrix);
    }
}

static void printFeatureImportances(const SleepStageTrainer::Result& result,
                                    FeatureMode mode) {
    auto importances = result.model.featureImportances();
    auto names = sleepStageFeatureNames(mode);

    // Pair up and sort descending
    std::vector<std::pair<double, std::string>> ranked;
    for (size_t i = 0; i < importances.size() && i < names.size(); ++i) {
        ranked.emplace_back(importances[i], names[i]);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    spdlog::info("Top 10 feature importances:");
    int n = std::min(10, static_cast<int>(ranked.size()));
    for (int i = 0; i < n; ++i) {
        spdlog::info("  {:2d}. {:<30s} {:.4f}", i + 1, ranked[i].second, ranked[i].first);
    }
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Set up colored console logger
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    CliArgs args;
    if (!parseArgs(argc, argv, args)) return 1;

    spdlog::info("HMS-CPAP Sleep Stage Trainer");
    spdlog::info("  Bidir CSV:  {}", args.features_bidir);
    spdlog::info("  Causal CSV: {}", args.features_causal);
    spdlog::info("  Output:     {}", args.output_dir);
    spdlog::info("  Trees: {}  Depth: {}  CV folds: {}  Seed: {}",
                 args.n_trees, args.max_depth, args.cv_folds, args.seed);

    // ── Load bidirectional features ────────────────────────────────────────
    std::vector<std::vector<double>> X_bidir;
    std::vector<int> y_bidir;

    spdlog::info("Loading bidirectional features from {}", args.features_bidir);
    if (!SleepStageTrainer::loadCSV(args.features_bidir, X_bidir, y_bidir)) {
        spdlog::error("Failed to load bidirectional CSV: {}", args.features_bidir);
        return 1;
    }
    printClassDistribution(y_bidir, "Bidirectional");

    // ── Load causal features ───────────────────────────────────────────────
    std::vector<std::vector<double>> X_causal;
    std::vector<int> y_causal;

    spdlog::info("Loading causal features from {}", args.features_causal);
    if (!SleepStageTrainer::loadCSV(args.features_causal, X_causal, y_causal)) {
        spdlog::error("Failed to load causal CSV: {}", args.features_causal);
        return 1;
    }
    printClassDistribution(y_causal, "Causal");

    // ── Training config ────────────────────────────────────────────────────
    TrainingConfig config;
    config.rf_params.n_estimators = args.n_trees;
    config.rf_params.max_depth = args.max_depth;
    config.rf_params.random_seed = args.seed;
    config.cv_folds = args.cv_folds;

    // ── Train bidirectional model ──────────────────────────────────────────
    spdlog::info("Training bidirectional model...");
    auto bidir_result = SleepStageTrainer::train(X_bidir, y_bidir, config);
    if (!bidir_result.success) {
        spdlog::error("Bidirectional training failed: {}", bidir_result.error);
        return 1;
    }
    printMetrics(bidir_result.metrics, "Bidirectional");
    printFeatureImportances(bidir_result, FeatureMode::BIDIRECTIONAL);

    // ── Train causal model ─────────────────────────────────────────────────
    spdlog::info("Training causal model...");
    auto causal_result = SleepStageTrainer::train(X_causal, y_causal, config);
    if (!causal_result.success) {
        spdlog::error("Causal training failed: {}", causal_result.error);
        return 1;
    }
    printMetrics(causal_result.metrics, "Causal");
    printFeatureImportances(causal_result, FeatureMode::CAUSAL);

    // ── Save model bundles ─────────────────────────────────────────────────
    spdlog::info("Saving models to {}", args.output_dir);

    if (!SleepStageTrainer::saveBundle(args.output_dir, "sleep_stage_bidir", bidir_result)) {
        spdlog::error("Failed to save bidirectional model bundle");
        return 1;
    }
    spdlog::info("Saved: sleep_stage_bidir");

    if (!SleepStageTrainer::saveBundle(args.output_dir, "sleep_stage_causal", causal_result)) {
        spdlog::error("Failed to save causal model bundle");
        return 1;
    }
    spdlog::info("Saved: sleep_stage_causal");

    // ── Summary ────────────────────────────────────────────────────────────
    spdlog::info("Training complete.");
    spdlog::info("  Bidirectional: accuracy={:.4f} macro_f1={:.4f}",
                 bidir_result.metrics.accuracy, bidir_result.metrics.macro_f1);
    spdlog::info("  Causal:        accuracy={:.4f} macro_f1={:.4f}",
                 causal_result.metrics.accuracy, causal_result.metrics.macro_f1);

    return 0;
}
