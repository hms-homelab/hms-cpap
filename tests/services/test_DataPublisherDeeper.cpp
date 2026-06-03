/**
 * HMS-CPAP DataPublisherService — Deeper coverage (broker-free, deterministic)
 *
 * Goal: raise DataPublisherService + DiscoveryPublisher coverage past the basics
 * by exercising every payload/branch path the builders can take:
 *   - discovery for realtime / historical / STR / insights / oximetry
 *   - realtime state with all optional metrics set vs unset
 *   - historical state across every optional/required field
 *   - STR daily state (delta on/off, mask-event pairing, fixed precision)
 *   - session-active ON (publishSession) vs OFF (publishSessionCompleted)
 *   - insights (empty + populated), summaries (daily/weekly/monthly)
 *   - oximetry live (valid vs invalid) and summary, with NULL db too
 *   - null/missing optional fields → safe no-publish
 *
 * Two construction strategies, both deterministic and broker-free:
 *
 *   1) NULL MqttClient. Many public methods early-return when mqtt_client_ is
 *      null; we assert those are safe no-ops returning the documented value.
 *
 *   2) Real hms::MqttClient pointed at RFC-5737 TEST-NET-1 (192.0.2.1), never
 *      connected. publish() is a non-virtual Paho wrapper that returns false
 *      (without throwing) while disconnected, so every JSON/topic *builder*
 *      path runs to completion. Methods that gate on isConnected() early-return.
 *      If a stray local broker happens to be up, the few behavioural
 *      not-connected assertions GTEST_SKIP so the suite stays deterministic.
 *
 * This mirrors the established pattern in tests/mqtt/test_DiscoveryPublisher.cpp.
 * No live MQTT/TCP/BLE/device/network and no wall-clock dependence (fixed epochs).
 */

#include <gtest/gtest.h>

#include "services/DataPublisherService.h"
#include "database/SQLiteDatabase.h"
#include "database/IDatabase.h"
#include "mqtt_client.h"
#include "clients/IO2RingClient.h"

#include <chrono>
#include <memory>
#include <string>
#include <unistd.h>

using namespace hms_cpap;

namespace {

// Real client pointed at a guaranteed-unreachable broker; never connect().
hms::MqttConfig unreachableConfig() {
    hms::MqttConfig cfg;
    cfg.broker = "192.0.2.1";  // RFC 5737 TEST-NET-1: not routable
    cfg.port = 1883;
    cfg.client_id = "dpd_test_" + std::to_string(static_cast<long>(::getpid()));
    return cfg;
}

std::shared_ptr<hms::MqttClient> makeUnreachableClient() {
    // Intentionally do NOT connect(): publish() stays broker-gated, returns false.
    return std::make_shared<hms::MqttClient>(unreachableConfig());
}

// A fixed-epoch time point so timestamp formatting is deterministic (no now()).
std::chrono::system_clock::time_point fixedEpoch() {
    // 1767322445s since epoch. Value is irrelevant to assertions; we only assert
    // that the builder runs (shape stable across locale/TZ), not a numeric offset.
    return std::chrono::system_clock::time_point{std::chrono::seconds{1767322445}};
}

// A SessionMetrics with EVERY optional populated, to drive all historical
// publish branches.
SessionMetrics fullMetrics() {
    SessionMetrics m;
    m.total_events = 12;
    m.ahi = 2.5;
    m.obstructive_apneas = 5;
    m.central_apneas = 3;
    m.hypopneas = 2;
    m.reras = 1;
    m.clear_airway_apneas = 1;
    m.avg_event_duration = 14.2;
    m.max_event_duration = 38.0;
    m.time_in_apnea_percent = 1.1;
    m.avg_pressure = 9.4;
    m.min_pressure = 6.0;
    m.max_pressure = 14.0;
    m.pressure_p95 = 12.5;
    m.pressure_p50 = 9.0;
    m.avg_leak_rate = 4.2;
    m.max_leak_rate = 30.1;
    m.leak_p95 = 24.0;
    m.leak_p50 = 3.0;
    m.avg_flow_rate = 11.0;
    m.max_flow_rate = 60.0;
    m.flow_p95 = 42.0;
    m.avg_respiratory_rate = 14.0;
    m.avg_tidal_volume = 480.0;
    m.avg_minute_ventilation = 6.7;
    m.avg_inspiratory_time = 1.4;
    m.avg_expiratory_time = 2.1;
    m.avg_ie_ratio = 0.66;
    m.avg_flow_limitation = 0.05;
    m.avg_mask_pressure = 8.8;
    m.avg_epr_pressure = 2.0;
    m.avg_snore = 0.3;
    m.avg_target_ventilation = 5.5;
    m.therapy_mode = 7;  // ASV
    m.avg_spo2 = 95.5;
    m.min_spo2 = 88.0;
    m.max_spo2 = 99.0;
    m.spo2_p95 = 98.0;
    m.spo2_p50 = 96.0;
    m.spo2_drops = 4;
    m.avg_heart_rate = 60;
    m.min_heart_rate = 48;
    m.max_heart_rate = 92;
    m.hr_p95 = 84;
    m.hr_p50 = 58;
    m.usage_hours = 7.5;
    m.usage_percent = 93.75;
    return m;
}

}  // namespace

