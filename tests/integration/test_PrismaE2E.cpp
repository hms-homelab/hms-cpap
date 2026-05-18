#include <gtest/gtest.h>
#include "services/PrismaIngestion.h"
#include "parsers/CpapdashBridge.h"

#include <filesystem>
#include <iostream>
#include <iomanip>
#include <algorithm>

using namespace hms_cpap;
namespace fs = std::filesystem;

static std::string findSampleDir() {
    for (const auto& candidate : {
        std::string("../lowenstein_samples/extracted_therapy"),
        std::string("../../lowenstein_samples/extracted_therapy"),
        std::string("/home/aamat/maestro_hub/lowenstein_samples/extracted_therapy"),
    }) {
        if (fs::is_directory(candidate)) return candidate;
    }
    return "";
}

class PrismaE2ETest : public ::testing::Test {
protected:
    std::string sample_dir;

    void SetUp() override {
        sample_dir = findSampleDir();
        if (sample_dir.empty()) GTEST_SKIP() << "Lowenstein sample data not found";
    }
};

TEST_F(PrismaE2ETest, FullPipelineDiscoverAndParse) {
    PrismaIngestion ingestion(sample_dir);
    ASSERT_TRUE(ingestion.initialize());

    auto sessions = ingestion.discoverSessions(std::nullopt);
    ASSERT_GT(sessions.size(), 0u);
    std::cout << "Discovered " << sessions.size() << " Lowenstein sessions" << std::endl;

    auto parser = createParser(DeviceManufacturer::LOWENSTEIN);
    ASSERT_NE(parser, nullptr);

    int parsed_count = 0;
    int total_events = 0;
    double total_hours = 0;

    for (const auto& ps : sessions) {
        std::string staged = ingestion.stageSession(ps);

        auto result = parser->parseSession(staged, "prisma_e2e", "Prisma 20A");
        fs::remove_all(staged);

        if (!result) continue;

        parsed_count++;
        EXPECT_EQ(result->manufacturer, DeviceManufacturer::LOWENSTEIN);
        EXPECT_TRUE(result->session_start.has_value());
        EXPECT_TRUE(result->session_end.has_value());
        EXPECT_TRUE(result->duration_seconds.has_value());
        EXPECT_GT(*result->duration_seconds, 0);
        EXPECT_EQ(result->status, cpapdash::parser::ParsedSession::Status::COMPLETED);

        if (result->metrics) {
            EXPECT_GE(result->metrics->ahi, 0.0);
            total_events += result->metrics->total_events;
        }

        if (result->duration_seconds)
            total_hours += *result->duration_seconds / 3600.0;

        // Sessions with events should have breathing summaries
        if (!result->events.empty()) {
            EXPECT_GT(result->breathing_summary.size(), 0u);
        }
    }

    std::cout << "Parsed: " << parsed_count << "/" << sessions.size()
              << " sessions, " << total_events << " total events, "
              << std::fixed << std::setprecision(1) << total_hours << " total hours"
              << std::endl;

    EXPECT_GT(parsed_count, 0);
}

TEST_F(PrismaE2ETest, DeviceInfoFromXml) {
    PrismaIngestion ingestion(sample_dir);
    ingestion.initialize();

    std::string dev_xml = ingestion.deviceXmlPath();
    if (dev_xml.empty()) GTEST_SKIP() << "No device.xml in sample data";

    auto info = cpapdash::parser::PrismaParser::parseDeviceXml(dev_xml);
    EXPECT_FALSE(info.serial_number.empty());
    EXPECT_GT(info.device_type, 0);
    EXPECT_FALSE(info.fw_version.empty());

    std::cout << "Device: serial=" << info.serial_number
              << " type=" << info.device_type
              << " fw=" << info.fw_version << std::endl;
}

TEST_F(PrismaE2ETest, LargestSessionHasRichData) {
    PrismaIngestion ingestion(sample_dir);
    ingestion.initialize();
    auto sessions = ingestion.discoverSessions(std::nullopt);
    ASSERT_GT(sessions.size(), 5u);

    // Find session with largest signal file
    auto largest = std::max_element(sessions.begin(), sessions.end(),
        [](const PrismaSessionFile& a, const PrismaSessionFile& b) {
            return fs::file_size(a.signal_path) < fs::file_size(b.signal_path);
        });

    auto parser = createParser(DeviceManufacturer::LOWENSTEIN);
    std::string staged = ingestion.stageSession(*largest);
    auto result = parser->parseSession(staged, "prisma_e2e", "Prisma 20A");
    fs::remove_all(staged);

    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(result->metrics.has_value());

    std::cout << "Largest session: seq=" << largest->sequence_number
              << " duration=" << (*result->duration_seconds / 60) << " min"
              << " AHI=" << result->metrics->ahi
              << " events=" << result->metrics->total_events
              << " breathing_summaries=" << result->breathing_summary.size()
              << std::endl;

    EXPECT_GT(*result->duration_seconds, 600);
    EXPECT_GT(result->events.size(), 0u);
    EXPECT_GT(result->breathing_summary.size(), 0u);
    EXPECT_TRUE(result->settings.has_value());
    EXPECT_EQ(result->serial_number, "30167534");
}
