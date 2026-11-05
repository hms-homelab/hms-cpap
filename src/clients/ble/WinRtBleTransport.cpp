// The Windows backend: WinRT Bluetooth LE (Windows 10 1803 and later), through
// C++/WinRT from the Windows SDK. An unpackaged desktop process may use all of
// it. Kept to this one file, with nothing of the web server's included, so
// the WinRT headers meet no other headers' macros.
//
// No OS pairing: the AirSense 11 pairs at the application level (the code on
// its screen) and the rings ask for no link security, so this never calls the
// pairing API and Windows never shows a pairing prompt.
#if defined(HMS_BLE_WINRT)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include "clients/ble/BleTransport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace hms_cpap::ble {

namespace {

namespace wf = winrt::Windows::Foundation;
namespace wb = winrt::Windows::Devices::Bluetooth;
namespace wa = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace wg = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace wr = winrt::Windows::Devices::Radios;
namespace ws = winrt::Windows::Storage::Streams;

constexpr auto kCallWait = std::chrono::seconds(5);      // a write, notifications on
constexpr auto kConnectWait = std::chrono::seconds(10);  // the device object

/// Every thread that calls WinRT joins the multi-threaded apartment once. A
/// thread that is already in one (of either kind) is left as it is.
void apartment() {
    thread_local bool done = false;
    if (done) return;
    done = true;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {}
}

std::string text(const winrt::hstring& s) { return winrt::to_string(s); }

std::string why(const winrt::hresult_error& e) { return winrt::to_string(e.message()); }

std::string macOf(uint64_t a) {
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", unsigned((a >> 40) & 0xFF),
                  unsigned((a >> 32) & 0xFF), unsigned((a >> 24) & 0xFF), unsigned((a >> 16) & 0xFF),
                  unsigned((a >> 8) & 0xFF), unsigned(a & 0xFF));
    return buf;
}

std::optional<uint64_t> addressOf(const std::string& mac) {
    std::string hex;
    for (char c : mac)
        if (c != ':' && c != '-') hex += c;
    if (hex.empty() || hex.size() > 12) return std::nullopt;
    try {
        return std::stoull(hex, nullptr, 16);
    } catch (...) {
        return std::nullopt;
    }
}

std::string uuidOf(const winrt::guid& g) { return normalizeUuid(text(winrt::to_hstring(g))); }

/// An async operation's result within `limit`, or nullopt when it did not
/// complete in time (it is cancelled) or failed.
template <typename Op>
auto within(Op op, std::chrono::milliseconds limit) -> std::optional<decltype(op.GetResults())> {
    const auto status = op.wait_for(std::chrono::duration_cast<wf::TimeSpan>(limit));
    if (status != wf::AsyncStatus::Completed) {
        try { op.Cancel(); } catch (...) {}
        return std::nullopt;
    }
    return op.GetResults();
}

// What the link's event handlers share with it.
struct LinkState {
    std::mutex m;
    LinkHandlers handlers;
    std::atomic<bool> up{true};
};

class WinRtLink : public ILink {
public:
    WinRtLink(wb::BluetoothLEDevice device, wg::GattSession session, std::vector<wg::GattDeviceService> services,
              wg::GattCharacteristic write, wg::GattCharacteristic notify, winrt::event_token value_token,
              winrt::event_token status_token, std::shared_ptr<LinkState> state)
        : device_(std::move(device)), session_(std::move(session)), services_(std::move(services)),
          write_(std::move(write)), notify_(std::move(notify)), value_token_(value_token),
          status_token_(status_token), state_(std::move(state)) {}
    ~WinRtLink() override { close(); }

    WriteResult write(const std::vector<uint8_t>& piece, bool with_response) override {
        apartment();
        WriteResult r;
        if (!write_ || !state_->up) {
            r.error = "the link is closed";
            return r;
        }
        try {
            ws::Buffer buf(static_cast<uint32_t>(piece.size()));
            if (!piece.empty()) std::memcpy(buf.data(), piece.data(), piece.size());
            buf.Length(static_cast<uint32_t>(piece.size()));
            const auto opt = with_response ? wg::GattWriteOption::WriteWithResponse
                                           : wg::GattWriteOption::WriteWithoutResponse;
            auto res = within(write_.WriteValueWithResultAsync(buf, opt), kCallWait);
            if (!res) {
                r.timed_out = true;
                r.error = "no acknowledgement";
                return r;
            }
            if (res->Status() != wg::GattCommunicationStatus::Success) {
                r.error = "write refused (status " + std::to_string(static_cast<int>(res->Status())) + ")";
                return r;
            }
            r.ok = true;
        } catch (const winrt::hresult_error& e) {
            r.error = why(e);
        }
        return r;
    }

