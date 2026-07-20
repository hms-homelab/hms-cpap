//
// test_SupplyPublisher.cpp — SDD-004 "Reminders": supply wear as Home Assistant
// entities over MQTT.
//
// SupplyPublisher publishes through a caller-supplied sink, so every test here
// runs with NO broker and NO database: the sink is a lambda that records topics
// and payloads, and snapshots are handed in directly. One test drives the real
// SQLite backend to prove collect() joins profiles, items and type defaults the
// way the rest of the suite expects.
//
// What must not regress:
//   * discovery topics/payload shape (HA silently ignores a malformed config)
//   * binary_sensor.cpap_supplies_due flipping with state
//   * untracked items (machines, undated accessories) never producing a sensor
//     that would read 0 forever
//   * entity id stability — a renamed entity orphans the user's automations
//   * an empty catalog publishing nothing instead of crashing
//
#include <gtest/gtest.h>

#include "services/SupplyPublisher.h"
#include "database/SQLiteDatabase.h"

#include <json/json.h>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace hms_cpap;

namespace {

constexpr long long kDay = 24LL * 60 * 60;
constexpr long long kNow = 1'750'000'000LL;  // fixed "now" so tests never drift
const std::string kDeviceId = "cpap_resmed_23243570851";

// Records every message the publisher emits.
struct Recorder {
    struct Message {
        std::string topic;
        std::string payload;
        bool retain{false};
    };

    std::vector<Message> messages;

    SupplyPublisher::PublishFn sink() {
        return [this](const std::string& topic, const std::string& payload, bool retain) {
            messages.push_back({topic, payload, retain});
            return true;
        };
    }

    bool has(const std::string& topic) const { return find(topic) != nullptr; }

    const Message* find(const std::string& topic) const {
        for (const auto& m : messages) {
            if (m.topic == topic) return &m;
        }
        return nullptr;
    }

    std::string payloadOf(const std::string& topic) const {
        const Message* m = find(topic);
        return m ? m->payload : std::string();
    }

    Json::Value jsonOf(const std::string& topic) const {
        Json::Value parsed;
        std::string body = payloadOf(topic);
        if (body.empty()) return parsed;
        Json::CharReaderBuilder builder;
        std::string errors;
        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        reader->parse(body.data(), body.data() + body.size(), &parsed, &errors);
        return parsed;
    }

    size_t countTopicsContaining(const std::string& needle) const {
        size_t n = 0;
        for (const auto& m : messages) {
            if (m.topic.find(needle) != std::string::npos) ++n;
        }
        return n;
    }
};

SupplyPublisher::Entry makeEntry(int id,
                                 const std::string& profile,
                                 const std::string& type,
                                 long long started_epoch,
                                 int interval_days,
                                 const std::string& category = "accessory") {
    SupplyPublisher::Entry e;
    e.item_id       = id;
    e.profile_name  = profile;
    e.type_key      = type;
    e.category      = category;
    e.brand         = "ResMed";
    e.model         = "AirFit P10";
    e.started_epoch = started_epoch;
    e.interval_days = interval_days;
    return e;
}

// A throwaway on-disk SQLite database, same pattern as test_EquipmentBackends.
class TempDb {
public:
    TempDb() {
        path_ = (std::filesystem::temp_directory_path() /
                 ("hms_supplypub_" + std::to_string(::getpid()) + "_" +
                  std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                    .string();
        std::filesystem::remove(path_);
        db_ = std::make_unique<SQLiteDatabase>(path_);
    }
    ~TempDb() {
        db_.reset();
        std::filesystem::remove(path_);
    }

    bool connect() { return db_->connect(); }
    IDatabase& db() { return *db_; }

private:
    std::string path_;
    std::unique_ptr<SQLiteDatabase> db_;
};

}  // namespace

// ── Discovery topics and payload shape ─────────────────────────────────

TEST(SupplyPublisherTest, PublishesDiscoveryForTrackedAccessory) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId, "CPAP");

    // 30 days into a 90-day mask: fresh, 60 days left, 33% worn.
    auto result = pub.publishSnapshot({makeEntry(1, "Travel Setup", "mask", kNow - 30 * kDay, 90)},
                                      kNow);

