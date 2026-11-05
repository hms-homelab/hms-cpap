#pragma once
//
// The Sleep Bridge's mask on the house MQTT broker, and the Home Assistant
// entity that shows it:
//
//   cpap/<device_id>/realtime/mask                             ON / OFF, retained
//   homeassistant/binary_sensor/<device_id>/mask/config        its discovery, retained
//
// LiveNight decides when the mask goes on and off; this says it on the
// broker. The entity belongs to the same Home Assistant device as the rest of
// hms-cpap's sensors (identifiers: the device id), like the Mule & Miner's
// live sensors (mule/LivePublisher.h). No MQTT here: it is given a publish
// function, so the unit suite reads what would go out.
//
// After a restart nothing here knows the mask, and the broker still holds
// whatever was said last, which can be an ON from a night that ended while
// hms-cpap was away. A night still running writes therapy within a minute
// and says ON again; a broker that hears no mask at all for `settle` after
// the start is told OFF, the same five quiet minutes LiveNight takes a mask
// off after. It rides the Sleep Bridge's own tick; there is no timer here.
//
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <string>

namespace hms_cpap::sleepbridge {

class LiveMask {
public:
    using Clock = std::chrono::system_clock;
    /// Sends one message; false when the broker is not there.
    using Publish = std::function<bool(const std::string& topic, const std::string& payload, int qos,
                                       bool retain)>;

    LiveMask(Publish publish, std::string device_id, std::string device_name,
             Clock::time_point started = Clock::now(),
             std::chrono::seconds settle = std::chrono::seconds(300));

    /// The Home Assistant discovery config, retained. At start.
    bool announce();
    /// A bridge's live night begins on this run: the first time a bridge is
    /// seen, the discovery is said again (a broker that was away at start
    /// gets it then). Later calls for the same bridge do nothing.
    void sourceSeen(const std::string& sender);
    /// A bridge's mask went on or off.
    void set(const std::string& sender, bool on);
    /// Time passing: after a restart, OFF once nothing said the mask for
    /// `settle`.
    void tick(Clock::time_point now = Clock::now());

    std::string stateTopic() const;
    std::string discoveryTopic() const;

private:
    bool send(const std::string& topic, const std::string& payload);

    Publish                 publish_;
    std::string             device_id_, device_name_;
    Clock::time_point       started_;
    std::chrono::seconds    settle_;
    std::mutex              mu_;
    bool                    known_{false};   // the broker holds a mask this run said
    std::set<std::string>   seen_;
};

}  // namespace hms_cpap::sleepbridge