    uint16_t mtu() const override {
        try {
            return session_ ? session_.MaxPduSize() : 0;
        } catch (...) {
            return 0;
        }
    }

    bool connected() const override { return state_->up; }

    void close() override {
        if (closed_) return;
        closed_ = true;
        apartment();
        state_->up = false;
        {
            std::lock_guard<std::mutex> lk(state_->m);
            state_->handlers = {};
        }
        try {
            if (notify_) {
                notify_.ValueChanged(value_token_);
                within(notify_.WriteClientCharacteristicConfigurationDescriptorAsync(
                           wg::GattClientCharacteristicConfigurationDescriptorValue::None),
                       std::chrono::milliseconds(1500));
            }
        } catch (...) {}
        try {
            if (device_) device_.ConnectionStatusChanged(status_token_);
        } catch (...) {}
        // Windows holds the link while any of these is open.
        for (auto& s : services_) {
            try { s.Close(); } catch (...) {}
        }
        services_.clear();
        try {
            if (session_) {
                session_.MaintainConnection(false);
                session_.Close();
            }
        } catch (...) {}
        try {
            if (device_) device_.Close();
        } catch (...) {}
        write_ = nullptr;
        notify_ = nullptr;
        session_ = nullptr;
        device_ = nullptr;
    }

private:
    wb::BluetoothLEDevice device_{nullptr};
    wg::GattSession session_{nullptr};
    std::vector<wg::GattDeviceService> services_;
    wg::GattCharacteristic write_{nullptr};
    wg::GattCharacteristic notify_{nullptr};
    winrt::event_token value_token_{};
    winrt::event_token status_token_{};
    std::shared_ptr<LinkState> state_;
    bool closed_ = false;
};

// What a scan's Received handler shares with discover().
struct ScanState {
    std::mutex m;
    std::condition_variable cv;
    std::map<std::string, Device> devices;
    std::map<std::string, wb::BluetoothAddressType> address_types;
    std::map<std::string, uint64_t> device_gen;
    uint64_t gen = 0;
    bool stopped = false;
    std::string stop_error;
};

class WinRtTransport : public ITransport {
public:
    explicit WinRtTransport(std::string tag) : tag_(std::move(tag)) {
        apartment();
        try {
            auto adapter = within(wb::BluetoothAdapter::GetDefaultAsync(), kConnectWait);
            available_ = adapter && *adapter && adapter->IsLowEnergySupported();
        } catch (...) {
            available_ = false;
        }
    }

    const char* backend() const override { return "winrt"; }
    bool available() const override { return available_; }

    RadioStatus radio(bool make_ready) override {
        apartment();
        RadioStatus r;
        try {
            auto adapter = within(wb::BluetoothAdapter::GetDefaultAsync(), kConnectWait);
            if (!adapter || !*adapter) {
                r.state = Radio::NoAdapter;
                r.detail = "no Bluetooth adapter";
            } else if (!adapter->IsLowEnergySupported()) {
                r.state = Radio::NoAdapter;
                r.detail = "this PC's Bluetooth has no LE";
            } else {
                auto radio = within(adapter->GetRadioAsync(), kConnectWait);
                if (radio && *radio) r.adapter = text(radio->Name());
                if (!radio || !*radio) {
                    r.state = Radio::Failed;
                    r.detail = "the Bluetooth radio could not be read";
                } else if (radio->State() == wr::RadioState::On) {
                    r.state = Radio::Ready;
                } else if (radio->State() == wr::RadioState::Off || radio->State() == wr::RadioState::Disabled) {
                    const std::string advice =
                        "Bluetooth is switched off on this PC: switch it on in Settings, Bluetooth & "
                        "devices (and check airplane mode), and try again";
                    r.state = Radio::Off;
                    r.detail = advice;
                    if (make_ready) {
                        // Switch it on where Windows lets a desktop process.
                        wr::Radio rad = *radio;
                        RadioSwitchOps ops;
                        ops.state = [rad]() {
                            const auto s = rad.State();
                            return s == wr::RadioState::On ? Radio::Ready
                                   : (s == wr::RadioState::Off || s == wr::RadioState::Disabled) ? Radio::Off
                                                                                                : Radio::Failed;
                        };
                        ops.request_access = []() -> std::string {
                            auto a = within(wr::Radio::RequestAccessAsync(), kConnectWait);
                            if (!a) return "no answer to the access request";
                            return *a == wr::RadioAccessStatus::Allowed
                                       ? std::string()
                                       : "access status " + std::to_string(static_cast<int>(*a));
                        };
                        ops.switch_on = [rad]() -> std::string {
                            auto a = within(rad.SetStateAsync(wr::RadioState::On), kConnectWait);
                            if (!a) return "no answer";
                            if (*a != wr::RadioAccessStatus::Allowed)
                                return "access status " + std::to_string(static_cast<int>(*a));
                            // The state follows a moment later.
                            for (int i = 0; i < 50 && rad.State() != wr::RadioState::On; i++)
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            return std::string();
                        };
                        const auto adapter_name = r.adapter;
                        r = switchOn(ops, advice);
                        r.adapter = adapter_name;
                        if (r.switched_on) std::cout << tag_ << "Bluetooth was off: switched it on" << std::endl;
                    }
                } else {
                    r.state = Radio::Failed;
                    r.detail = "the Bluetooth radio's state is unknown";
                }
            }
        } catch (const winrt::hresult_error& e) {
            r.state = Radio::Failed;
            r.detail = why(e);
        }
        if (make_ready && r.state != Radio::Ready) std::cerr << tag_ << r.detail << std::endl;
        return r;
    }

