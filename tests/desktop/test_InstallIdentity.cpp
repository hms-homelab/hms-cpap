// #94 follow-up: the config.json the desktop setup wizard writes names this
// install. Without device_id and device_name it loaded as the old fixed
// "cpap_resmed_23243570851" and "ResMed AirSense 10", so a fresh install
// started on another install's id, moved off it, and cleared its topics.
#include <gtest/gtest.h>

#include "ConfigModel.h"
#include "utils/AppConfig.h"
#include "utils/DeviceIdMigration.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

#include <nlohmann/json.hpp>

using cpapdash::supervisor::newDeviceId;
using cpapdash::supervisor::withInstallIdentity;

namespace {

// What the wizard's model holds after a first setup: only what it asks.
nlohmann::json wizardDoc() {
    return {{"source", "local"},
            {"local_dir", "/tmp/sleepbridge/TAP-S3-00000000"},
            {"database", {{"type", "sqlite"}}},
            {"setup_complete", true}};
}

}  // namespace

TEST(InstallIdentity, NewIdHasTheServicesShape) {
    const std::regex shape("^cpapdash_[0-9a-f]{8}$");
    for (int i = 0; i < 20; ++i) EXPECT_TRUE(std::regex_match(newDeviceId(), shape));
    EXPECT_NE(newDeviceId(), newDeviceId());
}

TEST(InstallIdentity, AFirstSetupNamesANewIdAndNoMachine) {
    const auto doc = withInstallIdentity(wizardDoc());
    EXPECT_TRUE(std::regex_match(doc["device_id"].get<std::string>(), std::regex("^cpapdash_[0-9a-f]{8}$")));
    EXPECT_EQ(doc["device_name"], "CPAP");
    EXPECT_EQ(doc["local_dir"], "/tmp/sleepbridge/TAP-S3-00000000") << "what the wizard set is kept";
}

// The service reads it as written: no old fixed id, so no move and nothing
// cleared on the broker.
TEST(InstallIdentity, TheServiceNeverStartsOnTheOldFixedId) {
    const auto path = (std::filesystem::temp_directory_path() /
                       ("wizard_identity_" + std::to_string(::getpid()) + ".json")).string();
    std::ofstream(path) << withInstallIdentity(wizardDoc()).dump(2);
    hms_cpap::AppConfig c;
    ASSERT_EQ(hms_cpap::AppConfig::loadFile(path, c), hms_cpap::AppConfig::LoadStatus::Ok);
    EXPECT_NE(c.device_id, hms_cpap::AppConfig::kLegacyDeviceId);
    EXPECT_FALSE(hms_cpap::isOldDefaultDeviceId(c.device_id));
    EXPECT_FALSE(hms_cpap::beginDeviceIdBackfill(c));
    EXPECT_TRUE(c.device_id_previous.empty());
    EXPECT_EQ(c.device_name, "CPAP");
    std::filesystem::remove(path);
}

// Settings or a re-run of setup over an existing config: its id never changes.
TEST(InstallIdentity, AnExistingIdAndNameAreKept) {
    auto doc = wizardDoc();
    doc["device_id"] = "cpapdash_0badc0de";
    doc["device_name"] = "Bedroom";
    const auto out = withInstallIdentity(doc);
    EXPECT_EQ(out["device_id"], "cpapdash_0badc0de");
    EXPECT_EQ(out["device_name"], "Bedroom");
}

// An install still on an old fixed id keeps it here; the service's own move
// (SDD-051) takes it off, with its nights.
TEST(InstallIdentity, AnOldFixedIdAlreadyWrittenIsLeftForTheServiceToMove) {
    auto doc = wizardDoc();
    doc["device_id"] = "cpap_resmed_23243570851";
    EXPECT_EQ(withInstallIdentity(doc)["device_id"], "cpap_resmed_23243570851");
}

// Start Over, "Settings only": the nights it keeps are filed under the old
// config's id, so the new config keeps it, pin and move under way included.
TEST(InstallIdentity, StartOverKeepsTheClearedConfigsId) {
    const nlohmann::json previous = {{"device_id", "cpapdash_0badc0de"},
                                     {"device_id_pinned", true},
                                     {"device_name", "Bedroom"}};
    const auto out = withInstallIdentity(wizardDoc(), previous);
    EXPECT_EQ(out["device_id"], "cpapdash_0badc0de");
    EXPECT_EQ(out["device_id_pinned"], true);
    EXPECT_EQ(out["device_name"], "CPAP") << "the name is a setting; Start Over clears it";
}

TEST(InstallIdentity, AnEmptyIdIsNotAnId) {
    auto doc = wizardDoc();
    doc["device_id"] = "";
    EXPECT_NE(withInstallIdentity(doc)["device_id"], "");
}