    EXPECT_EQ(result.entities, 1);
    EXPECT_EQ(result.skipped, 0);
    EXPECT_TRUE(result.published);
    EXPECT_TRUE(result.all_ok);

    const std::string cfg_topic =
        "homeassistant/sensor/" + kDeviceId + "/supply_travel_setup_mask_days_left/config";
    ASSERT_TRUE(rec.has(cfg_topic));

    Json::Value cfg = rec.jsonOf(cfg_topic);
    EXPECT_EQ(cfg["unique_id"].asString(), kDeviceId + "_supply_travel_setup_mask_days_left");
    EXPECT_EQ(cfg["state_topic"].asString(),
              "cpap/" + kDeviceId + "/supplies/travel_setup_mask/days_left");
    EXPECT_EQ(cfg["unit_of_measurement"].asString(), "d");
    EXPECT_EQ(cfg["state_class"].asString(), "measurement");
    EXPECT_FALSE(cfg["name"].asString().empty());

    // Grouped under the SAME HA device as the therapy sensors.
    ASSERT_TRUE(cfg["device"].isObject());
    ASSERT_EQ(cfg["device"]["identifiers"].size(), 1u);
    EXPECT_EQ(cfg["device"]["identifiers"][0].asString(), kDeviceId);

    // States land on the advertised topics.
    EXPECT_EQ(rec.payloadOf("cpap/" + kDeviceId + "/supplies/travel_setup_mask/days_left"), "60");
    EXPECT_EQ(rec.payloadOf("cpap/" + kDeviceId + "/supplies/travel_setup_mask/wear_percent"), "33");

    // Wear sensor advertises percent.
    Json::Value wear_cfg = rec.jsonOf("homeassistant/sensor/" + kDeviceId +
                                      "/supply_travel_setup_mask_wear_percent/config");
    EXPECT_EQ(wear_cfg["unit_of_measurement"].asString(), "%");
}

TEST(SupplyPublisherTest, AllMessagesAreRetained) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.publishSnapshot({makeEntry(1, "My CPAP", "filter", kNow - 10 * kDay, 30)}, kNow);

    ASSERT_FALSE(rec.messages.empty());
    for (const auto& m : rec.messages) {
        EXPECT_TRUE(m.retain) << m.topic << " must be retained so HA survives a restart";
    }
}

TEST(SupplyPublisherTest, AttributesCarryProfileBrandModelTypeAndReplaceBy) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);

    long long started = kNow - 30 * kDay;
    pub.publishSnapshot({makeEntry(1, "Travel Setup", "mask", started, 90)}, kNow);

    const std::string attrs_topic =
        "cpap/" + kDeviceId + "/supplies/travel_setup_mask/attributes";
    ASSERT_TRUE(rec.has(attrs_topic));

    Json::Value attrs = rec.jsonOf(attrs_topic);
    EXPECT_EQ(attrs["profile"].asString(), "Travel Setup");
    EXPECT_EQ(attrs["type"].asString(), "mask");
    EXPECT_EQ(attrs["brand"].asString(), "ResMed");
    EXPECT_EQ(attrs["model"].asString(), "AirFit P10");
    EXPECT_EQ(attrs["state"].asString(), "fresh");
    EXPECT_EQ(attrs["replace_by"].asString(), SupplyPublisher::formatDate(started + 90 * kDay));
    EXPECT_EQ(attrs["replace_by"].asString().size(), 10u);  // YYYY-MM-DD

    // Both sensors point at that attributes topic.
    Json::Value cfg = rec.jsonOf("homeassistant/sensor/" + kDeviceId +
                                 "/supply_travel_setup_mask_days_left/config");
    EXPECT_EQ(cfg["json_attributes_topic"].asString(), attrs_topic);
}

// ── The due binary_sensor ──────────────────────────────────────────────

TEST(SupplyPublisherTest, DueBinarySensorIsOffWhenEverythingIsFresh) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);

    auto result = pub.publishSnapshot(
        {makeEntry(1, "My CPAP", "mask", kNow - 5 * kDay, 90),
         makeEntry(2, "My CPAP", "tubing", kNow - 2 * kDay, 90)},
        kNow);

    EXPECT_EQ(result.due, 0);
    EXPECT_EQ(rec.payloadOf("cpap/" + kDeviceId + "/supplies/due"), "OFF");

    Json::Value cfg =
        rec.jsonOf("homeassistant/binary_sensor/" + kDeviceId + "/supplies_due/config");
    EXPECT_EQ(cfg["unique_id"].asString(), kDeviceId + "_supplies_due");
    EXPECT_EQ(cfg["state_topic"].asString(), "cpap/" + kDeviceId + "/supplies/due");
    EXPECT_EQ(cfg["device_class"].asString(), "problem");
}

