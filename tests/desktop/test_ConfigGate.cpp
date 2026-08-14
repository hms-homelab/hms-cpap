// SDD-016: the supervisor decides whether the child can run, before running it.
//
// This is the decision that justifies a supervisor existing rather than a
// shortcut that launches a binary, so it is pinned here rather than discovered
// by launching things. Every case below is a state a real machine reaches:
// a fresh install, an abandoned setup, a folder that moved, a hand-edited file.

#include <gtest/gtest.h>

#include "ConfigGate.h"

#include <nlohmann/json.hpp>

using namespace cpapdash::supervisor;

namespace {

/// A configuration that should actually start.
nlohmann::json workingConfig() {
    return nlohmann::json{
        {"setup_complete", true},
        {"source", "local"},
        {"local_dir", "/tmp/card"},
        {"archive_dir", ""},
        {"web_port", 8893},
        {"database", {{"type", "sqlite"}}},
    };
}

}  // namespace

// ── The two answers ─────────────────────────────────────────────────────────

TEST(ConfigGateTest, AGoodConfigStartsTheChild) {
    const auto r = evaluateConfig(workingConfig());
    EXPECT_EQ(r.readiness, Readiness::Ready);
    EXPECT_FALSE(r.needsSetup());
    EXPECT_TRUE(r.issues.empty());
}

TEST(ConfigGateTest, NoConfigFileOpensTheWizard) {
    // The first run. There is no file, no service and no port, which is exactly
    // why readiness cannot be a question asked over HTTP -- there would be
    // nothing to ask.
    const auto r = evaluateConfig(std::nullopt);
    EXPECT_TRUE(r.needsSetup());
    EXPECT_EQ(r.reason, SetupReason::NoConfigFile);
    EXPECT_FALSE(r.summary.empty()) << "the wizard needs something to say";
}

TEST(ConfigGateTest, AnAbandonedSetupResumesRatherThanStarting) {
    auto cfg = workingConfig();
    cfg["setup_complete"] = false;
    const auto r = evaluateConfig(cfg);
    EXPECT_TRUE(r.needsSetup());
    EXPECT_EQ(r.reason, SetupReason::SetupIncomplete);
}

// ── The case that must never be silently overwritten ────────────────────────

TEST(ConfigGateTest, AnUnparseableFileIsItsOwnReason) {
    // Someone hand-edited config.json and broke it. That is NOT the same as
    // having no configuration: the file holds settings they still want, so the
    // wizard has to offer the choice rather than quietly replace it.
    const auto r = evaluateConfig(std::nullopt, /*parse_failed=*/true);
    EXPECT_TRUE(r.needsSetup());
    EXPECT_EQ(r.reason, SetupReason::Unparseable);
    EXPECT_NE(r.summary.find("could not be read"), std::string::npos) << r.summary;
    EXPECT_NE(r.summary.find("Nothing has been changed"), std::string::npos)
        << "the user needs to know their file is intact: " << r.summary;
}

TEST(ConfigGateTest, UnparseableIsDistinctFromAbsent) {
    // If these collapsed into one answer, a broken file would be treated as a
    // first run and overwritten without anyone being asked.
    EXPECT_NE(evaluateConfig(std::nullopt, true).reason,
              evaluateConfig(std::nullopt, false).reason);
}

// ── Configured, but not usable ──────────────────────────────────────────────

TEST(ConfigGateTest, ALocalSourceWithNoFolderNeedsSetup) {
    // The common real failure: the card folder moved or was never chosen. The
    // service cannot collect anything, so starting it and reporting an exit
    // code would tell the user their software is broken.
    auto cfg = workingConfig();
    cfg["local_dir"] = "";
    const auto r = evaluateConfig(cfg);
    EXPECT_TRUE(r.needsSetup());
    EXPECT_EQ(r.reason, SetupReason::Invalid);
    ASSERT_FALSE(r.issues.empty());
    EXPECT_EQ(r.issues[0].path, "local_dir");
    EXPECT_FALSE(r.issues[0].hint.empty())
        << "an invalid setting must come with what to do about it";
}

TEST(ConfigGateTest, AServerDatabaseWithNoHostNeedsSetup) {
    auto cfg = workingConfig();
    cfg["database"] = {{"type", "postgresql"}, {"host", ""}, {"name", "cpap"}, {"user", "u"}};
    const auto r = evaluateConfig(cfg);
    EXPECT_TRUE(r.needsSetup());
    EXPECT_EQ(r.reason, SetupReason::Invalid);
}

TEST(ConfigGateTest, TheSummaryCountsWhatIsWrong) {
    auto cfg = workingConfig();
    cfg["local_dir"] = "";
    const auto r = evaluateConfig(cfg);
    EXPECT_NE(r.summary.find("setting"), std::string::npos) << r.summary;
}

// ── What must NOT block a start ─────────────────────────────────────────────

TEST(ConfigGateTest, AMissingArchiveFolderIsAWarningAndStillStarts) {
    // PreflightService treats this as non-fatal on purpose: it stops files
    // reaching disk, but the dashboard still works and still explains the
    // problem. Refusing to start here would take down the very UI that tells
    // the user what is wrong.
    auto cfg = workingConfig();
    cfg["source"]      = "ezshare";
    cfg["ezshare_url"] = "http://192.168.4.1";
    cfg["archive_dir"] = "";
    const auto r = evaluateConfig(cfg);
    EXPECT_EQ(r.readiness, Readiness::Ready)
        << "a warning-level problem must not send the user to the wizard";
}

TEST(ConfigGateTest, SettingsBehindAnOffToggleAreNotChecked) {
    // MQTT off with a nonsense broker left in the file must not block a start:
    // nothing reads it. Otherwise turning a feature off would strand someone on
    // an error about a field they cannot even see.
    auto cfg = workingConfig();
    cfg["mqtt"] = {{"enabled", false}, {"broker", ""}, {"port", 0}};
    const auto r = evaluateConfig(cfg);
    EXPECT_EQ(r.readiness, Readiness::Ready);
}

TEST(ConfigGateTest, AnEmptyObjectIsTreatedAsUnconfigured) {
    const auto r = evaluateConfig(nlohmann::json::object());
    EXPECT_TRUE(r.needsSetup());
}

TEST(ConfigGateTest, ANonObjectIsTreatedAsUnconfigured) {
    // A file containing "null" or a bare array parses fine and means nothing.
    EXPECT_TRUE(evaluateConfig(nlohmann::json::array()).needsSetup());
    EXPECT_TRUE(evaluateConfig(nlohmann::json()).needsSetup());
}
