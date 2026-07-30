//
// test_CleaningPublisher.cpp — SDD-007 phase 2.
//
// The publisher never touches an MQTT client: it sends through an injected sink,
// so every topic, payload and retain flag is captured here with no broker
// running. Same shape as test_SupplyPublisher.cpp.
//
// The property that matters most is the CLEAR path. A task the user switches off
// must actively remove its Home Assistant entity, because a retained payload
// left behind strands a sensor reporting a schedule that is no longer running,
// and that is worse than never having published it.
//
#include <gtest/gtest.h>

#include "services/CleaningPublisher.h"
#include "services/CleaningStatus.h"

#include <json/json.h>

#include <map>
#include <string>
#include <vector>

using namespace hms_cpap;

namespace {

constexpr long long kDay = 24LL * 60 * 60;
// 2026-07-30 00:00:00 UTC, an exact day boundary so slot maths stays readable.
constexpr long long kJul30 = 1785369600LL;
constexpr int k0830 = 8 * 60 + 30;

long long at(long long day, int minutes) {
    return day + static_cast<long long>(minutes) * 60;
}

struct Sent {
    std::string topic;
    std::string payload;
    bool        retain{false};
};

class Sink {
public:
    CleaningPublisher::PublishFn fn() {
        return [this](const std::string& t, const std::string& p, bool r) {
            sent.push_back({t, p, r});
            return ok;
        };
    }

    /// Last payload published to a topic, or nullopt when never published.
    std::optional<std::string> last(const std::string& topic) const {
        std::optional<std::string> out;
        for (const auto& s : sent) if (s.topic == topic) out = s.payload;
        return out;
    }

    bool sentTo(const std::string& topic) const {
        for (const auto& s : sent) if (s.topic == topic) return true;
        return false;
    }

    std::vector<Sent> sent;
    bool ok{true};
};

CleaningPublisher::Entry makeEntry(int id, const std::string& key, int interval,
                                   bool enabled = true) {
    CleaningPublisher::Entry e;
    e.task_id          = id;
    e.profile_name     = "Bedroom";
    e.task_key         = key;
    e.label            = "Wash the " + key;
    e.interval_days    = interval;
    e.time_minutes     = k0830;
    e.start_date_epoch = kJul30;
    e.enabled          = enabled;
    return e;
}

Json::Value parse(const std::string& raw) {
    Json::Value v;
    Json::CharReaderBuilder b;
    std::string errs;
    std::istringstream is(raw);
    Json::parseFromStream(b, is, &v, &errs);
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────

TEST(CleaningPublisher, PublishesTheCanonicalStateString) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1", "CPAP");

    // Starts today at 08:30; at 09:00 it is due.
    auto r = pub.publishSnapshot({makeEntry(7, "mask_wipe", 1)}, at(kJul30, 9 * 60));

    EXPECT_EQ(r.entities, 1);
    EXPECT_EQ(r.due, 1);
    EXPECT_TRUE(r.all_ok);

    const auto state = sink.last("cpap/cpap_1/cleaning/task_7/state");
    ASSERT_TRUE(state.has_value());
    // The same lowercase vocabulary the API and the cloud use, so one automation
    // works against either.
    EXPECT_EQ(*state, "due");
}

TEST(CleaningPublisher, UpcomingTaskIsNotCountedAsDue) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    auto r = pub.publishSnapshot({makeEntry(7, "mask_wipe", 1)}, at(kJul30, 6 * 60));

    EXPECT_EQ(r.due, 0);
    EXPECT_EQ(*sink.last("cpap/cpap_1/cleaning/task_7/state"), "upcoming");
}

TEST(CleaningPublisher, AttributesCarryWhatAnAutomationNeeds) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    pub.publishSnapshot({makeEntry(7, "tubing_wash", 7)}, at(kJul30, 9 * 60));

    const auto raw = sink.last("cpap/cpap_1/cleaning/task_7/attributes");
    ASSERT_TRUE(raw.has_value());
    const auto a = parse(*raw);
    EXPECT_EQ(a["profile"].asString(), "Bedroom");
    EXPECT_EQ(a["task_key"].asString(), "tubing_wash");
    EXPECT_EQ(a["interval_days"].asInt(), 7);
    EXPECT_FALSE(a["next_due"].asString().empty())
        << "without next_due an automation cannot say when, only whether";
}

TEST(CleaningPublisher, EverythingIsRetainedSoHaSurvivesARestart) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    pub.publishSnapshot({makeEntry(7, "mask_wipe", 1)}, at(kJul30, 9 * 60));

    ASSERT_FALSE(sink.sent.empty());
    for (const auto& s : sink.sent) {
        EXPECT_TRUE(s.retain)
            << "unretained: " << s.topic
            << " — a subscriber connecting later would see no schedule at all";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Clearing, which is the part that goes wrong quietly
// ─────────────────────────────────────────────────────────────────────────────

TEST(CleaningPublisher, DisabledTaskClearsItsEntity) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    auto r = pub.publishSnapshot({makeEntry(7, "mask_wipe", 1, /*enabled=*/false)},
                                 at(kJul30, 9 * 60));

    EXPECT_EQ(r.entities, 0);
    EXPECT_EQ(r.cleared, 1);

    // An empty retained payload on the discovery topic is how MQTT discovery
    // removes an entity. Anything else leaves a sensor in HA reporting a
    // schedule the user has switched off.
    const auto cfg = sink.last("homeassistant/sensor/cpap_1/cleaning_task_7/config");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(*cfg, "") << "the entity was not removed from Home Assistant";

    EXPECT_EQ(*sink.last("cpap/cpap_1/cleaning/task_7/state"), "");
    EXPECT_EQ(*sink.last("cpap/cpap_1/cleaning/task_7/attributes"), "");
}

