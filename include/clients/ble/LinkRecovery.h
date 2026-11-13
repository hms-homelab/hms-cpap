#pragma once

#include "clients/ble/BleTransport.h"

#include <string>
#include <vector>

namespace hms_cpap::ble {

/**
 * #67: what to do when a machine the scan can see will not give a working
 * link. BlueZ can hold a device object whose cached GATT no longer matches
 * the machine ("services not resolved in time" on every try) until the host
 * reboots; another scanner on the same adapter (Home Assistant) keeps the
 * object's RSSI fresh, so it always looks like a live advert and is reused.
 *
 * Strictly event driven: every step is a reaction to an observed failure,
 * never a timer.
 *
 *   failure 1                 nothing yet (BlueZ often aborts one connect)
 *   failure 2                 Forget: drop the OS's object, find the machine
 *                             again by a fresh advert
 *   the next failure          ResetAdapter: power the adapter off and on,
 *                             once per streak, then find it again
 *   the next failure          NotRecovering: the page says so, and offers
 *                             "Reset Bluetooth"
 *   any success               the streak is over, everything starts again
 *
 * Only Connect, Resolve and NoCharacteristics count (countsTowardRecovery):
 * a machine that is not found, a radio that is off, or no adapter at all
 * never reach a connect, so they never escalate.
 *
 * Plain C++ with no Bluetooth, so the whole ladder is unit-tested.
 */

/// The link failures a stale OS object explains.
bool countsTowardRecovery(LinkError e);

enum class RecoveryStep {
    None,            // keep going as before
    Forget,          // drop the OS's cached object, rediscover by advert
    ResetAdapter,    // power-cycle the adapter, then rediscover
    NotRecovering,   // nothing more to try automatically
};

const char* recoveryStepText(RecoveryStep s);

class LinkRecovery {
public:
    static constexpr int kForgetAfter = 2;

    /// A counted link failure while the machine was visible.
    RecoveryStep onFailure();
    /// A link came up: the streak is over.
    void onSuccess();
    /// ResetAdapter was asked for but not done (something in hms-cpap was
    /// mid-transfer): the next failure asks for it again.
    void resetDeferred();
    /// The person pressed "Reset Bluetooth": the forget and the reset are
    /// done by hand, and a new streak starts from nothing.
    void restart();

    int failures() const { return failures_; }
    bool forgot() const { return forgot_; }
    bool adapterReset() const { return reset_; }
    bool notRecovering() const { return stuck_; }

private:
    int failures_ = 0;
    bool forgot_ = false;
    bool reset_ = false;
    bool stuck_ = false;
};

/**
 * Process-wide: something in hms-cpap is in the middle of a transfer an
 * adapter power-cycle would cut (an AirSense 11 night streaming, an O2 Ring
 * transfer). Held for as long as it lasts; the adapter is never reset while
 * one is held.
 */
class RadioInUse {
public:
    explicit RadioInUse(std::string what);
    ~RadioInUse();
    RadioInUse(const RadioInUse&) = delete;
    RadioInUse& operator=(const RadioInUse&) = delete;

private:
    std::string what_;
};

/// What holds the radio now, one entry per RadioInUse alive.
std::vector<std::string> radioInUse();

} // namespace hms_cpap::ble
