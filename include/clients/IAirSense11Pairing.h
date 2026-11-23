#pragma once

#include "clients/As11Session.h"

#include <string>
#include <vector>

namespace hms_cpap {

/**
 * Pairing hms-cpap with an AirSense 11 over its own Bluetooth (SDD-057): two
 * steps, because the machine shows its code only once the key exchange has
 * started. AirSense11BleClient implements it in a Bluetooth build; the web
 * layer sees only this, so it builds and is tested everywhere.
 */
class IAirSense11Pairing {
public:
    virtual ~IAirSense11Pairing() = default;

    /// Finds and connects to the machine and starts the key exchange.
    /// WaitCode: the machine's screen shows its code now.
    virtual as11::Result pairStart() = 0;
    /// The code read off the machine. Ok: paired, and the pairing is saved.
    virtual as11::Result pairConfirm(const std::string& code) = 0;
    virtual void pairCancel() = 0;
    virtual bool pairPending() const = 0;
    virtual bool paired() const = 0;
    /// Why the last step failed, in English, for the log. Holds no secret.
    virtual std::string lastError() const = 0;

    /// What the link to the machine is doing now, for the Devices page.
    /// last_contact: epoch seconds of the last thing heard, 0 never.
    struct Presence {
        bool      connected = false;
        bool      therapy = false;     // the machine said it is in therapy
        bool      streaming = false;   // its night is streaming here
        long long last_contact = 0;
        /// #67: the machine is in sight but will not link, and forgetting
        /// it and resetting the adapter did not help ("Bluetooth adapter
        /// not recovering", not "Paired, not connected"). Clears on the
        /// next link that comes up, or on "Reset Bluetooth".
        bool      bluetooth_stuck = false;
    };
    virtual Presence presence() const { return {}; }

    /// Issue #108: the machine the pairing belongs to, and the other ResMed
    /// machines the scans have seen, for Settings.
    struct Machines {
        std::string address;   // the saved machine's address ("" none yet)
        std::string serial;    // its serial ("" until its first session)
        /// The saved machine itself said it no longer knows this pairing: it
        /// is not asked again until it is paired again (or the pairing is
        /// removed, or hms-cpap restarts).
        bool pairing_lost = false;
        struct Other {
            std::string name;      // as it advertises: "ResMed <serial tail>"
            std::string address;
            long long last_seen = 0;   // epoch s
            /// It said it does not know this pairing: not connected to again
            /// in this run.
            bool refused = false;
        };
        std::vector<Other> others;
    };
    virtual Machines machines() const { return {}; }
    /// Something is talking to the machine now (a pairing, a pull): the
    /// pairing is not removed under it.
    virtual bool busy() const { return pairPending(); }

    /// SDD-071: "Record each night for support" (airsense11_ble.capture),
    /// set live when Settings change it. Applies from the next stream.
    virtual void setCapture(bool /*on*/) {}

    /// #67: the pairing was removed here: drop the OS's object for the
    /// machine too, so nothing it cached outlives the pairing.
    virtual void forgetMachine() {}

    /// #67: "Reset Bluetooth" on the Devices page. Forgets the OS's object
    /// for the machine and power-cycles the adapter, now.
    struct BluetoothReset {
        enum class State { Done, Busy, Failed };
        State state = State::Failed;
        bool forgot = false;          // an OS object for the machine was dropped
        bool adapter_reset = false;   // the adapter was powered off and on again
        std::vector<std::string> others_connected;   // other devices that lost their link
        std::string detail;           // why not, when Busy or Failed (English, for the log)
        /// Why not, as a stable code the page translates: Busy is
        /// "streaming", "pairing_open" or "o2ring_transfer"; Failed is
        /// "no_adapter" or "reset_failed".
        std::string reason;
    };
    virtual BluetoothReset resetBluetooth() {
        return {BluetoothReset::State::Failed, false, false, {}, "not supported here", "reset_failed"};
    }
};

} // namespace hms_cpap
