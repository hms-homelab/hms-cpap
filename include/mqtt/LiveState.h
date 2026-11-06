#pragma once
//
// The night, live, on the house MQTT broker: the same topics and the same Home
// Assistant entities whichever source sees it.
//
//   cpap/<device_id>/realtime/mask          ON / OFF, retained
//   cpap/<device_id>/realtime/mask_changed  the change that set it, retained:
//       {"state":"on","source":"ezshare","source_id":"card",
//        "session":"20261104_231502",
//        "occurred_at":"2026-11-05T04:15:02Z","detected_at":"2026-11-05T04:17:10Z",
//        "delay_s":128}
//   cpap/<device_id>/realtime/event         one respiratory event, not retained
//   cpap/<device_id>/realtime/live_events   counts since the mask went on, retained
//
// occurred_at and detected_at mean what they mean on the CpapDash bus (the
// therapy domain's mask.on / mask.off): when it happened, on the machine's
// clock read on the real one, and when hms-cpap saw it, both UTC ISO-8601 with
// 'Z'. A source that streams sees a change within seconds; a card read over
// WiFi or from a folder sees it a burst or two later, and says so in delay_s.
//
// The sources (`source`): sleep_bridge, mule_ble, as11_ble (live), ezshare,
// flashair, local (a card). One mask for the device: it is on while any source
// says it is on.
//
// Home Assistant discovers two entities on the device every other hms-cpap
// sensor belongs to (identifiers: the device id): "Mask", a binary sensor with
// the change as its attributes, and "Live events", the count with the counts
// per type as attributes. No MQTT here: it is given a publish function, so the
// unit suite reads what would go out.
//
// After a restart nothing here knows the mask, and the broker still holds
// whatever was said last. A source that sees the mask says it again; if none
// says anything for `settle` after the start, OFF is published once (source
// "startup"). It rides the callers' own ticks (the Sleep Bridge's, the burst
// cycle's); there is no timer here.
//
#include <json/json.h>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace hms_cpap {

class LiveState {
public:
    using Clock = std::chrono::system_clock;
    /// Sends one message; false when the broker is not there.
    using Publish = std::function<bool(const std::string& topic, const std::string& payload, int qos,
                                       bool retain)>;

    static constexpr const char* kSleepBridge = "sleep_bridge";
    static constexpr const char* kMuleBle     = "mule_ble";
    static constexpr const char* kAs11Ble     = "as11_ble";
    static constexpr const char* kEzShare     = "ezshare";
    static constexpr const char* kFlashAir    = "flashair";
    static constexpr const char* kLocal       = "local";

    struct MaskChange {
        std::string       source;      // one of the k* above
        std::string       source_id;   // the bridge, the unit, the machine's serial, "card"
        bool              on{false};
        Clock::time_point occurred_at; // when it happened
        std::string       session;     // the session's file stem when known, e.g. 20261104_231502
    };

    LiveState(Publish publish, std::string device_id, std::string device_name,
              Clock::time_point started = Clock::now(),
              std::chrono::seconds settle = std::chrono::seconds(300));

    /// The Home Assistant discovery configs, retained. At start.
    bool announce();
    /// A source is seen for the first time this run: the discovery is said
    /// again (a broker away at start gets it then). Later calls do nothing.
    void sourceSeen(const std::string& source_id);
    /// A source's mask went on or off.
    void mask(const MaskChange& c, Clock::time_point detected_at = Clock::now());
    /// One respiratory event. `e` carries type (OA, CA, UA, H, RERA, CSR, FL,
    /// OTHER), description, duration_s, source and whatever the source adds;
    /// occurred_at, detected_at and delay_s are added here, and the counts.
    void event(Json::Value e, Clock::time_point occurred_at,
               Clock::time_point detected_at = Clock::now());
    /// Time passing: after a restart, OFF once no source said the mask for
    /// `settle`.
    void tick(Clock::time_point now = Clock::now());

    bool maskOn() const;
    /// The mask is off, and a source that streams the night (the Sleep
    /// Bridge, the Mule & Miner over Bluetooth, the AirSense 11 over
    /// Bluetooth) said so: an end seen as it happened, not a card read a
    /// burst later or the startup default. The session's end on MQTT
    /// (session_active / session_status) waits on this instead of the night's
    /// hour (issue #57).
    bool streamSaidOff() const;
    static bool isStreamingSource(const std::string& source);

    /// The session's own topics, realtime/session_status (in_progress /
    /// completed) and realtime/session_active (ON / OFF), retained, for a
    /// source no burst reads while it runs: the AirSense 11 or AirCurve 11
    /// streaming over this computer's Bluetooth, whose night is imported
    /// only when its stream ends (issue #50). The same topics the burst's
    /// publisher uses.
    void session(bool on);

    std::string topic(const std::string& leaf) const;
    std::string maskConfigTopic() const;
    std::string eventsConfigTopic() const;

    /// "2026-11-05T04:15:02Z".
    static std::string isoUtc(Clock::time_point t);

private:
    enum class Said { Nothing, On, Off };
    bool send(const std::string& leaf, const std::string& payload, bool retain, int qos = 1);
    void sendMask(bool on, const std::string& source, const std::string& source_id,
                  const std::string& session, Clock::time_point occurred, Clock::time_point detected);
    void sendCounts();

    Publish                    publish_;
    std::string                device_id_, device_name_;
    Clock::time_point          started_;
    std::chrono::seconds       settle_;
    mutable std::mutex         mu_;
    Said                       said_{Said::Nothing};
    std::string                session_;   // of the last on said
    std::string                off_from_;  // the source of the last off said
    std::set<std::string>      on_;      // the sources whose mask is on
    std::set<std::string>      seen_;
    std::map<std::string, int> counts_;
    int                        total_{0};
};

}  // namespace hms_cpap
