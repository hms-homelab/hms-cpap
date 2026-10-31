#pragma once
//
// The Mule & Miner's night, live, on the house MQTT broker: each of the
// machine's respiratory events the moment its state frame arrives, and
// therapy on and off, so a Home Assistant automation can react during the
// night (docs/MULE_LOCAL_STREAM.md, "Live on MQTT"). No MQTT here: it is
// given a publish function, so the unit suite reads what would go out.
//
//   cpap/<device_id>/live/event            one JSON per event, not retained
//   cpap/<device_id>/live/last_event       the event's type (OA, CA, UA, H, RERA, CSR), retained
//   cpap/<device_id>/live/event_count      events scored this night (apneas and hypopneas), retained
//   cpap/<device_id>/live/ahi              events so far per hour masked on, retained
//   cpap/<device_id>/live/therapy          ON while the machine streams, OFF when it stops, retained
//   cpap/<device_id>/live/therapy_changed  one JSON when therapy starts or ends, not retained
//
// Home Assistant discovers them as sensors of the same device the rest of
// hms-cpap's sensors belong to (identifiers: the device id).
//
#include "clients/As11Stream.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace hms_cpap::mule {

/// Sends one message; false when the broker is not there.
using Publish = std::function<bool(const std::string& topic, const std::string& payload, int qos,
                                   bool retain)>;

/// The card's short name for one of the machine's event names: OA, CA, UA,
/// H, RERA, CSR; the name itself when it is none of those.
std::string eventType(const std::string& machine_event);

class LivePublisher {
public:
    LivePublisher(Publish publish, std::string device_id, std::string device_name);

    /// The Home Assistant discovery configs, retained. Once at start.
    bool announce();

    /// Therapy started (`on`) or ended on the machine behind `unit`. Nights
    /// are kept per unit (one unit streams one machine); `machine` is the
    /// machine's serial when a block has named it, else "".
    void therapy(const std::string& unit, const std::string& machine, bool on);
    /// A block landed: where the night stands on the machine's clock.
    void block(const std::string& unit, int64_t start_ms, int64_t end_ms);
    /// One of the machine's events, as its state frame carried it.
    void event(const std::string& unit, const std::string& machine, const as11::RespEvent& e);

    struct Night {
        bool    on{false};
        int64_t start_ms{0}, end_ms{0};   // the machine's clock
        int     apneas{0}, hypopneas{0}, reras{0}, csr{0};
        int     events() const { return apneas + hypopneas; }
        double  hours() const;
        double  ahi() const;
    };
    Night night(const std::string& unit) const;

private:
    void send(const std::string& leaf, const std::string& payload, bool retain);
    std::string topic(const std::string& leaf) const;

    Publish     publish_;
    std::string device_id_, device_name_;
    mutable std::mutex mu_;
    std::map<std::string, Night> nights_;
};

}  // namespace hms_cpap::mule
