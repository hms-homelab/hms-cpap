#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::ble {

/**
 * The host's Bluetooth LE, as the two direct clients (AirSense11BleClient and
 * O2RingBleClient) need it: is the radio usable, what is around, connect to
 * one device's write and notify characteristics, write, hear notifications,
 * let go. Nothing above this line knows which OS it runs on.
 *
 * Three backends implement it, one per OS, chosen at build time:
 *   - BlueZ over D-Bus (Linux), src/clients/ble/BlueZTransport.cpp
 *   - CoreBluetooth (macOS), src/clients/ble/CoreBluetoothTransport.mm
 *   - WinRT (Windows 10/11), src/clients/ble/WinRtBleTransport.cpp
 * and the test binary drives the clients with a fake.
 *
 * Threads: notify and disconnected callbacks arrive on the backend's own
 * thread (the D-Bus loop, a dispatch queue, the WinRT thread pool), as BlueZ
 * signals always have. A callback must not block on the caller of connect().
 */

/// "FD56", "0xfd56", "0000FD56-0000-1000-8000-00805F9B34FB" and
/// "{0000fd56-...}" all become "0000fd56-0000-1000-8000-00805f9b34fb". Every
/// backend hands UUIDs up in this form, which is the one BlueZ uses.
std::string normalizeUuid(const std::string& uuid);

std::string lower(std::string s);

/// One device, as a scan or the OS's cache sees it.
struct Device {
    std::string id;       // the backend's handle: BlueZ object path, CoreBluetooth identifier, WinRT address
    std::string address;  // the MAC on Linux and Windows; the CoreBluetooth identifier on macOS (no MAC there)
    std::string name;
    std::vector<std::string> uuids;   // advertised services, normalizeUuid form
    std::optional<int> rssi;
    bool advertising = false;         // seen advertising in this discovery, not only remembered
};

enum class Radio {
    Ready,         // usable now
    Off,           // switched off by the user (macOS, Windows: never switched on from here)
    RfKilled,      // Linux: held off by rf-kill
    Unauthorized,  // macOS: Bluetooth permission denied to this app
    NoAdapter,     // no Bluetooth adapter on this computer
    Failed,        // anything else; detail says what
};

struct RadioStatus {
    Radio state = Radio::Failed;
    std::string detail;    // for the log and lastError(): the OS's own advice when there is one
    std::string adapter;   // the adapter's name or path, when there is one
    bool switched_on = false;   // this call switched the radio on
};

/// Switching a radio that is off on, where the OS lets this process: Linux
/// (BlueZ, BlueZAdapter.h) and Windows. macOS has no public way for an
/// unbundled process, so it keeps the advice. Every callback may be missing;
/// a missing one reads as "cannot".
struct RadioSwitchOps {
    std::function<Radio()> state;                 // as it reads now
    std::function<std::string()> request_access;  // "" allowed, else why not
    std::function<std::string()> switch_on;       // "" done, else why not
};

/// Off: ask for access, switch it on, read it again. Anything other than Off
/// is returned as it is. `advice` is the detail when it stays off.
RadioStatus switchOn(const RadioSwitchOps& ops, const std::string& advice);

/// Why connect() gave no link.
enum class LinkError { None, Connect, Resolve, NoCharacteristics, Notify };

struct WriteResult {
    bool ok = false;
    bool timed_out = false;   // the device never acknowledged: the link is as good as dead
    std::string error;
};

/// One connected device's two characteristics.
class ILink {
public:
    virtual ~ILink() = default;
    /// One ATT write; the caller splits to mtu() - 3. With response waits for
    /// the device's acknowledgement (bounded by the backend's call timeout).
    virtual WriteResult write(const std::vector<uint8_t>& piece, bool with_response) = 0;
    /// The link's ATT MTU, 0 when the OS does not say.
    virtual uint16_t mtu() const = 0;
    virtual bool connected() const = 0;
    /// Notifications off, link down. Safe to call twice.
    virtual void close() = 0;
};

struct GattSpec {
    std::string write_uuid;    // normalizeUuid form
    std::string notify_uuid;
};

struct LinkHandlers {
    /// The characteristics were found, notifications are about to be turned on.
    std::function<void()> ready;
    std::function<void(const std::vector<uint8_t>&)> notify;
    std::function<void()> disconnected;
};

struct ConnectOptions {
    int resolve_timeout_ms = 15000;   // connected to services resolved
    int resolve_poll_ms = 250;
};

struct ConnectResult {
    std::unique_ptr<ILink> link;
    LinkError error = LinkError::None;
    std::string detail;
};

struct DiscoverResult {
    std::optional<Device> device;
    bool started = true;      // false: the scan could not start (error says why)
    std::string error;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    /// "bluez", "corebluetooth", "winrt" (or "fake").
    virtual const char* backend() const = 0;
    /// An adapter was there when this transport was made.
    virtual bool available() const = 0;
    /// The radio's state. make_ready: on Linux power the adapter on and lift
    /// a soft rf-kill when possible; macOS and Windows never switch it on.
    virtual RadioStatus radio(bool make_ready) = 0;
    /// What the OS (BlueZ) or this process already knows, without a scan.
    virtual std::vector<Device> known() = 0;
    /// Scans until match() accepts a device or timeout_ms passes. Every
    /// device the scan sees is offered, the known ones included on BlueZ.
    virtual DiscoverResult discover(const std::function<bool(const Device&)>& match, int timeout_ms) = 0;
    virtual ConnectResult connect(const Device& device, const GattSpec& spec, LinkHandlers handlers,
                                  const ConnectOptions& options) = 0;
    /// Drops the OS's cached object for a device, so the next scan sees it as
    /// it advertises now. False when there is nothing to drop.
    virtual bool forget(const Device&) { return false; }
};

/// This build has a Bluetooth backend (BUILD_WITH_BLE).
bool compiled();
/// The backend this build has, "" when none.
const char* backendName();

/// The host's transport, or nullptr in a build without Bluetooth. Made only
/// when a Bluetooth feature is on: on macOS the first one asks the user for
/// Bluetooth permission. `tag` prefixes its log lines.
std::shared_ptr<ITransport> makeHostTransport(const std::string& tag);

/// For Settings' check: compiled, backend, radio state, without switching
/// anything on.
struct Probe {
    bool compiled = false;
    std::string backend;
    RadioStatus radio;
};
Probe probe();

/// `hms_cpap --ble-scan [seconds]`: every device that advertises within the
/// time, printed one per line with what it looks like. Never connects.
int runScan(int seconds);

} // namespace hms_cpap::ble
