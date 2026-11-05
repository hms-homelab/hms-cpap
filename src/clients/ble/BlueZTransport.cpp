// The Linux backend: BlueZ over D-Bus (sdbus-c++). What both direct clients
// did with D-Bus before the transport seam, moved here unchanged: the adapter
// made ready (power, rf-kill), a timeout on every call, three connect
// attempts, the wait for ServicesResolved, the characteristics by UUID with
// the MTU BlueZ publishes, and RemoveDevice to forget a stale object.
#if defined(HMS_BLE_BLUEZ)

#include "clients/BlueZAdapter.h"
#include "clients/ble/BleTransport.h"

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>

namespace hms_cpap::ble {

namespace {

const sdbus::ServiceName kBlueZ{"org.bluez"};
const sdbus::ObjectPath kRoot{"/"};
constexpr const char* kObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr const char* kProps = "org.freedesktop.DBus.Properties";
constexpr const char* kAdapter1 = "org.bluez.Adapter1";
constexpr const char* kDevice1 = "org.bluez.Device1";
constexpr const char* kChar1 = "org.bluez.GattCharacteristic1";

using Props = std::map<std::string, sdbus::Variant>;
using Objects = std::map<sdbus::ObjectPath, std::map<std::string, Props>>;

template <typename T>
std::optional<T> prop(const Props& p, const char* key) {
    auto it = p.find(key);
    if (it == p.end()) return std::nullopt;
    try { return it->second.get<T>(); } catch (...) { return std::nullopt; }
}

Device toDevice(const std::string& path, const Props& p) {
    Device d;
    d.id = path;
    d.address = lower(prop<std::string>(p, "Address").value_or(""));
    d.name = prop<std::string>(p, "Name").value_or("");
    for (const auto& u : prop<std::vector<std::string>>(p, "UUIDs").value_or(std::vector<std::string>{}))
        d.uuids.push_back(normalizeUuid(u));
    if (auto rssi = prop<int16_t>(p, "RSSI")) d.rssi = *rssi;
    // BlueZ sets RSSI on an object while its adverts come in.
    d.advertising = d.rssi.has_value();
    return d;
}

class BlueZLink : public ILink {
public:
    BlueZLink(std::unique_ptr<sdbus::IProxy> device, std::unique_ptr<sdbus::IProxy> write,
              std::unique_ptr<sdbus::IProxy> notify, uint16_t mtu, std::shared_ptr<std::atomic<bool>> up)
        : device_(std::move(device)), write_(std::move(write)), notify_(std::move(notify)), mtu_(mtu),
          up_(std::move(up)) {}
    ~BlueZLink() override { close(); }

    WriteResult write(const std::vector<uint8_t>& piece, bool with_response) override {
        WriteResult r;
        if (!write_) {
            r.error = "the link is closed";
            return r;
        }
        std::map<std::string, sdbus::Variant> options;
        options["type"] = sdbus::Variant{std::string(with_response ? "request" : "command")};
        try {
            write_->callMethod("WriteValue")
                .onInterface(kChar1)
                .withTimeout(bluez::kCallTimeout)
                .withArguments(piece, options);
            r.ok = true;
        } catch (const sdbus::Error& e) {
            r.timed_out = bluez::timedOut(e);
            r.error = e.getMessage();
        } catch (const std::exception& e) {
            r.error = e.what();
        }
        return r;
    }

    uint16_t mtu() const override { return mtu_; }
    bool connected() const override { return up_->load(); }

    void close() override {
        if (notify_) {
            try {
                notify_->callMethod("StopNotify").onInterface(kChar1).withTimeout(bluez::kCallTimeout);
            } catch (...) {}
        }
        if (device_) {
            try {
                device_->callMethod("Disconnect").onInterface(kDevice1).withTimeout(bluez::kCallTimeout);
            } catch (...) {}
        }
        up_->store(false);
        notify_.reset();
        write_.reset();
        device_.reset();
    }

private:
    std::unique_ptr<sdbus::IProxy> device_, write_, notify_;
    uint16_t mtu_;
    std::shared_ptr<std::atomic<bool>> up_;
};

class BlueZTransport : public ITransport {
public:
    explicit BlueZTransport(std::string tag) : tag_(std::move(tag)) {
        try {
            conn_ = sdbus::createSystemBusConnection();
        } catch (const std::exception& e) {
            std::cerr << tag_ << "no system D-Bus: " << e.what() << std::endl;
            return;
        }
        adapter_path_ = findAdapter();
        if (adapter_path_.empty()) {
            conn_.reset();
            return;
        }
        conn_->enterEventLoopAsync();
    }
    ~BlueZTransport() override {
        if (conn_) conn_->leaveEventLoop();
        conn_.reset();
    }