    // Nothing is offered from memory: every connect starts from a fresh scan
    // that finds the device by its advert, and opens it with that advert's
    // address and address type. A remembered address alone (the type unknown,
    // the device gone or moved to another identity) is what made a ring
    // unreachable.
    std::vector<Device> known() override { return {}; }

    DiscoverResult discover(const std::function<bool(const Device&)>& match, int timeout_ms) override {
        apartment();
        DiscoverResult res;
        auto st = std::make_shared<ScanState>();
        wa::BluetoothLEAdvertisementWatcher watcher{nullptr};
        winrt::event_token received{}, stopped{};
        try {
            watcher = wa::BluetoothLEAdvertisementWatcher();
            // Active: the scan response carries the name more often than the advert.
            watcher.ScanningMode(wa::BluetoothLEScanningMode::Active);
            received = watcher.Received([st](const wa::BluetoothLEAdvertisementWatcher&,
                                             const wa::BluetoothLEAdvertisementReceivedEventArgs& a) {
                try {
                    const std::string id = macOf(a.BluetoothAddress());
                    const std::string name = text(a.Advertisement().LocalName());
                    std::vector<std::string> uuids;
                    for (const auto& g : a.Advertisement().ServiceUuids()) uuids.push_back(uuidOf(g));
                    std::lock_guard<std::mutex> lk(st->m);
                    Device& d = st->devices[id];
                    d.id = id;
                    d.address = id;
                    if (!name.empty()) d.name = name;
                    for (const auto& u : uuids)
                        if (std::find(d.uuids.begin(), d.uuids.end(), u) == d.uuids.end()) d.uuids.push_back(u);
                    d.rssi = a.RawSignalStrengthInDBm();
                    st->address_types[id] = a.BluetoothAddressType();
                    st->device_gen[id] = ++st->gen;
                    st->cv.notify_all();
                } catch (...) {}
            });
            stopped = watcher.Stopped([st](const wa::BluetoothLEAdvertisementWatcher&,
                                           const wa::BluetoothLEAdvertisementWatcherStoppedEventArgs& a) {
                std::lock_guard<std::mutex> lk(st->m);
                st->stopped = true;
                if (a.Error() != wb::BluetoothError::Success)
                    st->stop_error = "the scan stopped (error " + std::to_string(static_cast<int>(a.Error())) + ")";
                st->cv.notify_all();
            });
            watcher.Start();
        } catch (const winrt::hresult_error& e) {
            res.started = false;
            res.error = why(e);
            return res;
        }

        // The match runs here, on the caller's thread, as it does on BlueZ.
        named_.clear();
        uint64_t since = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!res.device && std::chrono::steady_clock::now() < deadline) {
            std::vector<Device> fresh;
            {
                std::unique_lock<std::mutex> lk(st->m);
                st->cv.wait_for(lk, std::chrono::milliseconds(250));
                if (st->stopped) {
                    if (!st->stop_error.empty() && st->devices.empty()) {
                        res.started = false;
                        res.error = st->stop_error;
                    }
                    break;
                }
                for (const auto& [id, g] : st->device_gen)
                    if (g > since) fresh.push_back(st->devices[id]);
                since = st->gen;
                for (const auto& d : fresh) {
                    auto t = st->address_types.find(d.id);
                    if (t != st->address_types.end()) rememberType(d.id, t->second);
                }
            }
            for (auto& d : fresh) {
                d.advertising = true;
                // A name that never came in the advert (the scan response did
                // not arrive): Windows usually knows it. Asked once per scan,
                // and only for a device that advertises a service.
                if (d.name.empty() && !d.uuids.empty() && named_.insert(d.id).second) {
                    d.name = cachedName(d.id);
                    if (!d.name.empty()) {
                        std::lock_guard<std::mutex> lk(st->m);
                        st->devices[d.id].name = d.name;
                    }
                }
                if (match(d)) {
                    res.device = d;
                    break;
                }
            }
        }
        try {
            watcher.Stop();
            watcher.Received(received);
            watcher.Stopped(stopped);
        } catch (...) {}
        return res;
    }

    ConnectResult connect(const Device& device, const GattSpec& spec, LinkHandlers handlers,
                          const ConnectOptions& options) override {
        apartment();
        ConnectResult res;
        res.error = LinkError::Connect;
        const auto addr = addressOf(device.id);
        if (!addr) {
            res.detail = "not a Bluetooth address: " + device.id;
            return res;
        }
        auto state = std::make_shared<LinkState>();
        state->handlers = std::move(handlers);
        try {
            // The address type the device advertised with; a random one opened
            // as public is a device Windows can never reach. When the scan did
            // not say, the address's top two bits do (11: random static).
            const auto type = typeOf(device.id).value_or(((*addr >> 46) & 0x3) == 0x3
                                                             ? wb::BluetoothAddressType::Random
                                                             : wb::BluetoothAddressType::Public);
            auto dev = within(wb::BluetoothLEDevice::FromBluetoothAddressAsync(*addr, type), kConnectWait);
            if (!dev || !*dev) {
                res.detail = "the device could not be opened";
                std::cerr << tag_ << "connect failed: " << res.detail << std::endl;
                return res;
            }
            wb::BluetoothLEDevice ble = *dev;
            // Holds the link between calls, and says the link's MTU. With
            // MaintainConnection Windows starts connecting now; GATT calls made
            // before the session is active can come back Unreachable.
            wg::GattSession session{nullptr};
            if (auto s = within(wg::GattSession::FromDeviceIdAsync(ble.BluetoothDeviceId()), kConnectWait); s && *s) {
                session = *s;
                session.MaintainConnection(true);
                const auto until = std::chrono::steady_clock::now() + kConnectWait;
                while (session.SessionStatus() != wg::GattSessionStatus::Active &&
                       std::chrono::steady_clock::now() < until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (session.SessionStatus() != wg::GattSessionStatus::Active)
                    std::cerr << tag_ << "the link did not come up within "
                              << std::chrono::duration_cast<std::chrono::seconds>(kConnectWait).count()
                              << " s; reading services anyway" << std::endl;
            }
            auto fail = [&](LinkError e, const std::string& detail, std::vector<wg::GattDeviceService>* svcs) {
                if (svcs)
                    for (auto& s : *svcs) {
                        try { s.Close(); } catch (...) {}
                    }
                try {
                    if (session) {
                        session.MaintainConnection(false);
                        session.Close();
                    }
                    ble.Close();
                } catch (...) {}
                res.error = e;
                res.detail = detail;
                return std::move(res);
            };

            // Uncached: what the device offers now, which is also what makes
            // Windows actually connect.
            // Unreachable while the link is still coming up is retried, within
            // the resolve timeout, as BlueZ's connect is retried.
            const auto resolve_by =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(options.resolve_timeout_ms);
            std::optional<wg::GattDeviceServicesResult> services;
            for (int attempt = 1;; attempt++) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    resolve_by - std::chrono::steady_clock::now());
                if (left.count() <= 0) break;
                services = within(ble.GetGattServicesAsync(wb::BluetoothCacheMode::Uncached), left);
                if (!services || services->Status() != wg::GattCommunicationStatus::Unreachable) break;
                std::cerr << tag_ << "services unreachable (attempt " << attempt << "), retrying" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
            if (!services) return fail(LinkError::Resolve, "services not resolved in time", nullptr);
            if (services->Status() != wg::GattCommunicationStatus::Success)
                return fail(services->Status() == wg::GattCommunicationStatus::Unreachable ? LinkError::Connect
                                                                                          : LinkError::Resolve,
                            "services not read (status " + std::to_string(static_cast<int>(services->Status())) +
                                ")",
                            nullptr);

            std::vector<wg::GattDeviceService> held;
            wg::GattCharacteristic wc{nullptr}, nc{nullptr};
            for (const auto& svc : services->Services()) {
                held.push_back(svc);
                auto chars = within(svc.GetCharacteristicsAsync(wb::BluetoothCacheMode::Uncached), kCallWait);
                if (!chars || chars->Status() != wg::GattCommunicationStatus::Success) continue;
                for (const auto& c : chars->Characteristics()) {
                    const std::string u = uuidOf(c.Uuid());
                    if (u == spec.write_uuid && !wc) wc = c;
                    else if (u == spec.notify_uuid && !nc) nc = c;
                }
            }
            if (!wc || !nc) return fail(LinkError::NoCharacteristics, "the characteristics were not found", &held);

            const auto value_token = nc.ValueChanged([state](const wg::GattCharacteristic&,
                                                             const wg::GattValueChangedEventArgs& a) {
                std::function<void(const std::vector<uint8_t>&)> cb;
                {
                    std::lock_guard<std::mutex> lk(state->m);
                    cb = state->handlers.notify;
                }
                if (!cb) return;
                try {
                    const auto buf = a.CharacteristicValue();
                    const uint8_t* b = buf.data();
                    cb(std::vector<uint8_t>(b, b + buf.Length()));
                } catch (...) {}
            });
            const auto status_token = ble.ConnectionStatusChanged([state](const wb::BluetoothLEDevice& d,
                                                                          const wf::IInspectable&) {
                if (d.ConnectionStatus() != wb::BluetoothConnectionStatus::Disconnected) return;
                if (!state->up.exchange(false)) return;
                std::function<void()> cb;
                {
                    std::lock_guard<std::mutex> lk(state->m);
                    cb = state->handlers.disconnected;
                }
                if (cb) cb();
            });

            std::function<void()> ready;
            {
                std::lock_guard<std::mutex> lk(state->m);
                ready = state->handlers.ready;
            }
            if (ready) ready();
            auto cccd = within(nc.WriteClientCharacteristicConfigurationDescriptorAsync(
                                   wg::GattClientCharacteristicConfigurationDescriptorValue::Notify),
                               kCallWait);
            if (!cccd || *cccd != wg::GattCommunicationStatus::Success) {
                try {
                    nc.ValueChanged(value_token);
                    ble.ConnectionStatusChanged(status_token);
                } catch (...) {}
                return fail(LinkError::Notify,
                            cccd ? "refused (status " + std::to_string(static_cast<int>(*cccd)) + ")" : "no answer",
                            &held);
            }
            res.error = LinkError::None;
            res.link = std::make_unique<WinRtLink>(ble, session, std::move(held), wc, nc, value_token,
                                                   status_token, state);
            return res;
        } catch (const winrt::hresult_error& e) {
            res.detail = why(e);
            std::cerr << tag_ << "connect failed: " << res.detail << std::endl;
            return res;
        }
    }

