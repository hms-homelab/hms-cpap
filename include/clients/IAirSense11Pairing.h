#pragma once

#include "clients/As11Session.h"

#include <string>

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
};

} // namespace hms_cpap