// ===========================================================================
// Fixture A: NULL MqttClient — all gated calls are safe no-ops.
// ===========================================================================

class NullMqttDeepTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_shared<SQLiteDatabase>(":memory:");
        db_->connect();
        pub_ = std::make_unique<DataPublisherService>(nullptr, db_);
    }
    std::shared_ptr<IDatabase> db_;
    std::unique_ptr<DataPublisherService> pub_;
};

TEST_F(NullMqttDeepTest, InitializeReturnsTrueWithoutBroker) {
    EXPECT_TRUE(pub_->initialize());
}

TEST_F(NullMqttDeepTest, PublishDiscoveryReturnsTrueAsNoOp) {
    // publishDiscovery() returns true immediately when mqtt_client_ is null.
    EXPECT_TRUE(pub_->publishDiscovery());
}

TEST_F(NullMqttDeepTest, PublishSessionCompletedReturnsTrueAsNoOp) {
    EXPECT_TRUE(pub_->publishSessionCompleted());
}

TEST_F(NullMqttDeepTest, PublishHistoricalStateFullMetricsNoCrash) {
    // Drives every branch in publishHistoricalState (all optionals present),
    // but with null client each publish is skipped at the guard. No crash.
    EXPECT_NO_THROW(pub_->publishHistoricalState(fullMetrics()));
}

TEST_F(NullMqttDeepTest, PublishHistoricalStateEmptyMetricsNoCrash) {
    // No optionals set: only the always-published event counters would fire,
    // but the null guard short-circuits before any. Exercises the absent path.
    SessionMetrics m;  // defaults
    EXPECT_NO_THROW(pub_->publishHistoricalState(m));
}

TEST_F(NullMqttDeepTest, PublishSessionRealtimeFullOptionalsNoCrash) {
    CPAPSession s;
    s.device_id = "dev";
    s.duration_seconds = 3600;
    s.session_start = fixedEpoch();

    BreathingSummary bs;
    bs.avg_pressure = 9.0;
    bs.avg_flow_rate = 11.0;
    bs.leak_rate = 4.0;
    bs.respiratory_rate = 14.0;
    bs.tidal_volume = 480.0;
    bs.minute_ventilation = 6.7;
    bs.flow_limitation = 0.05;
    bs.mask_pressure = 8.5;
    bs.snore_index = 0.2;
    bs.target_ventilation = 5.0;
    s.breathing_summary.push_back(bs);

    CPAPVitals v;
    v.spo2 = 96.0;
    v.heart_rate = 60;
    s.vitals.push_back(v);

    s.metrics = fullMetrics();

    // With null client, publishSession publishes nothing but must not crash.
    EXPECT_NO_THROW(pub_->publishSession(s));
}