    const char* backend() const override { return "bluez"; }
    bool available() const override { return conn_ && !adapter_path_.empty(); }

    RadioStatus radio(bool make_ready) override {
        RadioStatus s;
        s.adapter = adapter_path_;
        if (!available()) {
            s.state = Radio::NoAdapter;
            s.detail = "no Bluetooth adapter";
            return s;
        }
        if (!make_ready) {
            const auto on = bluez::hostOps(*conn_, adapter_path_).powered();
            if (!on) {
                s.state = Radio::Failed;
                s.detail = "the adapter's Powered state could not be read";
            } else if (*on) {
                s.state = Radio::Ready;
            } else {
                s.state = bluez::softBlockedBluetooth().empty() ? Radio::Off : Radio::RfKilled;
                s.detail = s.state == Radio::RfKilled ? bluez::rfkillAdvice() : "the adapter is powered off";
            }
            return s;
        }
        const auto out = bluez::ensurePowered(*conn_, adapter_path_, tag_.c_str());
        s.detail = out.detail;
        s.state = out.power == bluez::Power::Ready      ? Radio::Ready
                  : out.power == bluez::Power::RfKilled ? Radio::RfKilled
                                                        : Radio::Failed;
        return s;
    }

    std::vector<Device> known() override {
        std::vector<Device> out;
        if (!available()) return out;
        try {
            for (const auto& [path, ifaces] : objects()) {
                auto it = ifaces.find(kDevice1);
                if (it != ifaces.end()) out.push_back(toDevice(std::string(path), it->second));
            }
        } catch (...) {}
        return out;
    }

    DiscoverResult discover(const std::function<bool(const Device&)>& match, int timeout_ms) override {
        DiscoverResult res;
        if (!available()) {
            res.started = false;
            res.error = "no Bluetooth adapter";
            return res;
        }
        auto adapter = sdbus::createProxy(*conn_, kBlueZ, sdbus::ObjectPath{adapter_path_});
        try {
            adapter->callMethod("StartDiscovery").onInterface(kAdapter1).withTimeout(bluez::kCallTimeout);
        } catch (const std::exception& e) {
            res.started = false;
            res.error = e.what();
            return res;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!res.device && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            try {
                for (const auto& [path, ifaces] : objects()) {
                    auto it = ifaces.find(kDevice1);
                    if (it == ifaces.end()) continue;
                    Device d = toDevice(std::string(path), it->second);
                    if (match(d)) {
                        res.device = std::move(d);
                        break;
                    }
                }
            } catch (...) {}
        }
        try {
            adapter->callMethod("StopDiscovery").onInterface(kAdapter1).withTimeout(bluez::kCallTimeout);
        } catch (...) {}
        return res;
    }

