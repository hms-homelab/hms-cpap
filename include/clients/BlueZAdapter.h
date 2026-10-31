#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#ifdef WITH_BLE
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace hms_cpap::bluez {

/**
 * The host's Bluetooth adapter, made ready before a scan or a connect. Both
 * BlueZ clients (the AirSense 11 and the O2 Ring) go through here, so neither
 * asks BlueZ to discover on an adapter that is switched off: BlueZ answers
 * that with org.bluez.Error.NotReady, which used to read as "the device is
 * out of range" when the real problem was the computer's own radio.
 *
 * What it does, in order:
 *   1. reads org.bluez.Adapter1 Powered; on, nothing else happens;
 *   2. off: sets Powered = true (what `bluetoothctl power on` does);
 *   3. if BlueZ refuses and a bluetooth rfkill switch is soft-blocked, lifts
 *      the block by writing 0 to its sysfs `soft` file when that is writable,
 *      and powers on again;
 *   4. otherwise logs one line that names rf-kill and the command that lifts
 *      it, and reports RfKilled so the caller can say so distinctly.
 *
 * The decision is plain C++ over a few callbacks (AdapterOps), so it is
 * unit-tested without BlueZ; the sdbus binding is only in a Bluetooth build.
 */

/// Every synchronous call on the link carries one of these. Without one a
/// call waits on sd-bus's default, and a GATT write the device never answers
/// held a summary pull for minutes with nothing in the log.
constexpr std::chrono::seconds kCallTimeout{5};      // GATT, discovery, properties
constexpr std::chrono::seconds kConnectTimeout{10};  // Device1.Connect: the radio needs longer

enum class Power {
    Ready,       // was already on, or is on now
    RfKilled,    // off, and rf-kill holds it off (soft block that could not be lifted)
    Failed,      // off, and BlueZ would not power it for another reason
};

struct Outcome {
    Power power = Power::Failed;
    bool powered_on = false;     // this call switched the adapter on
    bool rfkill_lifted = false;  // this call wrote 0 to a soft block
    std::string detail;          // for the log: the error, or the rf-kill advice
};

/// The adapter and the host, as the decision sees them. Every callback may
/// be missing in a test; a missing one reads as "cannot".
struct AdapterOps {
    /// Powered, or nullopt when the property could not be read.
    std::function<std::optional<bool>()> powered;
    /// Sets Powered = true. Empty on success, else the error text.
    std::function<std::string()> power_on;
    /// The sysfs `soft` files of bluetooth rfkill switches that read 1.
    std::function<std::vector<std::string>()> soft_blocked;
    /// Writes 0 to one such file. False when it is not writable.
    std::function<bool(const std::string&)> unblock;
};

/// Makes the adapter ready as described above.
Outcome ensurePowered(const AdapterOps& ops);

/// The one log line for a radio held off by rf-kill, naming the switch and
/// the command that lifts it.
std::string rfkillAdvice();

/// `<sysfs_root>/rfkill*/soft` for every switch whose `type` is bluetooth and
/// whose `soft` reads 1. The default root is the live one.
std::vector<std::string> softBlockedBluetooth(const std::string& sysfs_root = "/sys/class/rfkill");

/// Writes 0 to a `soft` file. False when it cannot be written (not root, or
/// the file is gone).
bool liftSoftBlock(const std::string& soft_file);

/// The AdapterOps of the live host: BlueZ over D-Bus for the adapter at
/// `adapter_path`, sysfs for rf-kill.
#ifdef WITH_BLE
AdapterOps hostOps(sdbus::IConnection& conn, const std::string& adapter_path);

/// ensurePowered over hostOps, logging what it did under `tag`.
Outcome ensurePowered(sdbus::IConnection& conn, const std::string& adapter_path, const char* tag);

/// True when a call failed because its timeout ran out, not because the
/// other side refused it.
bool timedOut(const sdbus::Error& e);

/// Asks the adapter to forget a device object (org.bluez.Adapter1
/// RemoveDevice), so the next scan sees the device as it advertises now and
/// not as BlueZ last cached it. False when BlueZ refused or timed out.
bool forgetDevice(sdbus::IConnection& conn, const std::string& adapter_path, const std::string& device_path);
#endif

} // namespace hms_cpap::bluez