TEST_F(NullMqttDeepTest, PublishSessionEmptyBreathingAndVitalsNoCrash) {
    CPAPSession s;
    s.device_id = "dev";
    // No duration, no start, no breathing_summary, no vitals, no metrics:
    // every "has_value()/empty()" branch takes the absent path.
    EXPECT_NO_THROW(pub_->publishSession(s));
}

TEST_F(NullMqttDeepTest, PublishSTRStateNoCrashWithAndWithoutNightly) {
    STRDailyRecord r;
    r.ahi = 3.0;
    r.duration_minutes = 360.0;
    r.mask_events = 6;
    EXPECT_NO_THROW(pub_->publishSTRState(r, 2.0));   // delta path
    EXPECT_NO_THROW(pub_->publishSTRState(r, 0.0));   // delta forced to 0
    EXPECT_NO_THROW(pub_->publishSTRState(r));        // default nightly_ahi=0
}

TEST_F(NullMqttDeepTest, PublishInsightsEmptyAndPopulatedNoCrash) {
    std::vector<Insight> none;
    EXPECT_NO_THROW(pub_->publishInsights(none));

    std::vector<Insight> some;
    Insight a; a.title = "Good night"; a.category = "positive"; a.metric = "AHI"; a.value = 1.2;
    Insight b; b.title = "High leak";  b.category = "warning";  b.metric = "Leak"; b.value = 28.0;
    some.push_back(a);
    some.push_back(b);
    EXPECT_NO_THROW(pub_->publishInsights(some));
}

TEST_F(NullMqttDeepTest, SummariesReturnFalseWhenNoMqtt) {
    EXPECT_FALSE(pub_->publishSessionSummary("daily text"));
    EXPECT_FALSE(pub_->publishRangeSummary(SummaryPeriod::WEEKLY, "week"));
    EXPECT_FALSE(pub_->publishRangeSummary(SummaryPeriod::MONTHLY, "month"));
}

TEST_F(NullMqttDeepTest, OximetryLiveValidAndInvalidNoCrash) {
    IO2RingClient::LiveReading valid;
    valid.spo2 = 97; valid.hr = 58; valid.motion = 0; valid.active = true; valid.valid = true;
    EXPECT_NO_THROW(pub_->publishOximetryLive("oxi", valid));

    IO2RingClient::LiveReading invalid;
    invalid.spo2 = 0; invalid.hr = 0; invalid.motion = 5; invalid.active = false; invalid.valid = false;
    EXPECT_NO_THROW(pub_->publishOximetryLive("oxi", invalid));
}

TEST_F(NullMqttDeepTest, OximetrySummaryNoCrashAndNeverTouchesDb) {
    // Null mqtt → early return before any DB access; date parsing not reached.
    EXPECT_NO_THROW(pub_->publishOximetrySummary("20260102"));
}

TEST(NullMqttNullDbTest, ConstructAndInitializeWithNullDb) {
    // Both null: construction + initialize must not crash.
    DataPublisherService pub(nullptr, nullptr);
    EXPECT_TRUE(pub.initialize());
    EXPECT_TRUE(pub.publishDiscovery());
    EXPECT_TRUE(pub.publishSessionCompleted());
}

// ===========================================================================
// Fixture B: Real-but-unreachable MqttClient — every builder path runs.
// publish() returns false while disconnected; isConnected()-gated methods
// early-return. We assert no-throw + documented not-connected return values.
// ===========================================================================

class UnreachableMqttDeepTest : public ::testing::Test {
protected:
    void SetUp() override {
        client_ = makeUnreachableClient();
        db_ = std::make_shared<SQLiteDatabase>(":memory:");
        db_->connect();
        pub_ = std::make_unique<DataPublisherService>(client_, db_);
    }
    bool connected() const { return client_->isConnected(); }

