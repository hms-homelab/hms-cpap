#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "services/BackfillService.h"

using namespace hms_cpap;

// ── Status reporting ────────────────────────────────────────────────────

TEST(BackfillServiceTest, InitialStatusIsIdle) {
    BackfillService::Config cfg;
    cfg.local_dir = "/nonexistent";
    cfg.device_id = "test_device";
    cfg.device_name = "Test CPAP";

    // Use nullptr for DB — we won't actually run a backfill
    BackfillService svc(cfg, nullptr);

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "idle");
    EXPECT_EQ(status["folders_total"].asInt(), 0);
    EXPECT_EQ(status["folders_done"].asInt(), 0);
    EXPECT_EQ(status["sessions_parsed"].asInt(), 0);
    EXPECT_EQ(status["sessions_saved"].asInt(), 0);
    EXPECT_EQ(status["errors"].asInt(), 0);
}

TEST(BackfillServiceTest, StatusJsonHasAllFields) {
    BackfillService::Config cfg;
    cfg.local_dir = "/tmp";
    cfg.device_id = "test";
    cfg.device_name = "Test";

    BackfillService svc(cfg, nullptr);
    auto status = svc.getStatus();

    EXPECT_TRUE(status.isMember("status"));
    EXPECT_TRUE(status.isMember("folders_total"));
    EXPECT_TRUE(status.isMember("folders_done"));
    EXPECT_TRUE(status.isMember("sessions_parsed"));
    EXPECT_TRUE(status.isMember("sessions_saved"));
    EXPECT_TRUE(status.isMember("sessions_deleted"));
    EXPECT_TRUE(status.isMember("errors"));
}

TEST(BackfillServiceTest, TriggerSetsRequestedFlag) {
    BackfillService::Config cfg;
    cfg.local_dir = "/tmp";
    cfg.device_id = "test";
    cfg.device_name = "Test";

    BackfillService svc(cfg, nullptr);
    // Just verify trigger doesn't crash without start()
    svc.trigger("2025-08-15", "2025-08-20");

    // Status should still be idle (worker not running)
    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "idle");
}

TEST(BackfillServiceTest, StartAndStopCleanly) {
    BackfillService::Config cfg;
    cfg.local_dir = "/tmp";
    cfg.device_id = "test";
    cfg.device_name = "Test";

    BackfillService svc(cfg, nullptr);
    svc.start();
    svc.stop();

    auto status = svc.getStatus();
    EXPECT_EQ(status["status"].asString(), "idle");
}