TEST(SupplyPublisherTest, DueBinarySensorFlipsOnForDueSoon) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);

    // 80 days into a 90-day mask: 10 days left, inside the 14-day window.
    auto result =
        pub.publishSnapshot({makeEntry(1, "My CPAP", "mask", kNow - 80 * kDay, 90)}, kNow);

    EXPECT_EQ(result.due, 1);
    EXPECT_EQ(rec.payloadOf("cpap/" + kDeviceId + "/supplies/due"), "ON");
    EXPECT_EQ(rec.jsonOf("cpap/" + kDeviceId + "/supplies/my_cpap_mask/attributes")["state"]
                  .asString(),
              "due_soon");
}

TEST(SupplyPublisherTest, DueBinarySensorFlipsOnForOverdueAndListsTheItem) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);

    // One overdue filter alongside a fresh mask: the household sensor is ON.
    auto result = pub.publishSnapshot(
        {makeEntry(1, "My CPAP", "mask", kNow - 5 * kDay, 90),
         makeEntry(2, "My CPAP", "filter", kNow - 45 * kDay, 30)},
        kNow);

    EXPECT_EQ(result.entities, 2);
    EXPECT_EQ(result.due, 1);
    EXPECT_EQ(rec.payloadOf("cpap/" + kDeviceId + "/supplies/due"), "ON");

    Json::Value attrs = rec.jsonOf("cpap/" + kDeviceId + "/supplies/due/attributes");
    EXPECT_EQ(attrs["due_count"].asInt(), 1);
    EXPECT_EQ(attrs["tracked_count"].asInt(), 2);
    ASSERT_EQ(attrs["items"].size(), 1u);
    EXPECT_EQ(attrs["items"][0]["type"].asString(), "filter");
    EXPECT_EQ(attrs["items"][0]["state"].asString(), "overdue");
    EXPECT_LT(attrs["items"][0]["days_left"].asInt(), 0);

    // An overdue item is pinned at 100% wear, not beyond it.
    EXPECT_EQ(rec.payloadOf("cpap/" + kDeviceId + "/supplies/my_cpap_filter/wear_percent"), "100");
}

// ── Untracked items are skipped ────────────────────────────────────────

TEST(SupplyPublisherTest, SkipsMachineAndUndatedAccessories) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);

    auto result = pub.publishSnapshot(
        {
            // A machine never wears out: catalog default is -1.
            makeEntry(1, "My CPAP", "machine", kNow - 400 * kDay, -1, "machine"),
            // An accessory the user never dated.
            makeEntry(2, "My CPAP", "tubing", 0, 90),
            // A custom type with no interval configured.
            makeEntry(3, "My CPAP", "battery", kNow - 10 * kDay, 0),
            // The only genuinely tracked item.
            makeEntry(4, "My CPAP", "mask", kNow - 10 * kDay, 90),
        },
        kNow);

    EXPECT_EQ(result.entities, 1);
    EXPECT_EQ(result.skipped, 3);

    EXPECT_FALSE(rec.has("cpap/" + kDeviceId + "/supplies/my_cpap_machine/days_left"));
    EXPECT_FALSE(rec.has("cpap/" + kDeviceId + "/supplies/my_cpap_tubing/days_left"));
    EXPECT_FALSE(rec.has("cpap/" + kDeviceId + "/supplies/my_cpap_battery/days_left"));
    EXPECT_TRUE(rec.has("cpap/" + kDeviceId + "/supplies/my_cpap_mask/days_left"));

    // No discovery config was registered for the skipped ones either.
    EXPECT_EQ(rec.countTopicsContaining("homeassistant/sensor/"), 2u);  // days_left + wear
}

