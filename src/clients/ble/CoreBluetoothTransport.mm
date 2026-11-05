// The macOS backend: CoreBluetooth. One CBCentralManager per transport, on its
// own serial dispatch queue, so nothing here needs the main thread's run loop
// (hms_cpap's main thread belongs to the web server).
//
// The first CBCentralManager a process makes asks the user for Bluetooth
// permission. A transport is made only when a Bluetooth feature is on, so
// nobody who never turns one on is asked.
//
// CoreBluetooth names devices by a per-computer identifier, not by their MAC,
// so Device::id and Device::address are that identifier here.
#if defined(HMS_BLE_COREBLUETOOTH)

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include "clients/ble/BleTransport.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

namespace hms_cpap::ble {
struct CbState;
}

@interface HmsCbDelegate : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
- (instancetype)initWithState:(std::shared_ptr<hms_cpap::ble::CbState>)state;
@end

namespace hms_cpap::ble {

namespace {

constexpr auto kStateWait = std::chrono::seconds(5);    // the manager's first state
constexpr auto kCallWait = std::chrono::seconds(5);     // a write, notifications on
constexpr auto kConnectWait = std::chrono::seconds(15); // CoreBluetooth never times a connect out itself

const void* const kQueueKey = &kQueueKey;

std::string str(NSString* s) { return s ? std::string([s UTF8String]) : std::string(); }

bool onQueue() { return dispatch_get_specific(kQueueKey) != nullptr; }

} // namespace

// Everything the delegate learns, for the C++ side to wait on.
struct CbState {
    std::mutex m;
    std::condition_variable cv;
    dispatch_queue_t queue = nil;
    CBCentralManager* manager = nil;

    bool state_known = false;
    CBManagerState state = CBManagerStateUnknown;

    // Seen this process, by identifier.
    std::map<std::string, Device> devices;
    std::map<std::string, uint64_t> device_gen;
    std::map<std::string, CBPeripheral*> peripherals;
    uint64_t gen = 0;

    // The one connection this transport holds at a time.
    CBPeripheral* active = nil;
    bool connected = false;
    bool connect_done = false;
    std::string connect_error;
    int pending_services = -1;   // characteristics discoveries still out
    std::string discover_error;
    CBCharacteristic* write_char = nil;
    CBCharacteristic* notify_char = nil;
    bool notify_done = false;
    std::string notify_error;
    bool write_done = false;
    std::string write_error;
    LinkHandlers handlers;

    void run(dispatch_block_t block) {
        if (onQueue()) block();
        else dispatch_sync(queue, block);
    }
};

namespace {

class CbLink : public ILink {
public:
    explicit CbLink(std::shared_ptr<CbState> s) : s_(std::move(s)) {}
    ~CbLink() override { close(); }

    WriteResult write(const std::vector<uint8_t>& piece, bool with_response) override {
        WriteResult r;
        CBPeripheral* p = nil;
        CBCharacteristic* c = nil;
        {
            std::lock_guard<std::mutex> lk(s_->m);
            p = s_->active;
            c = s_->write_char;
            if (!p || !c || !s_->connected) {
                r.error = "the link is closed";
                return r;
            }
            s_->write_done = false;
            s_->write_error.clear();
        }
        NSData* data = [NSData dataWithBytes:piece.data() length:piece.size()];
        // From inside a notification (the queue's own thread) nothing can be
        // waited for: the acknowledgement arrives on this same thread.
        const bool inside = onQueue();
        if (!with_response) {
            if (!inside) {
                __block bool can = false;
                s_->run(^{ can = p.canSendWriteWithoutResponse; });
                const auto deadline = std::chrono::steady_clock::now() + kCallWait;
                while (!can && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    s_->run(^{ can = p.canSendWriteWithoutResponse; });
                }
                if (!can) {
                    r.timed_out = true;
                    r.error = "the link would not take a write";
                    return r;
                }
            }
            s_->run(^{ [p writeValue:data forCharacteristic:c type:CBCharacteristicWriteWithoutResponse]; });
            r.ok = true;
            return r;
        }
        s_->run(^{ [p writeValue:data forCharacteristic:c type:CBCharacteristicWriteWithResponse]; });
        if (inside) {
            r.ok = true;
            return r;
        }
        std::unique_lock<std::mutex> lk(s_->m);
        if (!s_->cv.wait_for(lk, kCallWait, [&] { return s_->write_done || !s_->connected; })) {
            r.timed_out = true;
            r.error = "no acknowledgement";
            return r;
        }
        if (!s_->write_done) {
            r.error = "the link dropped";
            return r;
        }
        r.ok = s_->write_error.empty();
        r.error = s_->write_error;
        return r;
    }

