// #77: the AI summary prompt is one file, the one the Settings editor saves.
//
// A path set in Settings (llm.prompt_file) reached config.json but never the
// summary generator, which read only LLM_PROMPT_FILE, once, at startup. Both
// now go through AppConfig::llmPromptPath, and the generator reads the file
// at every summary.
#include <gtest/gtest.h>

#include "PosixCompat.h"
#include "services/BurstCollectorService.h"
#include "utils/AppConfig.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using hms_cpap::AppConfig;
using hms_cpap::BurstCollectorService;
namespace fs = std::filesystem;

namespace {

// Each test starts with neither variable set, and puts both back after.
class SummaryPrompt : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("summary-prompt-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir_);
        saved_env_ = read("LLM_PROMPT_FILE");
        saved_data_ = read("HMS_CPAP_DATA_DIR");
        unsetenv("LLM_PROMPT_FILE");
        setenv("HMS_CPAP_DATA_DIR", dir_.string().c_str(), 1);
    }
    void TearDown() override {
        restore("LLM_PROMPT_FILE", saved_env_);
        restore("HMS_CPAP_DATA_DIR", saved_data_);
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    static std::string read(const char* name) {
        const char* v = std::getenv(name);
        return v ? v : "";
    }
    static void restore(const char* name, const std::string& v) {
        if (v.empty()) unsetenv(name);
        else setenv(name, v.c_str(), 1);
    }
    static void write(const fs::path& p, const std::string& text) {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << text;
    }

    fs::path dir_;
    std::string saved_env_, saved_data_;
};

}  // namespace

TEST_F(SummaryPrompt, APathSetInSettingsIsTheOneUsed) {
    AppConfig cfg;
    cfg.llm.prompt_file = (dir_ / "mine.txt").string();
    setenv("LLM_PROMPT_FILE", (dir_ / "env.txt").string().c_str(), 1);
    EXPECT_EQ(AppConfig::llmPromptPath(&cfg), cfg.llm.prompt_file);
}

TEST_F(SummaryPrompt, WithNoPathTheEnvironmentThenTheDataFolder) {
    AppConfig cfg;
    EXPECT_EQ(AppConfig::llmPromptPath(&cfg), (dir_ / "llm_prompt.txt").string())
        << "the file the Settings editor saves when no path is set";
    setenv("LLM_PROMPT_FILE", (dir_ / "env.txt").string().c_str(), 1);
    EXPECT_EQ(AppConfig::llmPromptPath(nullptr), (dir_ / "env.txt").string());
}

TEST_F(SummaryPrompt, TheSettingsPathIsReadAtTheNextSummaryWithNoRestart) {
    AppConfig cfg;
    cfg.llm.prompt_file = (dir_ / "llm_prompt.txt").string();
    write(cfg.llm.prompt_file, "first {metrics}");
    std::string source;
    EXPECT_EQ(BurstCollectorService::summaryPromptTemplate(&cfg, &source), "first {metrics}");
    EXPECT_EQ(source, cfg.llm.prompt_file);

    write(cfg.llm.prompt_file, "edited {metrics}");
    EXPECT_EQ(BurstCollectorService::summaryPromptTemplate(&cfg), "edited {metrics}");
}

TEST_F(SummaryPrompt, NoFileOrAnEmptyOneGivesTheBuiltInPromptAndSaysWhy) {
    AppConfig cfg;
    std::string source;
    const std::string built_in = BurstCollectorService::summaryPromptTemplate(&cfg, &source);
    EXPECT_NE(built_in.find("{metrics}"), std::string::npos);
    EXPECT_NE(source.find("built-in"), std::string::npos);
    EXPECT_NE(source.find((dir_ / "llm_prompt.txt").string()), std::string::npos)
        << "the log names the file it looked for";

    write(dir_ / "llm_prompt.txt", "");
    EXPECT_EQ(BurstCollectorService::summaryPromptTemplate(&cfg), built_in);
}
