/**
 * HMS-CPAP CPAPModels Unit Tests (Simplified)
 *
 * Basic tests for data structures and session handling.
 */

#include <gtest/gtest.h>
#include "models/CPAPModels.h"
#include <chrono>

using namespace hms_cpap;
using namespace std::chrono;

// Test fixture
class CPAPModelsTest : public ::testing::Test {
protected:
    void SetUp() override {
        session = std::make_unique<CPAPSession>();
        session->device_id = "TEST-DEVICE";
        session->device_name = "Test Device";
        session->session_start = system_clock::now();
    }

    void TearDown() override {
        session.reset();
    }

    std::unique_ptr<CPAPSession> session;
};

// ============================================================================
// DATA STRUCTURE TESTS
// ============================================================================

TEST_F(CPAPModelsTest, Session_BasicFields) {
    EXPECT_EQ(session->device_id, "TEST-DEVICE");
    EXPECT_EQ(session->device_name, "Test Device");
    EXPECT_TRUE(session->session_start.has_value());
}

TEST_F(CPAPModelsTest, Session_Status) {
    // Test status values
    session->status = CPAPSession::Status::IN_PROGRESS;
    EXPECT_EQ(session->status, CPAPSession::Status::IN_PROGRESS);

    session->status = CPAPSession::Status::COMPLETED;
    EXPECT_EQ(session->status, CPAPSession::Status::COMPLETED);
}

TEST_F(CPAPModelsTest, Session_HasEvents) {
    session->has_events = false;
    EXPECT_FALSE(session->has_events);

    session->has_events = true;
    EXPECT_TRUE(session->has_events);
}

TEST_F(CPAPModelsTest, Session_HasSummary) {
    session->has_summary = false;
    EXPECT_FALSE(session->has_summary);

    session->has_summary = true;
    EXPECT_TRUE(session->has_summary);
}

// ============================================================================
// FILE PATH TESTS
// ============================================================================

TEST_F(CPAPModelsTest, FilePaths_BRP) {
    EXPECT_FALSE(session->brp_file_path.has_value());

    session->brp_file_path = "/archive/test_BRP.edf";
    EXPECT_TRUE(session->brp_file_path.has_value());
    EXPECT_EQ(session->brp_file_path.value(), "/archive/test_BRP.edf");
}

TEST_F(CPAPModelsTest, FilePaths_AllTypes) {
    session->brp_file_path = "/archive/test_BRP.edf";
    session->eve_file_path = "/archive/test_EVE.edf";
    session->sad_file_path = "/archive/test_SAD.edf";
    session->pld_file_path = "/archive/test_PLD.edf";
    session->csl_file_path = "/archive/test_CSL.edf";

    EXPECT_TRUE(session->brp_file_path.has_value());
    EXPECT_TRUE(session->eve_file_path.has_value());
    EXPECT_TRUE(session->sad_file_path.has_value());
    EXPECT_TRUE(session->pld_file_path.has_value());
    EXPECT_TRUE(session->csl_file_path.has_value());
}

// ============================================================================
// BREATHING SUMMARY TESTS
// ============================================================================

TEST_F(CPAPModelsTest, BreathingSummary_Add) {
    BreathingSummary summary;
    summary.respiratory_rate = 15.0;
    summary.tidal_volume = 500.0;

    session->breathing_summary.push_back(summary);

    EXPECT_EQ(session->breathing_summary.size(), 1);
    EXPECT_TRUE(session->breathing_summary[0].respiratory_rate.has_value());
    EXPECT_NEAR(session->breathing_summary[0].respiratory_rate.value(), 15.0, 0.01);
}

// ============================================================================
// EVENTS TESTS
// ============================================================================

TEST_F(CPAPModelsTest, Events_Add) {
    CPAPEvent event;
    event.duration_seconds = 20.0;

    session->events.push_back(event);

    EXPECT_EQ(session->events.size(), 1);
    EXPECT_NEAR(session->events[0].duration_seconds, 20.0, 0.01);
}

// ============================================================================
// VITALS TESTS
// ============================================================================

TEST_F(CPAPModelsTest, Vitals_Add) {
    CPAPVitals vital;
    vital.spo2 = 95;
    vital.heart_rate = 72;

    session->vitals.push_back(vital);

    EXPECT_EQ(session->vitals.size(), 1);
    EXPECT_EQ(session->vitals[0].spo2, 95);
    EXPECT_EQ(session->vitals[0].heart_rate, 72);
}

// ============================================================================
// DURATION TESTS
// ============================================================================

TEST_F(CPAPModelsTest, Duration_Calculation) {
    session->session_start = system_clock::now() - hours(7);
    session->session_end = system_clock::now();

    auto duration = duration_cast<seconds>(
        session->session_end.value() - session->session_start.value()
    );

    session->duration_seconds = static_cast<int>(duration.count());

    // Duration should be approximately 7 hours = 25200 seconds
    EXPECT_GT(session->duration_seconds.value(), 25000);
    EXPECT_LT(session->duration_seconds.value(), 26000);
}

// ============================================================================
// SESSION METRICS TESTS (Event Counting)
// ============================================================================