    uint16_t mtu() const override {
        __block NSUInteger n = 0;
        CBPeripheral* p = s_->active;
        if (!p) return 0;
        s_->run(^{ n = [p maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse]; });
        return n ? static_cast<uint16_t>(n + 3) : 0;
    }

    bool connected() const override {
        std::lock_guard<std::mutex> lk(s_->m);
        return s_->connected;
    }

    void close() override {
        if (closed_) return;
        closed_ = true;
        auto s = s_;
        s->run(^{
            CBPeripheral* p = s->active;
            if (!p) return;
            if (s->notify_char && p.state == CBPeripheralStateConnected)
                [p setNotifyValue:NO forCharacteristic:s->notify_char];
            [s->manager cancelPeripheralConnection:p];
        });
        std::lock_guard<std::mutex> lk(s->m);
        s->connected = false;
        s->handlers = {};
        s->write_char = nil;
        s->notify_char = nil;
        s->cv.notify_all();
    }

private:
    std::shared_ptr<CbState> s_;
    bool closed_ = false;
};

class CoreBluetoothTransport : public ITransport {
public:
    explicit CoreBluetoothTransport(std::string tag) : tag_(std::move(tag)), s_(std::make_shared<CbState>()) {
        s_->queue = dispatch_queue_create("hms_cpap.bluetooth", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(s_->queue, kQueueKey, (void*)kQueueKey, nullptr);
        delegate_ = [[HmsCbDelegate alloc] initWithState:s_];
        // No system alert of our own when Bluetooth is off: radio() says so.
        s_->manager = [[CBCentralManager alloc] initWithDelegate:delegate_
                                                           queue:s_->queue
                                                         options:@{CBCentralManagerOptionShowPowerAlertKey : @NO}];
        std::unique_lock<std::mutex> lk(s_->m);
        s_->cv.wait_for(lk, kStateWait, [&] { return s_->state_known; });
    }

    ~CoreBluetoothTransport() override {
        auto s = s_;
        s->run(^{
            if (s->manager.isScanning) [s->manager stopScan];
            if (s->active) [s->manager cancelPeripheralConnection:s->active];
            s->manager.delegate = nil;
        });
        std::lock_guard<std::mutex> lk(s->m);
        s->handlers = {};
    }

    const char* backend() const override { return "corebluetooth"; }

    bool available() const override {
        std::lock_guard<std::mutex> lk(s_->m);
        return s_->state != CBManagerStateUnsupported;
    }

    RadioStatus radio(bool make_ready) override {
        RadioStatus r;
        r.adapter = "CoreBluetooth";
        {
            std::unique_lock<std::mutex> lk(s_->m);
            s_->cv.wait_for(lk, kStateWait, [&] { return s_->state_known; });
            switch (s_->state) {
            case CBManagerStatePoweredOn:
                r.state = Radio::Ready;
                break;
            case CBManagerStatePoweredOff:
                r.state = Radio::Off;
                r.detail = "Bluetooth is switched off on this Mac: switch it on in System Settings, Bluetooth, and "
                           "try again";
                break;
            case CBManagerStateUnauthorized:
                r.state = Radio::Unauthorized;
                r.detail = "this app is not allowed to use Bluetooth: allow it in System Settings, Privacy & "
                           "Security, Bluetooth, and try again";
                break;
            case CBManagerStateUnsupported:
                r.state = Radio::NoAdapter;
                r.detail = "this Mac has no Bluetooth LE";
                break;
            default:
                r.state = Radio::Failed;
                r.detail = "Bluetooth is not ready yet (resetting or unknown): try again";
                break;
            }
        }
        if (make_ready && r.state != Radio::Ready) std::cerr << tag_ << r.detail << std::endl;
        return r;
    }

    std::vector<Device> known() override {
        std::vector<Device> out;
        std::lock_guard<std::mutex> lk(s_->m);
        for (const auto& [id, d] : s_->devices) {
            Device copy = d;
            copy.advertising = false;
            out.push_back(std::move(copy));
        }
        return out;
    }

    DiscoverResult discover(const std::function<bool(const Device&)>& match, int timeout_ms) override {
        DiscoverResult res;
        if (radio(false).state != Radio::Ready) {
            res.started = false;
            res.error = "Bluetooth is not on";
            return res;
        }
        uint64_t since = 0;
        {
            std::lock_guard<std::mutex> lk(s_->m);
            since = s_->gen;
        }
        auto s = s_;
        s->run(^{
            [s->manager scanForPeripheralsWithServices:nil
                                               options:@{CBCentralManagerScanOptionAllowDuplicatesKey : @YES}];
        });
        // The match runs here, on the caller's thread, as it does on BlueZ.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!res.device && std::chrono::steady_clock::now() < deadline) {
            std::vector<Device> fresh;
            {
                std::unique_lock<std::mutex> lk(s_->m);
                s_->cv.wait_for(lk, std::chrono::milliseconds(250));
                for (const auto& [id, g] : s_->device_gen)
                    if (g > since) fresh.push_back(s_->devices[id]);
                since = s_->gen;
            }
            for (auto& d : fresh) {
                d.advertising = true;
                if (match(d)) {
                    res.device = d;
                    break;
                }
            }
        }
        s->run(^{ [s->manager stopScan]; });
        return res;
    }

