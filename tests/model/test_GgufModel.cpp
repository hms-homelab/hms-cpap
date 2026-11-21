//
// SDD-013: the narrator's runner (GgufModel on llama.cpp) and its prompt
// contract.
//
// The default suite needs no model file: the prompt contract is plain string
// work, and the runner is checked with no file, a file that is not the
// published one, and a file that is not a model. The tests that run the real
// model are gated on HMS_CPAP_NARRATOR_MODEL naming narrator-v1 on this
// machine (the release asset of tag narrator-v1) and skip otherwise:
//
//   HMS_CPAP_NARRATOR_MODEL=/path/narrator-v1-q4_k_m.gguf ./run_tests --gtest_filter='Narrator*'
//
// The fixture, tests/fixtures/narrator/pi_set_sample.jsonl, is three of the
// 200 synthetic test nights the export was checked with on the Pi: the fact
// block, the prompt text, the ids sentencepiece gives that prompt (what
// training saw), and the HF model's greedy summary.
//
#include "model/LocalModel.h"
#include "model/ModelFile.h"
#include "model/NarrationService.h"

#include <gtest/gtest.h>
#include <json/json.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

using namespace hms_cpap::model;
namespace fs = std::filesystem;

namespace {

struct FixtureNight {
    std::string id, facts, prompt, hf;
    std::vector<std::int32_t> ids;
};

std::vector<FixtureNight> fixtureNights() {
    std::ifstream in(fs::path(HMS_CPAP_SOURCE_DIR) / "tests" / "fixtures" / "narrator" / "pi_set_sample.jsonl");
    EXPECT_TRUE(in.good()) << "the narrator fixture is missing";
    std::vector<FixtureNight> out;
    std::string line;
    Json::CharReaderBuilder rb;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Json::Value v;
        std::string err;
        std::istringstream s(line);
        EXPECT_TRUE(Json::parseFromStream(rb, s, &v, &err)) << err;
        FixtureNight n{v["id"].asString(), v["facts"].asString(), v["prompt"].asString(), v["hf"].asString(), {}};
        for (const auto& id : v["ids"]) n.ids.push_back(id.asInt());
        out.push_back(std::move(n));
    }
    return out;
}

std::string tempFile(const std::string& name, const std::string& bytes) {
    const auto dir = fs::temp_directory_path() /
                     ("hms_sdd013_gguf_" + std::to_string(::getpid()) + "_" +
                      ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::create_directories(dir);
    const auto p = dir / name;
    std::ofstream(p, std::ios::binary) << bytes;
    return p.string();
}

/// The real model, when this machine has it; "" otherwise.
std::string realModel() {
    const char* p = std::getenv("HMS_CPAP_NARRATOR_MODEL");
    if (!p || !*p) return "";
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? std::string(p) : std::string();
}

}  // namespace

// -- The prompt contract ---------------------------------------------------------

TEST(NarratorPrompt, IsTheBlockOnOneLineAfterASpaceAndBeforeTheSeparator) {
    EXPECT_EQ(narratorPrompt("Night: 2099-07-01\nUsage: 6.25 hours\n"),
              " Night: 2099-07-01<n>Usage: 6.25 hours<n><|summary|>");
    EXPECT_EQ(narratorPrompt(""), " <|summary|>");
    EXPECT_EQ(narratorPrompt("a\n\nb"), " a<n><n>b<|summary|>") << "an empty line is kept";
}

TEST(NarratorPrompt, IsWhatTheExportWasCheckedWithForEveryFixtureNight) {
    const auto nights = fixtureNights();
    ASSERT_EQ(nights.size(), 3u);
    for (const auto& n : nights) EXPECT_EQ(narratorPrompt(n.facts), n.prompt) << n.id;
}

TEST(NarratorPrompt, TheModelsTextHasItsLineBreaksBackAndNoEdgeSpace) {
    EXPECT_EQ(narratorText(" On 2099-07-01 you slept.<n>AHI was 1.00. "), "On 2099-07-01 you slept.\nAHI was 1.00.");
    EXPECT_EQ(narratorText("   "), "");
    EXPECT_EQ(narratorText("plain"), "plain");
}

TEST(NarratorPrompt, TheCallerHandsOverTheFactBlockNotAPrompt) {
    // NarrationService gives the model the fact block and checks the text
    // against that same block; the framing is the backend's alone.
    class Recorder : public ILocalModel {
    public:
        std::string given;
        bool ready() const override { return true; }
        std::string whyNotReady() const override { return ""; }
        std::optional<std::string> generate(const std::string& facts, const GenerateParams&) override {
            given = facts;
            return std::string("Night of 2099-07-01.");
        }
    } rec;
    hms_cpap::NightFacts night;
    night.sleep_day = "2099-07-01";
    night.usage_hours = 6.25;
    narrate(night, rec);
    EXPECT_EQ(rec.given, factBlock(night));
    EXPECT_EQ(rec.given.find("<|summary|>"), std::string::npos);
    EXPECT_EQ(rec.given.find("<n>"), std::string::npos);
}

// -- The runner, no model needed -------------------------------------------------