TEST_F(CPAPModelsTest, Metrics_EventCounting_ObstructiveApneas) {
    // Add 3 obstructive apnea events
    CPAPEvent event1;
    event1.event_type = EventType::OBSTRUCTIVE;
    event1.duration_seconds = 12.0;
    session->events.push_back(event1);

    CPAPEvent event2;
    event2.event_type = EventType::OBSTRUCTIVE;
    event2.duration_seconds = 10.0;
    session->events.push_back(event2);

    CPAPEvent event3;
    event3.event_type = EventType::OBSTRUCTIVE;
    event3.duration_seconds = 15.0;
    session->events.push_back(event3);

    session->duration_seconds = 3600;  // 1 hour session
    session->calculateMetrics();

    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_EQ(session->metrics->obstructive_apneas, 3);
    EXPECT_EQ(session->metrics->total_events, 3);
    EXPECT_NEAR(session->metrics->ahi, 3.0, 0.01);  // 3 events / 1 hour = 3.0 AHI
}

TEST_F(CPAPModelsTest, Metrics_EventCounting_CentralApneas) {
    // Add 2 central apnea events
    CPAPEvent event1;
    event1.event_type = EventType::CENTRAL;
    event1.duration_seconds = 11.0;
    session->events.push_back(event1);

    CPAPEvent event2;
    event2.event_type = EventType::CENTRAL;
    event2.duration_seconds = 13.0;
    session->events.push_back(event2);

    session->duration_seconds = 3600;
    session->calculateMetrics();

    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_EQ(session->metrics->central_apneas, 2);
    EXPECT_EQ(session->metrics->total_events, 2);
}

TEST_F(CPAPModelsTest, Metrics_EventCounting_Hypopneas) {
    // Add 4 hypopnea events
    for (int i = 0; i < 4; i++) {
        CPAPEvent event;
        event.event_type = EventType::HYPOPNEA;
        event.duration_seconds = 10.0;
        session->events.push_back(event);
    }

    session->duration_seconds = 7200;  // 2 hours
    session->calculateMetrics();

    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_EQ(session->metrics->hypopneas, 4);
    EXPECT_EQ(session->metrics->total_events, 4);
    EXPECT_NEAR(session->metrics->ahi, 2.0, 0.01);  // 4 events / 2 hours = 2.0 AHI
}

TEST_F(CPAPModelsTest, Metrics_EventCounting_MixedEvents) {
    // Add mixed event types (matching real session data)
    CPAPEvent obstructive;
    obstructive.event_type = EventType::OBSTRUCTIVE;
    obstructive.duration_seconds = 12.0;
    session->events.push_back(obstructive);

    CPAPEvent central1;
    central1.event_type = EventType::CENTRAL;
    central1.duration_seconds = 12.0;
    session->events.push_back(central1);

    CPAPEvent central2;
    central2.event_type = EventType::CENTRAL;
    central2.duration_seconds = 10.0;
    session->events.push_back(central2);

    CPAPEvent rera;
    rera.event_type = EventType::RERA;
    rera.duration_seconds = 0.0;
    session->events.push_back(rera);

    session->duration_seconds = 8160;  // 2.27 hours (actual session duration)
    session->calculateMetrics();

    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_EQ(session->metrics->obstructive_apneas, 1);
    EXPECT_EQ(session->metrics->central_apneas, 2);
    EXPECT_EQ(session->metrics->hypopneas, 0);
    EXPECT_EQ(session->metrics->reras, 1);
    EXPECT_EQ(session->metrics->total_events, 4);
    EXPECT_NEAR(session->metrics->ahi, 1.76, 0.01);  // 4 events / 2.27 hours ≈ 1.76
}

TEST_F(CPAPModelsTest, Metrics_EventCounting_NoEvents) {
    // Session with no events (excellent therapy!)
    session->duration_seconds = 28800;  // 8 hours
    session->calculateMetrics();

    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_EQ(session->metrics->obstructive_apneas, 0);
    EXPECT_EQ(session->metrics->central_apneas, 0);
    EXPECT_EQ(session->metrics->hypopneas, 0);
    EXPECT_EQ(session->metrics->reras, 0);
    EXPECT_EQ(session->metrics->clear_airway_apneas, 0);
    EXPECT_EQ(session->metrics->total_events, 0);
    EXPECT_NEAR(session->metrics->ahi, 0.0, 0.01);
}

TEST_F(CPAPModelsTest, Metrics_EventCounting_HighAHI) {
    // Session with multiple events (higher AHI)
    // Add 15 events total
    for (int i = 0; i < 5; i++) {
        CPAPEvent obstructive;
        obstructive.event_type = EventType::OBSTRUCTIVE;
        obstructive.duration_seconds = 12.0;
        session->events.push_back(obstructive);

        CPAPEvent central;
        central.event_type = EventType::CENTRAL;
        central.duration_seconds = 11.0;
        session->events.push_back(central);

        CPAPEvent hypopnea;
        hypopnea.event_type = EventType::HYPOPNEA;
        hypopnea.duration_seconds = 10.0;
        session->events.push_back(hypopnea);
    }

    session->duration_seconds = 21600;  // 6 hours
    session->calculateMetrics();

    ASSERT_TRUE(session->metrics.has_value());
    EXPECT_EQ(session->metrics->obstructive_apneas, 5);
    EXPECT_EQ(session->metrics->central_apneas, 5);
    EXPECT_EQ(session->metrics->hypopneas, 5);
    EXPECT_EQ(session->metrics->total_events, 15);
    EXPECT_NEAR(session->metrics->ahi, 2.5, 0.01);  // 15 events / 6 hours = 2.5 AHI
}