    ConnectResult connect(const Device& device, const GattSpec& spec, LinkHandlers handlers,
                          const ConnectOptions& options) override {
        ConnectResult res;
        res.error = LinkError::Connect;
        CBPeripheral* p = nil;
        {
            std::lock_guard<std::mutex> lk(s_->m);
            auto it = s_->peripherals.find(device.id);
            if (it != s_->peripherals.end()) p = it->second;
        }
        if (!p) {
            NSUUID* uuid = [[NSUUID alloc] initWithUUIDString:[NSString stringWithUTF8String:device.id.c_str()]];
            __block NSArray<CBPeripheral*>* found = nil;
            auto s0 = s_;
            if (uuid) s0->run(^{ found = [s0->manager retrievePeripheralsWithIdentifiers:@[ uuid ]]; });
            if (found.count) p = found.firstObject;
        }
        if (!p) {
            res.detail = "the device is not known to this computer";
            return res;
        }
        {
            std::lock_guard<std::mutex> lk(s_->m);
            s_->active = p;
            s_->connected = false;
            s_->connect_done = false;
            s_->connect_error.clear();
            s_->pending_services = -1;
            s_->discover_error.clear();
            s_->write_char = nil;
            s_->notify_char = nil;
            s_->notify_done = false;
            s_->notify_error.clear();
            s_->handlers = std::move(handlers);
        }
        auto s = s_;
        HmsCbDelegate* d = delegate_;
        s->run(^{
            p.delegate = d;
            [s->manager connectPeripheral:p options:nil];
        });
        auto fail = [&](LinkError e, const std::string& why) {
            s->run(^{ [s->manager cancelPeripheralConnection:p]; });
            std::lock_guard<std::mutex> lk(s->m);
            s->connected = false;
            s->handlers = {};
            res.error = e;
            res.detail = why;
            return std::move(res);
        };
        {
            std::unique_lock<std::mutex> lk(s->m);
            s->cv.wait_for(lk, kConnectWait, [&] { return s->connect_done; });
            if (!s->connect_done || !s->connected) {
                const std::string why = s->connect_done ? s->connect_error : "connect timed out";
                lk.unlock();
                std::cerr << tag_ << "connect failed: " << why << std::endl;
                return fail(LinkError::Connect, why);
            }
        }
        s->run(^{ [p discoverServices:nil]; });
        {
            std::unique_lock<std::mutex> lk(s->m);
            const bool done = s->cv.wait_for(lk, std::chrono::milliseconds(options.resolve_timeout_ms), [&] {
                return s->pending_services == 0 || !s->discover_error.empty() || !s->connected;
            });
            if (!done || !s->connected || !s->discover_error.empty()) {
                const std::string why = !s->discover_error.empty() ? s->discover_error : "services not resolved in time";
                lk.unlock();
                return fail(LinkError::Resolve, why);
            }
        }
        // The characteristics, by UUID, across every service.
        __block CBCharacteristic* wc = nil;
        __block CBCharacteristic* nc = nil;
        const std::string want_w = spec.write_uuid, want_n = spec.notify_uuid;
        s->run(^{
            for (CBService* svc in p.services) {
                for (CBCharacteristic* c in svc.characteristics) {
                    const std::string u = normalizeUuid(str(c.UUID.UUIDString));
                    if (u == want_w && !wc) wc = c;
                    else if (u == want_n && !nc) nc = c;
                }
            }
        });
        if (!wc || !nc) return fail(LinkError::NoCharacteristics, "the characteristics were not found");
        std::function<void()> ready;
        {
            std::lock_guard<std::mutex> lk(s->m);
            s->write_char = wc;
            s->notify_char = nc;
            ready = s->handlers.ready;
        }
        if (ready) ready();
        s->run(^{ [p setNotifyValue:YES forCharacteristic:nc]; });
        {
            std::unique_lock<std::mutex> lk(s->m);
            s->cv.wait_for(lk, kCallWait, [&] { return s->notify_done || !s->connected; });
            if (!s->notify_done || !s->notify_error.empty()) {
                const std::string why = s->notify_done ? s->notify_error : "no answer";
                lk.unlock();
                return fail(LinkError::Notify, why);
            }
        }
        res.error = LinkError::None;
        res.link = std::make_unique<CbLink>(s_);
        return res;
    }

private:
    std::string tag_;
    std::shared_ptr<CbState> s_;
    HmsCbDelegate* delegate_ = nil;
};

} // namespace

std::shared_ptr<ITransport> makeCoreBluetoothTransport(const std::string& tag) {
    @autoreleasepool {
        return std::make_shared<CoreBluetoothTransport>(tag);
    }
}

} // namespace hms_cpap::ble