TEST(SupplyPublisherTest, SnapshotOfOnlyUntrackedItemsPublishesNothing) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);

    auto result = pub.publishSnapshot(
        {makeEntry(1, "My CPAP", "machine", kNow - 400 * kDay, -1, "machine")}, kNow);

    EXPECT_EQ(result.entities, 0);
    EXPECT_EQ(result.skipped, 1);
    EXPECT_FALSE(result.published);
    // Not even the household binary sensor: nothing to report on.
    EXPECT_TRUE(rec.messages.empty());
}

// ── Empty catalog ──────────────────────────────────────────────────────

TEST(SupplyPublisherTest, EmptySnapshotPublishesNothingAndDoesNotCrash) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);

    auto result = pub.publishSnapshot({}, kNow);

    EXPECT_EQ(result.entities, 0);
    EXPECT_EQ(result.skipped, 0);
    EXPECT_FALSE(result.published);
    EXPECT_TRUE(result.all_ok);
    EXPECT_TRUE(rec.messages.empty());
}

TEST(SupplyPublisherTest, NullSinkDoesNotCrash) {
    SupplyPublisher pub(nullptr, kDeviceId);
    auto result = pub.publishSnapshot({makeEntry(1, "My CPAP", "mask", kNow - 10 * kDay, 90)}, kNow);

    EXPECT_EQ(result.entities, 1);
    EXPECT_FALSE(result.all_ok);  // reported, not fatal
}

TEST(SupplyPublisherTest, SinkFailureIsReportedNotFatal) {
    int calls = 0;
    SupplyPublisher pub(
        [&calls](const std::string&, const std::string&, bool) {
            ++calls;
            return false;  // broker down
        },
        kDeviceId);

    auto result = pub.publishSnapshot({makeEntry(1, "My CPAP", "mask", kNow - 10 * kDay, 90)}, kNow);

    EXPECT_GT(calls, 0);
    EXPECT_EQ(result.entities, 1);
    EXPECT_FALSE(result.all_ok);
}

// ── Entity id stability ────────────────────────────────────────────────

TEST(SupplyPublisherTest, SlugifyIsStableAndAlnumOnly) {
    EXPECT_EQ(SupplyPublisher::slugify("Travel Setup"), "travel_setup");
    EXPECT_EQ(SupplyPublisher::slugify("travel setup"), "travel_setup");
    EXPECT_EQ(SupplyPublisher::slugify("  Travel   Setup  "), "travel_setup");
    EXPECT_EQ(SupplyPublisher::slugify("Albin's CPAP #2"), "albin_s_cpap_2");
    EXPECT_EQ(SupplyPublisher::slugify("Mask/Tubing"), "mask_tubing");
    EXPECT_EQ(SupplyPublisher::slugify(""), "");
}

TEST(SupplyPublisherTest, EntityKeyIsStableAcrossRuns) {
    auto entry = makeEntry(7, "Travel Setup", "mask", kNow - kDay, 90);
    EXPECT_EQ(SupplyPublisher::entityKey(entry), "travel_setup_mask");

    // Same profile + type => same key regardless of item id, brand or dates.
    auto other = makeEntry(99, "Travel Setup", "mask", kNow - 50 * kDay, 30);
    other.brand = "Fisher & Paykel";
    EXPECT_EQ(SupplyPublisher::entityKey(other), SupplyPublisher::entityKey(entry));
}

TEST(SupplyPublisherTest, DuplicateProfileTypePairsGetStableDistinctKeys) {
    // Two masks in one profile: the lower item id keeps the clean key, so adding
    // a second mask never renames the first one's HA entity.
    std::vector<SupplyPublisher::Entry> entries = {
        makeEntry(3, "My CPAP", "mask", kNow - kDay, 90),
        makeEntry(9, "My CPAP", "mask", kNow - kDay, 90),
    };

    auto keys = SupplyPublisher::entityKeys(entries);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "my_cpap_mask");
    EXPECT_EQ(keys[1], "my_cpap_mask_9");

    // Order of the input does not change the assignment.
    std::vector<SupplyPublisher::Entry> reversed = {entries[1], entries[0]};
    auto reversed_keys = SupplyPublisher::entityKeys(reversed);
    EXPECT_EQ(reversed_keys[0], "my_cpap_mask_9");
    EXPECT_EQ(reversed_keys[1], "my_cpap_mask");
}