private:
    // The name Windows holds for an address (its device cache, filled by the
    // system's own scans), "" when it has none. Opening the device object does
    // not connect to it.
    std::string cachedName(const std::string& id) {
        const auto addr = addressOf(id);
        if (!addr) return {};
        try {
            const auto type = typeOf(id).value_or(((*addr >> 46) & 0x3) == 0x3 ? wb::BluetoothAddressType::Random
                                                                                : wb::BluetoothAddressType::Public);
            auto dev = within(wb::BluetoothLEDevice::FromBluetoothAddressAsync(*addr, type),
                              std::chrono::milliseconds(2000));
            if (!dev || !*dev) return {};
            std::string name = text(dev->Name());
            dev->Close();
            return name;
        } catch (...) {
            return {};
        }
    }

    void rememberType(const std::string& id, wb::BluetoothAddressType t) {
        std::lock_guard<std::mutex> lk(seen_m_);
        types_[id] = t;
    }
    std::optional<wb::BluetoothAddressType> typeOf(const std::string& id) {
        std::lock_guard<std::mutex> lk(seen_m_);
        auto it = types_.find(id);
        if (it == types_.end()) return std::nullopt;
        return it->second;
    }

    std::string tag_;
    bool available_ = false;
    std::mutex seen_m_;
    std::set<std::string> named_;   // asked Windows for a name in this scan
    // The advertised address's type, from the scan that found the device. A ring advertises from a random static
    // address; opened as a public one, Windows answers every GATT call with
    // Unreachable.
    std::map<std::string, wb::BluetoothAddressType> types_;
};

} // namespace

std::shared_ptr<ITransport> makeWinRtTransport(const std::string& tag) {
    return std::make_shared<WinRtTransport>(tag);
}

} // namespace hms_cpap::ble

#endif // HMS_BLE_WINRT