using hms_cpap::ble::CbState;
using hms_cpap::ble::Device;
using hms_cpap::ble::normalizeUuid;

@implementation HmsCbDelegate {
    std::weak_ptr<CbState> _state;
}

- (instancetype)initWithState:(std::shared_ptr<CbState>)state {
    if ((self = [super init])) _state = state;
    return self;
}

static std::string nsstr(NSString* s) { return s ? std::string([s UTF8String]) : std::string(); }

- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
    auto s = _state.lock();
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->m);
    s->state = central.state;
    s->state_known = central.state != CBManagerStateUnknown;
    s->cv.notify_all();
}

- (void)centralManager:(CBCentralManager*)central
    didDiscoverPeripheral:(CBPeripheral*)peripheral
        advertisementData:(NSDictionary<NSString*, id>*)ad
                     RSSI:(NSNumber*)RSSI {
    auto s = _state.lock();
    if (!s) return;
    const std::string id = hms_cpap::ble::lower(nsstr(peripheral.identifier.UUIDString));
    std::string name = nsstr(ad[CBAdvertisementDataLocalNameKey]);
    if (name.empty()) name = nsstr(peripheral.name);
    std::vector<std::string> uuids;
    for (NSString* key in @[ CBAdvertisementDataServiceUUIDsKey, CBAdvertisementDataOverflowServiceUUIDsKey ]) {
        NSArray<CBUUID*>* list = ad[key];
        for (CBUUID* u in list) uuids.push_back(normalizeUuid(nsstr(u.UUIDString)));
    }
    const int rssi = RSSI.intValue;
    std::lock_guard<std::mutex> lk(s->m);
    Device& d = s->devices[id];
    d.id = id;
    d.address = id;
    if (!name.empty()) d.name = name;
    for (const auto& u : uuids)
        if (std::find(d.uuids.begin(), d.uuids.end(), u) == d.uuids.end()) d.uuids.push_back(u);
    // 127 is CoreBluetooth's "not available".
    if (rssi != 127) d.rssi = rssi;
    s->peripherals[id] = peripheral;
    s->device_gen[id] = ++s->gen;
    s->cv.notify_all();
}