TEST(SupplyPublisherTest, RepublishingProducesIdenticalTopics) {
    // The burst cycle republishes every ~65s; the topic set must not move.
    std::vector<SupplyPublisher::Entry> snapshot = {
        makeEntry(1, "Travel Setup", "mask", kNow - 30 * kDay, 90),
        makeEntry(2, "Travel Setup", "filter", kNow - 5 * kDay, 30),
    };

    Recorder first, second;
    SupplyPublisher(first.sink(), kDeviceId).publishSnapshot(snapshot, kNow);
    SupplyPublisher(second.sink(), kDeviceId).publishSnapshot(snapshot, kNow + 60);

    ASSERT_EQ(first.messages.size(), second.messages.size());
    for (size_t i = 0; i < first.messages.size(); ++i) {
        EXPECT_EQ(first.messages[i].topic, second.messages[i].topic);
    }
}

TEST(SupplyPublisherTest, FormatDateIsUtcAndEmptyForUnset) {
    EXPECT_EQ(SupplyPublisher::formatDate(0), "");
    EXPECT_EQ(SupplyPublisher::formatDate(-1), "");
    EXPECT_EQ(SupplyPublisher::formatDate(1'700'000'000LL), "2023-11-14");
}

// ── collect() against the real SQLite backend ──────────────────────────

TEST(SupplyPublisherTest, CollectJoinsProfilesItemsAndTypeDefaults) {
    TempDb temp;
    ASSERT_TRUE(temp.connect());
    IDatabase& db = temp.db();

    IDatabase::EquipmentProfile profile;
    profile.name = "Travel Setup";
    int profile_id = db.upsertEquipmentProfile(profile, "");
    ASSERT_GT(profile_id, 0);

    // Mask with no override: must pick up the seeded catalog default of 90.
    IDatabase::EquipmentItem mask;
    mask.profile_id = profile_id;
    mask.type_key = "mask";
    mask.category = "accessory";
    mask.brand = "ResMed";
    mask.started_using_at = "2026-01-01T00:00:00Z";
    mask.started_epoch = 1767225600LL;
    mask.replace_after_days = -1;  // NULL -> type default
    ASSERT_GT(db.upsertEquipmentItem(mask, ""), 0);

    // Filter with an explicit override: the override wins.
    IDatabase::EquipmentItem filter;
    filter.profile_id = profile_id;
    filter.type_key = "filter";
    filter.category = "accessory";
    filter.started_using_at = "2026-01-01T00:00:00Z";
    filter.started_epoch = 1767225600LL;
    filter.replace_after_days = 14;
    ASSERT_GT(db.upsertEquipmentItem(filter, ""), 0);

    auto entries = SupplyPublisher::collect(db);
    ASSERT_EQ(entries.size(), 2u);

    std::map<std::string, SupplyPublisher::Entry> by_type;
    for (const auto& e : entries) by_type[e.type_key] = e;

    ASSERT_EQ(by_type.count("mask"), 1u);
    EXPECT_EQ(by_type["mask"].profile_name, "Travel Setup");
    EXPECT_EQ(by_type["mask"].interval_days, 90);
    EXPECT_EQ(by_type["mask"].brand, "ResMed");
    EXPECT_GT(by_type["mask"].started_epoch, 0);

    ASSERT_EQ(by_type.count("filter"), 1u);
    EXPECT_EQ(by_type["filter"].interval_days, 14) << "per-item override must beat the default";

    // Entries are id-ordered so entity keys stay stable across restarts.
    EXPECT_LT(entries[0].item_id, entries[1].item_id);
}

TEST(SupplyPublisherTest, EmptyCatalogPublishesNothingFromDatabase) {
    TempDb temp;
    ASSERT_TRUE(temp.connect());

    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    auto result = pub.publishFromDatabase(temp.db(), kNow);

    EXPECT_EQ(result.entities, 0);
    EXPECT_FALSE(result.published);
    EXPECT_TRUE(rec.messages.empty());
}

// -- Transition events -------------------------------------------------------
//
// The retained sensors say what IS true; these say what just CHANGED. The whole
// point is that an automation can fire once on a crossing instead of re-firing
// every burst cycle, so the tests below are mostly about NOT emitting.

namespace {

const std::string kEventTopic = "cpap/" + kDeviceId + "/supplies/event";

// In-memory stand-in for the sidecar file, so a test can simulate a restart by
// building a second publisher over the same backing string.
struct MemLedger {
    std::string blob;
    SupplyPublisher::LedgerIO io() {
        SupplyPublisher::LedgerIO l;
        l.read  = [this]() { return blob; };
        l.write = [this](const std::string& t) { blob = t; };
        return l;
    }
};

std::vector<Recorder::Message> eventsIn(const Recorder& rec) {
    std::vector<Recorder::Message> out;
    for (const auto& m : rec.messages) {
        if (m.topic == kEventTopic) out.push_back(m);
    }
    return out;
}

}  // namespace

TEST(SupplyEventTest, CrossingIntoOverdueEmitsAnEvent) {
    Recorder rec;
    MemLedger ledger;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.setLedger(ledger.io());

    // Fresh first, so the ledger has a "fresh" baseline to cross away from.
    pub.publishSnapshot({makeEntry(1, "Home", "mask", kNow - 10 * kDay, 90)}, kNow);
    ASSERT_TRUE(eventsIn(rec).empty()) << "a fresh mask is not news";

    rec.messages.clear();
    pub.publishSnapshot({makeEntry(1, "Home", "mask", kNow - 100 * kDay, 90)}, kNow);

    auto events = eventsIn(rec);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_FALSE(events[0].retain) << "an event must not replay on reconnect";

    Json::Value body = rec.jsonOf(kEventTopic);
    EXPECT_EQ(body["entity"].asString(), "home_mask");
    EXPECT_EQ(body["from"].asString(), "fresh");
    EXPECT_EQ(body["state"].asString(), "overdue");
    EXPECT_EQ(body["profile"].asString(), "Home");
    EXPECT_EQ(body["type"].asString(), "mask");
    EXPECT_LT(body["days_left"].asInt(), 0);
}

TEST(SupplyEventTest, HoldingTheSameStateEmitsNothingOnLaterCycles) {
    Recorder rec;
    MemLedger ledger;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.setLedger(ledger.io());

    auto overdue = makeEntry(1, "Home", "mask", kNow - 100 * kDay, 90);
    auto first = pub.publishSnapshot({overdue}, kNow);
    EXPECT_EQ(first.events, 1) << "already-overdue on first sighting IS news";

    rec.messages.clear();
    for (int cycle = 0; cycle < 5; ++cycle) {
        auto later = pub.publishSnapshot({overdue}, kNow + cycle * kDay);
        EXPECT_EQ(later.events, 0) << "cycle " << cycle << " re-announced a stale crossing";
    }
    EXPECT_TRUE(eventsIn(rec).empty());
}

TEST(SupplyEventTest, AFreshItemSeenForTheFirstTimeIsSilent) {
    Recorder rec;
    MemLedger ledger;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.setLedger(ledger.io());

    auto result = pub.publishSnapshot({makeEntry(1, "Home", "tubing", kNow - 1 * kDay, 90)}, kNow);
    EXPECT_EQ(result.events, 0);
    EXPECT_TRUE(eventsIn(rec).empty());
}

TEST(SupplyEventTest, RestartDoesNotReplayACrossingAlreadyAnnounced) {
    MemLedger ledger;  // survives both publishers, like the sidecar file
    auto overdue = makeEntry(1, "Home", "mask", kNow - 100 * kDay, 90);

    Recorder before;
    {
        SupplyPublisher pub(before.sink(), kDeviceId);
        pub.setLedger(ledger.io());
        EXPECT_EQ(pub.publishSnapshot({overdue}, kNow).events, 1);
    }

    Recorder after;
    SupplyPublisher restarted(after.sink(), kDeviceId);
    restarted.setLedger(ledger.io());
    EXPECT_EQ(restarted.publishSnapshot({overdue}, kNow).events, 0)
        << "restarting the service must not re-notify an unchanged overdue item";
    EXPECT_TRUE(eventsIn(after).empty());
}

TEST(SupplyEventTest, ReplacingAnItemEmitsTheCrossingBackToFresh) {
    Recorder rec;
    MemLedger ledger;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.setLedger(ledger.io());

    pub.publishSnapshot({makeEntry(1, "Home", "mask", kNow - 100 * kDay, 90)}, kNow);
    rec.messages.clear();

    // User fits a new mask: started_epoch resets to today.
    auto result = pub.publishSnapshot({makeEntry(1, "Home", "mask", kNow, 90)}, kNow);

    EXPECT_EQ(result.events, 1);
    Json::Value body = rec.jsonOf(kEventTopic);
    EXPECT_EQ(body["from"].asString(), "overdue");
    EXPECT_EQ(body["state"].asString(), "fresh")
        << "an automation needs this to clear the notification it raised";
}

TEST(SupplyEventTest, DueSoonThenOverdueEmitsBothCrossings) {
    Recorder rec;
    MemLedger ledger;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.setLedger(ledger.io());

    // 80/90 days in: inside the 14-day window, not yet past.
    pub.publishSnapshot({makeEntry(1, "Home", "mask", kNow - 80 * kDay, 90)}, kNow);
    ASSERT_EQ(eventsIn(rec).size(), 1u);
    EXPECT_EQ(rec.jsonOf(kEventTopic)["state"].asString(), "due_soon");

    rec.messages.clear();
    pub.publishSnapshot({makeEntry(1, "Home", "mask", kNow - 95 * kDay, 90)}, kNow);
    ASSERT_EQ(eventsIn(rec).size(), 1u);
    Json::Value body = rec.jsonOf(kEventTopic);
    EXPECT_EQ(body["from"].asString(), "due_soon");
    EXPECT_EQ(body["state"].asString(), "overdue");
}

TEST(SupplyEventTest, UntrackedItemsNeverEmitEvents) {
    Recorder rec;
    MemLedger ledger;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.setLedger(ledger.io());

    auto result = pub.publishSnapshot(
        {makeEntry(1, "Home", "machine", kNow - 900 * kDay, -1, "machine"),
         makeEntry(2, "Home", "mask", 0, 90)},   // never dated
        kNow);

    EXPECT_EQ(result.events, 0) << "a machine has no wear clock to cross";
    EXPECT_TRUE(eventsIn(rec).empty());
}

TEST(SupplyEventTest, DeletingAnItemForgetsItSoReAddingAnnouncesAgain) {
    Recorder rec;
    MemLedger ledger;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.setLedger(ledger.io());

    auto overdue = makeEntry(1, "Home", "mask", kNow - 100 * kDay, 90);
    pub.publishSnapshot({overdue}, kNow);
    pub.publishSnapshot({}, kNow);        // item removed from the catalog
    rec.messages.clear();

    EXPECT_EQ(pub.publishSnapshot({overdue}, kNow).events, 1)
        << "a re-added item is a first sighting again";
}

TEST(SupplyEventTest, ACorruptLedgerIsTreatedAsEmptyRatherThanThrowing) {
    Recorder rec;
    MemLedger ledger;
    ledger.blob = "{not json at all";

    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.setLedger(ledger.io());

    SupplyPublisher::Result result;
    ASSERT_NO_THROW(result = pub.publishSnapshot(
                        {makeEntry(1, "Home", "mask", kNow - 100 * kDay, 90)}, kNow));
    EXPECT_EQ(result.events, 1);
    EXPECT_TRUE(result.all_ok);
}

TEST(SupplyEventTest, WithNoLedgerEventsStillDoNotRepeatWithinOneRun) {
    Recorder rec;
    SupplyPublisher pub(rec.sink(), kDeviceId);  // no setLedger at all

    auto overdue = makeEntry(1, "Home", "mask", kNow - 100 * kDay, 90);
    EXPECT_EQ(pub.publishSnapshot({overdue}, kNow).events, 1);
    EXPECT_EQ(pub.publishSnapshot({overdue}, kNow).events, 1)
        << "without persistence each publish re-derives from an empty ledger";
}

TEST(SupplyEventTest, StateSensorsStayRetainedEvenWhenAnEventFires) {
    Recorder rec;
    MemLedger ledger;
    SupplyPublisher pub(rec.sink(), kDeviceId);
    pub.setLedger(ledger.io());

    pub.publishSnapshot({makeEntry(1, "Home", "mask", kNow - 100 * kDay, 90)}, kNow);

    for (const auto& m : rec.messages) {
        if (m.topic == kEventTopic) {
            EXPECT_FALSE(m.retain) << "event topic must not be retained";
        } else {
            EXPECT_TRUE(m.retain) << m.topic << " lost its retain flag";
        }
    }
}