    ConnectResult connect(const Device& device, const GattSpec& spec, LinkHandlers handlers,
                          const ConnectOptions& options) override {
        ConnectResult res;
        res.error = LinkError::Connect;
        if (!available() || device.id.empty()) {
            res.detail = "no Bluetooth adapter";
            return res;
        }
        auto up = std::make_shared<std::atomic<bool>>(false);
        auto dev = sdbus::createProxy(*conn_, kBlueZ, sdbus::ObjectPath{device.id});
        // The link dropping wakes whatever waits on it.
        dev->uponSignal("PropertiesChanged")
            .onInterface(kProps)
            .call([up, cb = handlers.disconnected](const std::string& iface, const Props& changed,
                                                   const std::vector<std::string>&) {
                if (iface != kDevice1) return;
                auto it = changed.find("Connected");
                if (it == changed.end()) return;
                try {
                    if (!it->second.get<bool>()) {
                        up->store(false);
                        if (cb) cb();
                    }
                } catch (...) {}
            });

        // BlueZ often aborts the first LE connect (le-connection-abort-by-local).
        bool connected = false;
        for (int attempt = 0; attempt < 3 && !connected; attempt++) {
            try {
                dev->callMethod("Connect").onInterface(kDevice1).withTimeout(bluez::kConnectTimeout);
                connected = true;
            } catch (const sdbus::Error& e) {
                std::cerr << tag_ << "connect attempt " << (attempt + 1) << "/3 "
                          << (bluez::timedOut(e) ? "timed out" : "failed: " + e.getMessage()) << std::endl;
                try {
                    dev->callMethod("Disconnect").onInterface(kDevice1).withTimeout(bluez::kCallTimeout);
                } catch (...) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            }
        }
        if (!connected) {
            res.detail = "connect failed";
            return res;
        }
        up->store(true);

        auto disconnect = [&dev] {
            try {
                dev->callMethod("Disconnect").onInterface(kDevice1).withTimeout(bluez::kCallTimeout);
            } catch (...) {}
        };

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(options.resolve_timeout_ms);
        bool resolved = false;
        while (!resolved && std::chrono::steady_clock::now() < deadline) {
            try {
                sdbus::Variant v;
                dev->callMethod("Get")
                    .onInterface(kProps)
                    .withTimeout(bluez::kCallTimeout)
                    .withArguments(std::string(kDevice1), std::string("ServicesResolved"))
                    .storeResultsTo(v);
                resolved = v.get<bool>();
            } catch (...) {}
            if (!resolved) std::this_thread::sleep_for(std::chrono::milliseconds(options.resolve_poll_ms));
        }
        if (!resolved) {
            disconnect();
            res.error = LinkError::Resolve;
            res.detail = "services not resolved in time";
            return res;
        }

        std::string write_path, notify_path;
        uint16_t mtu = 0;
        try {
            for (const auto& [path, ifaces] : objects()) {
                const std::string p(path);
                if (p.rfind(device.id + "/", 0) != 0) continue;
                auto it = ifaces.find(kChar1);
                if (it == ifaces.end()) continue;
                const auto uuid = normalizeUuid(prop<std::string>(it->second, "UUID").value_or(""));
                if (uuid == spec.write_uuid) {
                    write_path = p;
                    // BlueZ 5.62+ publishes the link's ATT MTU on the characteristic.
                    if (auto m = prop<uint16_t>(it->second, "MTU")) mtu = *m;
                } else if (uuid == spec.notify_uuid) {
                    notify_path = p;
                }
            }
        } catch (const std::exception& e) {
            disconnect();
            res.error = LinkError::NoCharacteristics;
            res.detail = std::string("GATT discovery failed: ") + e.what();
            return res;
        }
        if (write_path.empty() || notify_path.empty()) {
            disconnect();
            res.error = LinkError::NoCharacteristics;
            res.detail = "the characteristics were not found";
            return res;
        }

        auto write = sdbus::createProxy(*conn_, kBlueZ, sdbus::ObjectPath{write_path});
        std::unique_ptr<sdbus::IProxy> notify;
        try {
            notify = sdbus::createProxy(*conn_, kBlueZ, sdbus::ObjectPath{notify_path});
            notify->uponSignal("PropertiesChanged")
                .onInterface(kProps)
                .call([cb = handlers.notify](const std::string& iface, const Props& changed,
                                             const std::vector<std::string>&) {
                    if (iface != kChar1 || !cb) return;
                    auto it = changed.find("Value");
                    if (it == changed.end()) return;
                    try {
                        cb(it->second.get<std::vector<uint8_t>>());
                    } catch (...) {}
                });
            // Before the first notification can land.
            if (handlers.ready) handlers.ready();
            notify->callMethod("StartNotify").onInterface(kChar1).withTimeout(bluez::kCallTimeout);
        } catch (const std::exception& e) {
            disconnect();
            res.error = LinkError::Notify;
            res.detail = e.what();
            return res;
        }
        res.error = LinkError::None;
        res.link = std::make_unique<BlueZLink>(std::move(dev), std::move(write), std::move(notify), mtu, up);
        return res;
    }

    bool forget(const Device& device) override {
        if (!available()) return false;
        return bluez::forgetDevice(*conn_, adapter_path_, device.id);
    }

private:
    Objects objects() {
        auto proxy = sdbus::createProxy(*conn_, kBlueZ, kRoot);
        Objects out;
        proxy->callMethod("GetManagedObjects")
            .onInterface(kObjectManager)
            .withTimeout(bluez::kCallTimeout)
            .storeResultsTo(out);
        return out;
    }

    // On the system bus, always. sdbus's default connection is the session
    // bus whenever one exists (a desktop or an SSH login), where BlueZ is not,
    // so the lookup found no adapter outside systemd and Docker.
    std::string findAdapter() {
        try {
            Objects objects = this->objects();
            for (const auto& [path, ifaces] : objects)
                if (ifaces.count(kAdapter1)) return std::string(path);
        } catch (const std::exception& e) {
            std::cerr << tag_ << "adapter lookup failed: " << e.what() << std::endl;
        }
        return {};
    }

    std::string tag_;
    std::string adapter_path_;
    std::unique_ptr<sdbus::IConnection> conn_;
};

} // namespace

std::shared_ptr<ITransport> makeBlueZTransport(const std::string& tag) {
    return std::make_shared<BlueZTransport>(tag);
}

} // namespace hms_cpap::ble

#endif // HMS_BLE_BLUEZ
