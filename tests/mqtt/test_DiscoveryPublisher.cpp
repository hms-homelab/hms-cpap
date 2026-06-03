/**
 * HMS-CPAP DiscoveryPublisher Unit Tests
 *
 * Exercises the Home Assistant MQTT discovery config/topic builder.
 *
 * DiscoveryPublisher publishes via hms::MqttClient::publish(), which is a
 * non-virtual method that returns false (without throwing) whenever the
 * underlying Paho client is not connected. We therefore drive the publisher
 * with a *real* MqttClient pointed at an unreachable broker (never connected),
 * which lets every payload/topic builder path run to completion while keeping
 * the test fully deterministic and broker-free.
 *
 * If a local broker happens to be listening (CI with mosquitto), publish()
 * could succeed; the tests that assert on the "not connected" return value
 * GTEST_SKIP in that case so the suite stays deterministic either way.
 */

#include <gtest/gtest.h>

#include "mqtt/DiscoveryPublisher.h"
#include "mqtt_client.h"

#include <memory>
#include <string>

using hms_cpap::DiscoveryPublisher;

namespace {

// Build an MqttClient config that points at a guaranteed-unreachable broker so
// the client never establishes a connection. Unique client_id per pid avoids
// any accidental collisions.
hms::MqttConfig unreachableConfig() {
    hms::MqttConfig cfg;
    // RFC 5737 TEST-NET-1 address: never routable.
    cfg.broker = "192.0.2.1";
    cfg.port = 1883;
    cfg.client_id = "disc_pub_test_" + std::to_string(static_cast<long>(::getpid()));
    return cfg;
}

std::shared_ptr<hms::MqttClient> makeClient() {
    auto client = std::make_shared<hms::MqttClient>(unreachableConfig());
    // Intentionally do NOT call connect(); publish() must stay broker-gated.
    return client;
}

// A connected client would make publishAll/removeDevice return true; bail in
// that situation so behavioral assertions remain deterministic.
bool brokerLikelyConnected(const std::shared_ptr<hms::MqttClient>& c) {
    return c->isConnected();
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(DiscoveryPublisherTest, ConstructsWithDefaultManufacturerAndModel) {
    auto client = makeClient();
    // Default manufacturer/model arguments exercised.
    DiscoveryPublisher pub(client, "apc_ups", "Docker NUT UPS");
    SUCCEED();
}

TEST(DiscoveryPublisherTest, ConstructsWithCustomManufacturerAndModel) {
    auto client = makeClient();
    DiscoveryPublisher pub(client, "apc_ups", "Docker NUT UPS",
                           "American Power Conversion", "Back-UPS XS 1000M");
    SUCCEED();
}

// ---------------------------------------------------------------------------
// publishAll - exercises every sensor + binary sensor builder path
// ---------------------------------------------------------------------------

TEST(DiscoveryPublisherTest, PublishAllRunsAllBuildersWithoutThrowing) {
    auto client = makeClient();
    DiscoveryPublisher pub(client, "apc_ups", "Docker NUT UPS");

    // Builds 25 sensor configs + 1 binary sensor config; must not throw even
    // though delivery fails on the unreachable broker.
    bool result = false;
    ASSERT_NO_THROW({ result = pub.publishAll(); });

    if (brokerLikelyConnected(client)) {
        GTEST_SKIP() << "Local broker connected; skipping not-connected assertion";
    }
    // With no broker, every publish() returns false, so aggregate is false.
    EXPECT_FALSE(result);
}

TEST(DiscoveryPublisherTest, PublishAllIsIdempotentAcrossRepeatedCalls) {
    auto client = makeClient();
    DiscoveryPublisher pub(client, "cpap_resmed", "ResMed AirSense 11");

    bool r1 = false, r2 = false;
    ASSERT_NO_THROW({ r1 = pub.publishAll(); });
    ASSERT_NO_THROW({ r2 = pub.publishAll(); });

    if (brokerLikelyConnected(client)) {
        GTEST_SKIP() << "Local broker connected; skipping not-connected assertion";
    }
    EXPECT_EQ(r1, r2);
    EXPECT_FALSE(r1);
}

// Different device IDs must not crash and each builds its own topic namespace.
TEST(DiscoveryPublisherTest, PublishAllWorksForMultipleDistinctDevices) {
    auto client = makeClient();
    DiscoveryPublisher a(client, "device_alpha", "Alpha");
    DiscoveryPublisher b(client, "device_beta", "Beta", "Acme", "ModelX");

    ASSERT_NO_THROW({ a.publishAll(); });
    ASSERT_NO_THROW({ b.publishAll(); });
    SUCCEED();
}

// ---------------------------------------------------------------------------
// removeDevice - exercises the teardown / empty-retained-message path
// ---------------------------------------------------------------------------

TEST(DiscoveryPublisherTest, RemoveDeviceRunsAllClearsWithoutThrowing) {
    auto client = makeClient();
    DiscoveryPublisher pub(client, "apc_ups", "Docker NUT UPS");

    bool result = false;
    ASSERT_NO_THROW({ result = pub.removeDevice(); });

    if (brokerLikelyConnected(client)) {
        GTEST_SKIP() << "Local broker connected; skipping not-connected assertion";
    }
    EXPECT_FALSE(result);
}

TEST(DiscoveryPublisherTest, PublishAllThenRemoveDeviceSequence) {
    auto client = makeClient();
    DiscoveryPublisher pub(client, "apc_ups", "Docker NUT UPS");

    ASSERT_NO_THROW({ pub.publishAll(); });
    ASSERT_NO_THROW({ pub.removeDevice(); });
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Device IDs containing characters that flow into topics/unique_ids
// ---------------------------------------------------------------------------

TEST(DiscoveryPublisherTest, HandlesLongUnderscoredDeviceId) {
    auto client = makeClient();
    // Mirrors the real CPAP device id form used in production topics.
    DiscoveryPublisher pub(client, "cpap_resmed_23243570851",
                           "ResMed AirSense 11");
    ASSERT_NO_THROW({ pub.publishAll(); });
    ASSERT_NO_THROW({ pub.removeDevice(); });
    SUCCEED();
}