TEST(CleaningPublisher, DeletedTaskClearsItsEntityToo) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    auto e = makeEntry(9, "filter_check", 30);
    e.deleted = true;   // enabled but tombstoned: still must not publish a state

    auto r = pub.publishSnapshot({e}, at(kJul30, 9 * 60));
    EXPECT_EQ(r.entities, 0);
    EXPECT_EQ(r.cleared, 1);
    EXPECT_EQ(*sink.last("homeassistant/sensor/cpap_1/cleaning_task_9/config"), "");
}

TEST(CleaningPublisher, EntityKeyIsTheIdNotTheLabel) {
    // A rename must keep the same entity. Keying on a slug of the label would
    // orphan the old sensor and silently create a second one.
    auto a = makeEntry(7, "mask_wipe", 1);
    auto b = makeEntry(7, "mask_wipe", 1);
    b.label = "Completely different wording";
    EXPECT_EQ(CleaningPublisher::entityKey(a), CleaningPublisher::entityKey(b));

    auto c = makeEntry(8, "mask_wipe", 1);
    EXPECT_NE(CleaningPublisher::entityKey(a), CleaningPublisher::entityKey(c));
}

// ─────────────────────────────────────────────────────────────────────────────
// The household summary
// ─────────────────────────────────────────────────────────────────────────────

TEST(CleaningPublisher, AnyDueIsOnWhenSomethingIsDue) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    pub.publishSnapshot({makeEntry(1, "mask_wipe", 1),
                         makeEntry(2, "tubing_wash", 7)},
                        at(kJul30, 9 * 60));

    EXPECT_EQ(*sink.last("cpap/cpap_1/cleaning/any_due"), "ON");
    const auto a = parse(*sink.last("cpap/cpap_1/cleaning/any_due/attributes"));
    EXPECT_EQ(a["count"].asInt(), 2);
    EXPECT_EQ(a["tasks"].size(), 2u)
        << "the summary must name what is due, or it only says that something is";
}

TEST(CleaningPublisher, AnyDueIsOffWhenNothingIsDue) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    pub.publishSnapshot({makeEntry(1, "mask_wipe", 1)}, at(kJul30, 6 * 60));

    EXPECT_EQ(*sink.last("cpap/cpap_1/cleaning/any_due"), "OFF");
}

TEST(CleaningPublisher, AnyDueIsOffWhenEveryTaskIsDisabled) {
    // A household that switched everything off must read OFF, not stay stuck ON
    // from a previous cycle.
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    pub.publishSnapshot({makeEntry(1, "mask_wipe", 1, /*enabled=*/false)},
                        at(kJul30, 9 * 60));

    EXPECT_EQ(*sink.last("cpap/cpap_1/cleaning/any_due"), "OFF");
}

// ─────────────────────────────────────────────────────────────────────────────
// Housekeeping
// ─────────────────────────────────────────────────────────────────────────────

TEST(CleaningPublisher, EmptyScheduleSendsNothing) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    auto r = pub.publishSnapshot({}, at(kJul30, 9 * 60));

    EXPECT_FALSE(r.published);
    EXPECT_TRUE(sink.sent.empty())
        << "a household with no cleaning schedule got MQTT traffic anyway";
}

TEST(CleaningPublisher, ARejectedMessageIsReportedNotSwallowed) {
    Sink sink;
    sink.ok = false;
    CleaningPublisher pub(sink.fn(), "cpap_1");
    auto r = pub.publishSnapshot({makeEntry(1, "mask_wipe", 1)}, at(kJul30, 9 * 60));

    EXPECT_FALSE(r.all_ok) << "a broker rejection was reported as success";
}

TEST(CleaningPublisher, DiscoveryGroupsUnderTheExistingCpapDevice) {
    Sink sink;
    CleaningPublisher pub(sink.fn(), "cpap_1", "My CPAP");
    pub.publishSnapshot({makeEntry(1, "mask_wipe", 1)}, at(kJul30, 9 * 60));

    const auto cfg = parse(*sink.last("homeassistant/sensor/cpap_1/cleaning_task_1/config"));
    ASSERT_TRUE(cfg["device"]["identifiers"].isArray());
    EXPECT_EQ(cfg["device"]["identifiers"][0].asString(), "cpap_1")
        << "cleaning must join the existing CPAP device, not create a second one";
    EXPECT_EQ(cfg["unique_id"].asString(), "cpap_1_cleaning_task_1");
}

TEST(CleaningPublisher, NextDueTracksCompletion) {
    // Marking done is what advances the clock, and the published attribute has
    // to move with it or the sensor keeps advertising the old date.
    Sink before_sink, after_sink;
    auto e = makeEntry(3, "mask_wash", 7);

    CleaningPublisher before(before_sink.fn(), "cpap_1");
    before.publishSnapshot({e}, at(kJul30, 9 * 60));
    const auto a1 = parse(*before_sink.last("cpap/cpap_1/cleaning/task_3/attributes"));

    e.last_done_epoch = at(kJul30, 10 * 60);
    CleaningPublisher after(after_sink.fn(), "cpap_1");
    after.publishSnapshot({e}, at(kJul30, 11 * 60));
    const auto a2 = parse(*after_sink.last("cpap/cpap_1/cleaning/task_3/attributes"));

    EXPECT_NE(a1["next_due"].asString(), a2["next_due"].asString());
    EXPECT_EQ(*after_sink.last("cpap/cpap_1/cleaning/task_3/state"), "upcoming");
    EXPECT_FALSE(a2["last_done"].asString().empty());
}

}  // namespace