- (void)centralManager:(CBCentralManager*)central didConnectPeripheral:(CBPeripheral*)peripheral {
    auto s = _state.lock();
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->m);
    if (peripheral != s->active) return;
    s->connected = true;
    s->connect_done = true;
    s->cv.notify_all();
}

- (void)centralManager:(CBCentralManager*)central
    didFailToConnectPeripheral:(CBPeripheral*)peripheral
                         error:(NSError*)error {
    auto s = _state.lock();
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->m);
    if (peripheral != s->active) return;
    s->connected = false;
    s->connect_done = true;
    s->connect_error = error ? nsstr(error.localizedDescription) : "connect failed";
    s->cv.notify_all();
}

- (void)centralManager:(CBCentralManager*)central
    didDisconnectPeripheral:(CBPeripheral*)peripheral
                      error:(NSError*)error {
    auto s = _state.lock();
    if (!s) return;
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(s->m);
        if (peripheral != s->active) return;
        const bool was = s->connected;
        s->connected = false;
        s->cv.notify_all();
        if (was) cb = s->handlers.disconnected;
    }
    if (cb) cb();
}

- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {
    auto s = _state.lock();
    if (!s) return;
    std::unique_lock<std::mutex> lk(s->m);
    if (error) {
        s->discover_error = nsstr(error.localizedDescription);
        s->cv.notify_all();
        return;
    }
    s->pending_services = static_cast<int>(peripheral.services.count);
    s->cv.notify_all();
    lk.unlock();
    for (CBService* svc in peripheral.services) [peripheral discoverCharacteristics:nil forService:svc];
}

- (void)peripheral:(CBPeripheral*)peripheral
    didDiscoverCharacteristicsForService:(CBService*)service
                                   error:(NSError*)error {
    auto s = _state.lock();
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->m);
    // A service whose characteristics would not list is skipped, not fatal:
    // the one we need may be in another.
    if (s->pending_services > 0) s->pending_services--;
    s->cv.notify_all();
}

- (void)peripheral:(CBPeripheral*)peripheral
    didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)c
                                          error:(NSError*)error {
    auto s = _state.lock();
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->m);
    if (c != s->notify_char) return;
    s->notify_done = true;
    s->notify_error = error ? nsstr(error.localizedDescription) : "";
    s->cv.notify_all();
}

- (void)peripheral:(CBPeripheral*)peripheral didUpdateValueForCharacteristic:(CBCharacteristic*)c error:(NSError*)error {
    auto s = _state.lock();
    if (!s || error) return;
    std::function<void(const std::vector<uint8_t>&)> cb;
    {
        std::lock_guard<std::mutex> lk(s->m);
        if (c != s->notify_char) return;
        cb = s->handlers.notify;
    }
    NSData* v = c.value;
    if (!cb || !v) return;
    const auto* b = static_cast<const uint8_t*>(v.bytes);
    cb(std::vector<uint8_t>(b, b + v.length));
}

- (void)peripheral:(CBPeripheral*)peripheral didWriteValueForCharacteristic:(CBCharacteristic*)c error:(NSError*)error {
    auto s = _state.lock();
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->m);
    s->write_done = true;
    s->write_error = error ? nsstr(error.localizedDescription) : "";
    s->cv.notify_all();
}

@end

#endif // HMS_BLE_COREBLUETOOTH