TEST(NarratorRunner, TheRuntimeIsLinkedUnlessTheBuildDropsIt) {
#ifdef HMS_WITH_LOCAL_MODEL
    EXPECT_TRUE(GgufModel::runtimeAvailable());
#else
    EXPECT_FALSE(GgufModel::runtimeAvailable()) << "built with BUILD_WITH_LOCAL_MODEL=OFF";
#endif
}

TEST(NarratorRunner, WithNoFileItIsNotReadyAndSaysWhy) {
    GgufModel none((fs::temp_directory_path() / "hms_sdd013_absent" / "model.gguf").string(),
                   kNarratorModelSha256);
    EXPECT_FALSE(none.ready());
    EXPECT_EQ(none.whyNotReady(), "No model file is installed.");
    EXPECT_FALSE(none.generate("Night: 2099-07-01\n", {}).has_value());
    EXPECT_TRUE(none.tokenize(" Night<|summary|>").empty());

    GgufModel unnamed("");
    EXPECT_FALSE(unnamed.ready());
    EXPECT_EQ(unnamed.whyNotReady(), "No model file is installed.");
}

TEST(NarratorRunner, AFileThatIsNotThePublishedOneIsNotRun) {
    const auto path = tempFile("model.gguf", "GGUF but not the model");
    GgufModel wrong(path, kNarratorModelSha256);
    EXPECT_FALSE(wrong.ready());
    if (GgufModel::runtimeAvailable())
        EXPECT_NE(wrong.whyNotReady().find("SHA-256"), std::string::npos) << wrong.whyNotReady();
    else
        EXPECT_NE(wrong.whyNotReady().find("runtime"), std::string::npos) << wrong.whyNotReady();
    EXPECT_FALSE(wrong.generate("Night: 2099-07-01\n", {}).has_value());
    fs::remove_all(fs::path(path).parent_path());
}

TEST(NarratorRunner, AFileThatIsNotAModelFailsToLoadAndSaysSo) {
    if (!GgufModel::runtimeAvailable()) GTEST_SKIP() << "no runtime in this build";
    const auto path = tempFile("model.gguf", "GGUF but not the model");
    GgufModel junk(path);   // no hash to hold it to: the load is what fails
    EXPECT_TRUE(junk.ready()) << "nothing has tried to load it yet";
    EXPECT_FALSE(junk.generate("Night: 2099-07-01\n", {}).has_value());
    EXPECT_FALSE(junk.ready());
    EXPECT_EQ(junk.whyNotReady(), "The model file could not be loaded.");
    EXPECT_TRUE(junk.tokenize(" Night<|summary|>").empty());
    fs::remove_all(fs::path(path).parent_path());
}

// -- The real model (HMS_CPAP_NARRATOR_MODEL) --------------------------------------

TEST(NarratorModel, TokenizesEachFixturePromptExactlyAsTrainingDid) {
    const auto path = realModel();
    if (path.empty()) GTEST_SKIP() << "HMS_CPAP_NARRATOR_MODEL names no model file";
    GgufModel model(path, kNarratorModelSha256);
    ASSERT_TRUE(model.ready()) << model.whyNotReady();
    for (const auto& n : fixtureNights()) {
        EXPECT_EQ(model.tokenize(narratorPrompt(n.facts)), n.ids) << n.id;
    }
}

TEST(NarratorModel, AGreedySummaryOfAFixtureNightPassesTheFactsCheck) {
    const auto path = realModel();
    if (path.empty()) GTEST_SKIP() << "HMS_CPAP_NARRATOR_MODEL names no model file";
    GgufModel model(path, kNarratorModelSha256);
    ASSERT_TRUE(model.ready()) << model.whyNotReady();
    const auto night = fixtureNights().front();

    GenerateParams greedy;
    greedy.temperature = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    const auto text = model.generate(night.facts, greedy);
    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(text.has_value());
    ASSERT_FALSE(text->empty());
    EXPECT_EQ(violations(*text, night.facts), std::vector<std::string>{}) << *text;
    EXPECT_NE(text->find("2025-10-26"), std::string::npos) << "it names its own night: " << *text;
    // Q4 against the HF model is not held to be identical (it was on the Pi);
    // said, not asserted.
    std::cout << "  greedy (" << secs << " s), " << (*text == night.hf ? "identical to" : "differs from")
              << " the HF model's: " << *text << std::endl;

    EXPECT_EQ(model.generate(night.facts, greedy), text) << "greedy decoding is deterministic";
}

TEST(NarratorModel, ASampledSummaryIsTheSameEachTimeForOneNight) {
    const auto path = realModel();
    if (path.empty()) GTEST_SKIP() << "HMS_CPAP_NARRATOR_MODEL names no model file";
    GgufModel model(path, kNarratorModelSha256);
    const auto night = fixtureNights().front();
    GenerateParams p;
    p.temperature = 0.7;   // GgufModel still samples when asked; narrate() asks for greedy
    const auto a = model.generate(night.facts, p);
    const auto b = model.generate(night.facts, p);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a, b) << "the seed is fixed";
    GenerateParams few = p;
    few.max_tokens = 5;
    const auto shortText = model.generate(night.facts, few);
    ASSERT_TRUE(shortText.has_value());
    EXPECT_LT(shortText->size(), a->size()) << "max_tokens ends it";
}