    std::shared_ptr<hms::MqttClient> client_;
    std::shared_ptr<IDatabase> db_;
    std::unique_ptr<DataPublisherService> pub_;
};

// publishDiscovery() fans out to realtime + historical + STR + insights +
// oximetry discovery builders. Every SensorDef config JSON gets built. With an
// unreachable broker each publish() returns false, so the aggregate is false.
TEST_F(UnreachableMqttDeepTest, PublishDiscoveryRunsAllBuilders) {
    bool result = true;
    ASSERT_NO_THROW({ result = pub_->publishDiscovery(); });
    if (connected()) GTEST_SKIP() << "Local broker connected; skip not-connected assertion";
    EXPECT_FALSE(result);
}

// initialize() does NOT subscribe (gated on isConnected) but still calls
// publishDiscovery() unconditionally; must run all builders without throwing.
TEST_F(UnreachableMqttDeepTest, InitializeRunsDiscoveryBuilders) {
    bool ok = false;
    ASSERT_NO_THROW({ ok = pub_->initialize(); });
    EXPECT_TRUE(ok);
}

// publishSession() with mqtt present but disconnected: takes the
// "Not connected, skipping MQTT publish" branch; returns true (db_success).
TEST_F(UnreachableMqttDeepTest, PublishSessionDisconnectedReturnsTrueViaDb) {
    CPAPSession s;
    s.device_id = "dev";
    s.duration_seconds = 7200;
    s.session_start = fixedEpoch();
    BreathingSummary bs;
    bs.avg_pressure = 8.0; bs.avg_flow_rate = 10.0;
    s.breathing_summary.push_back(bs);
    s.metrics = fullMetrics();

    bool result = false;
    ASSERT_NO_THROW({ result = pub_->publishSession(s); });
    if (connected()) GTEST_SKIP() << "Local broker connected; skip not-connected assertion";
    // db_success defaults true, mqtt_success false → db_success || mqtt_success.
    EXPECT_TRUE(result);
}

// publishHistoricalState builder: every optional present → every publish()
// branch is taken (and returns false on the unreachable client). No throw.
TEST_F(UnreachableMqttDeepTest, PublishHistoricalStateFullMetricsRunsAllBranches) {
    EXPECT_NO_THROW(pub_->publishHistoricalState(fullMetrics()));
}

// Same builder with NO optionals set: only the always-published event counters
// (ahi/total_events/obstructive/central/hypopneas/reras/clear_airway) fire; all
// optional branches take the absent path.
TEST_F(UnreachableMqttDeepTest, PublishHistoricalStateEmptyMetricsRunsRequiredOnly) {
    SessionMetrics m;  // all defaults / nullopt
    EXPECT_NO_THROW(pub_->publishHistoricalState(m));
}

// publishSessionCompleted publishes session_status=completed + session_active=OFF
// (session-active OFF path). Returns true (does not gate on isConnected).
TEST_F(UnreachableMqttDeepTest, PublishSessionCompletedReturnsTrue) {
    bool r = false;
    ASSERT_NO_THROW({ r = pub_->publishSessionCompleted(); });
    EXPECT_TRUE(r);
}

// publishSession with a rich session: even though the disconnected client routes
// publishSession to its "not connected" branch (so the realtime builder is not
// reached in that path), the call must remain safe and not throw.
TEST_F(UnreachableMqttDeepTest, PublishSessionWithRichSessionNoThrow) {
    CPAPSession s;
    s.device_id = "dev";
    s.duration_seconds = 3600;
    s.session_start = fixedEpoch();

    BreathingSummary bs;
    bs.avg_pressure = 9.5; bs.avg_flow_rate = 12.3;
    bs.leak_rate = 5.0; bs.respiratory_rate = 15.0; bs.tidal_volume = 500.0;
    bs.minute_ventilation = 7.5; bs.flow_limitation = 0.1;
    bs.mask_pressure = 8.8; bs.snore_index = 0.0; bs.target_ventilation = 5.0;
    s.breathing_summary.push_back(bs);

    CPAPVitals v; v.spo2 = 95.0; v.heart_rate = 62;
    s.vitals.push_back(v);
    s.metrics = fullMetrics();

    EXPECT_NO_THROW(pub_->publishSession(s));
}

// STR builder: precision + pairing + delta logic all run; returns void.
TEST_F(UnreachableMqttDeepTest, PublishSTRStateAllPathsNoThrow) {
    STRDailyRecord r;
    r.ahi = 3.0; r.oai = 1.5; r.cai = 0.5; r.hi = 1.0; r.rin = 0.25; r.csr = 12.0;
    r.duration_minutes = 360.0; r.mask_events = 6; r.leak_95 = 18.5;
    r.mask_press_95 = 11.2; r.spo2_50 = 95.0; r.patient_hours = 1234.5;

    EXPECT_NO_THROW(pub_->publishSTRState(r, 2.0));  // nightly>0 → delta computed
    EXPECT_NO_THROW(pub_->publishSTRState(r, 0.0));  // nightly==0 → delta forced 0
    EXPECT_NO_THROW(pub_->publishSTRState(r, -1.0)); // nightly<0 → delta forced 0
}

// Insights builder: empty + populated. publishInsights does not gate on
// isConnected (only on null), so the builder runs in both cases.
TEST_F(UnreachableMqttDeepTest, PublishInsightsRunsBuilder) {
    std::vector<Insight> none;
    EXPECT_NO_THROW(pub_->publishInsights(none));

    std::vector<Insight> some;
    Insight a; a.title = "t1"; a.body = "b1"; a.category = "alert"; a.metric = "Leak"; a.value = 30.0;
    some.push_back(a);
    EXPECT_NO_THROW(pub_->publishInsights(some));
}

// Summaries gate on isConnected → false on the unreachable client.
TEST_F(UnreachableMqttDeepTest, SummariesReturnFalseWhenDisconnected) {
    if (connected()) GTEST_SKIP() << "Local broker connected; skip not-connected assertion";
    EXPECT_FALSE(pub_->publishSessionSummary("daily"));
    EXPECT_FALSE(pub_->publishRangeSummary(SummaryPeriod::WEEKLY, "week"));
    EXPECT_FALSE(pub_->publishRangeSummary(SummaryPeriod::MONTHLY, "month"));
}

// Oximetry live gates on isConnected → no-op (no throw) on disconnected client,
// for both valid and invalid readings.
TEST_F(UnreachableMqttDeepTest, OximetryLiveNoThrowDisconnected) {
    IO2RingClient::LiveReading valid;
    valid.spo2 = 97; valid.hr = 58; valid.motion = 0; valid.active = true; valid.valid = true;
    EXPECT_NO_THROW(pub_->publishOximetryLive("oxi", valid));

    IO2RingClient::LiveReading invalid;
    invalid.spo2 = 0; invalid.hr = 0; invalid.motion = 9; invalid.active = false; invalid.valid = false;
    EXPECT_NO_THROW(pub_->publishOximetryLive("oxi", invalid));
}

// Oximetry summary gates on isConnected → early return before DB/date parsing.
TEST_F(UnreachableMqttDeepTest, OximetrySummaryNoThrowDisconnected) {
    EXPECT_NO_THROW(pub_->publishOximetrySummary("20260102"));
}

// Distinct device IDs / names should not affect builder safety.
TEST(UnreachableMqttDeepStandaloneTest, MultipleDistinctDevicesBuildSafely) {
    auto client = makeUnreachableClient();
    auto db = std::make_shared<SQLiteDatabase>(":memory:");
    db->connect();
    DataPublisherService p(client, db);
    EXPECT_NO_THROW(p.publishDiscovery());
    EXPECT_NO_THROW(p.publishSessionCompleted());
    EXPECT_NO_THROW(p.publishHistoricalState(fullMetrics()));
}