#ifdef WITH_BLE

#include "clients/O2RingBleClient.h"
#include "clients/OxyIIProtocol.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <thread>
#include <sstream>

namespace hms_cpap {

static const sdbus::ServiceName BLUEZ{"org.bluez"};
static const sdbus::ObjectPath ROOT{"/"};

// ── Device match ──────────────────────────────────────────────────────
//
// The Viatom/Wellue oximeter line (O2Ring, Checkme O2 / O2 Ultra, SleepU,
// PO1-PO4, ...) shares one GATT profile, but every model advertises a
// different name. Match any known name substring case-insensitively, then
// fall back to the advertised service UUID for the models that drop their
// name from adverts once they have been paired.
//
// "band-wu" is the Checkme O2 Ultra, which advertises neither "checkme" nor
// the Viatom service — its advertisement carries only 16-bit Heart Rate
// (180D), so the UUID fallback cannot see it and the name is the only hook
// we get. Kept specific rather than a bare "band", which would match any
// fitness tracker in range and have us connect to a stranger's device only
// to fail GATT discovery against it.
static const char* const KNOWN_NAME_SUBSTRINGS[] = {
    // Advertised BLE names, NOT the storage device_id (see kOximetryDeviceId).
    "o2ring", "checkme", "checko2", "viatom", "wellue", "sleepu", "oxyring",
    "band-wu",
};

std::optional<O2RingBleClient::Proto> O2RingBleClient::classifyDevice(
        const std::map<std::string, sdbus::Variant>& props, bool* by_t8520) {
    if (by_t8520) *by_t8520 = false;

    // The O2Ring-S first: its name, its idle-mode manufacturer ID, or the
    // OxyII service. It has none of the original family's markers.
    std::string name;
    auto name_it = props.find("Name");
    if (name_it != props.end()) {
        try { name = name_it->second.get<std::string>(); } catch (...) {}
    }
    bool svc = false, mfg = false;
    auto uuids_it = props.find("UUIDs");
    if (uuids_it != props.end()) {
        try {
            for (const auto& uuid : uuids_it->second.get<std::vector<std::string>>())
                if (uuid == oxyii::kServiceUuid) svc = true;
        } catch (...) {}
    }
    auto mfg_it = props.find("ManufacturerData");
    if (mfg_it != props.end()) {
        try {
            auto data = mfg_it->second.get<std::map<uint16_t, sdbus::Variant>>();
            mfg = data.count(oxyii::kManufacturerId) > 0;
        } catch (...) {}
    }
    const bool named = oxyii::nameMatches(name);
    if (svc || mfg || named) {
        if (by_t8520 && !svc && !mfg) {
            std::string lower = name.substr(0, 5);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            *by_t8520 = lower == "t8520";
        }
        return Proto::OxyII;
    }

    if (deviceMatches(props)) return Proto::Legacy;
    return std::nullopt;
}

bool O2RingBleClient::deviceMatches(const std::map<std::string, sdbus::Variant>& props) {
    auto name_it = props.find("Name");
    if (name_it != props.end()) {
        try {
            std::string lower = name_it->second.get<std::string>();
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const char* needle : KNOWN_NAME_SUBSTRINGS)
                if (lower.find(needle) != std::string::npos) return true;
        } catch (...) {}
    }

    // BlueZ publishes advertised service UUIDs on Device1.UUIDs, lowercased,
    // which is the same form as SVC_UUID.
    auto uuids_it = props.find("UUIDs");
    if (uuids_it != props.end()) {
        try {
            for (const auto& uuid : uuids_it->second.get<std::vector<std::string>>())
                if (uuid == SVC_UUID) return true;
        } catch (...) {}
    }
    return false;
}

std::string O2RingBleClient::describeDevice(const std::map<std::string, sdbus::Variant>& props) {
    auto it = props.find("Name");
    if (it != props.end()) {
        try {
            std::string name = it->second.get<std::string>();
            if (!name.empty()) return name;
        } catch (...) {}
    }
    return "<unnamed, matched by service UUID>";
}

// ── Viatom CRC-8 ──────────────────────────────────────────────────────

uint8_t O2RingBleClient::crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t chk = crc ^ data[i];
        crc = 0;
        if (chk & 0x01) crc  = 0x07;
        if (chk & 0x02) crc ^= 0x0e;
        if (chk & 0x04) crc ^= 0x1c;
        if (chk & 0x08) crc ^= 0x38;
        if (chk & 0x10) crc ^= 0x70;
        if (chk & 0x20) crc ^= 0xe0;
        if (chk & 0x40) crc ^= 0xc7;
        if (chk & 0x80) crc ^= 0x89;
    }
    return crc;
}

std::vector<uint8_t> O2RingBleClient::buildCmd(uint8_t cmd, uint16_t block,
                                                 const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0xAA);
    frame.push_back(cmd);
    frame.push_back(cmd ^ 0xFF);
    frame.push_back(block & 0xFF);
    frame.push_back((block >> 8) & 0xFF);
    uint16_t plen = static_cast<uint16_t>(payload.size());
    frame.push_back(plen & 0xFF);
    frame.push_back((plen >> 8) & 0xFF);
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(crc8(frame.data(), frame.size()));
    return frame;
}

// ── Constructor / Destructor ───────────────────────────────────────────

O2RingBleClient::O2RingBleClient() {
    adapter_path_ = findAdapter();
    if (adapter_path_.empty()) {
        std::cerr << "O2Ring BLE: No Bluetooth adapter detected — plug in a USB BLE adapter or switch to HTTP mode" << std::endl;
        return;
    }
    startDBusLoop();
    std::cout << "O2Ring BLE: Initialized (adapter: " << adapter_path_ << ")" << std::endl;
}

O2RingBleClient::~O2RingBleClient() {
    disconnect();
    stopDBusLoop();
}

// ── D-Bus event loop ───────────────────────────────────────────────────

void O2RingBleClient::startDBusLoop() {
    conn_ = sdbus::createSystemBusConnection();
    conn_->enterEventLoopAsync();
    dbus_running_ = true;
}

void O2RingBleClient::stopDBusLoop() {
    dbus_running_ = false;
    if (conn_) conn_->leaveEventLoop();
    conn_.reset();
}

std::string O2RingBleClient::findAdapter() {
    try {
        auto proxy = sdbus::createProxy(BLUEZ, ROOT);
        std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objects;
        proxy->callMethod("GetManagedObjects")
            .onInterface("org.freedesktop.DBus.ObjectManager")
            .storeResultsTo(objects);

        for (auto& [path, ifaces] : objects) {
            if (ifaces.count("org.bluez.Adapter1")) {
                return std::string(path);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "O2Ring BLE: findAdapter error: " << e.what() << std::endl;
    }
    return "";
}

// ── Scan ───────────────────────────────────────────────────────────────

bool O2RingBleClient::scan(int timeout_ms) {
    if (adapter_path_.empty() || !conn_) return false;
    device_path_.clear();

    // Check already-known devices
    try {
        auto proxy = sdbus::createProxy(*conn_, BLUEZ, ROOT);
        std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objects;
        proxy->callMethod("GetManagedObjects")
            .onInterface("org.freedesktop.DBus.ObjectManager")
            .storeResultsTo(objects);

        for (auto& [path, ifaces] : objects) {
            if (ifaces.count("org.bluez.Device1")) {
                auto& props = ifaces.at("org.bluez.Device1");
                if (skip_paths_.count(std::string(path))) continue;
                bool by_t8520 = false;
                if (auto proto = classifyDevice(props, &by_t8520)) {
                    device_path_ = std::string(path);
                    proto_ = *proto;
                    oxy_by_t8520_ = by_t8520;
                    std::cout << "O2Ring BLE: Found cached " << describeDevice(props)
                              << (proto_ == Proto::OxyII ? " (O2Ring-S)" : "") << std::endl;
                    return true;
                }
            }
        }
    } catch (...) {}

    // Start discovery
    auto adapter = sdbus::createProxy(*conn_, BLUEZ, sdbus::ObjectPath{adapter_path_});
    try {
        adapter->callMethod("StartDiscovery").onInterface("org.bluez.Adapter1");
    } catch (const std::exception& e) {
        std::cerr << "O2Ring BLE: StartDiscovery failed: " << e.what() << std::endl;
        return false;
    }

    // Poll for device appearance
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline && device_path_.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        try {
            auto proxy = sdbus::createProxy(*conn_, BLUEZ, ROOT);
            std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objects;
            proxy->callMethod("GetManagedObjects")
                .onInterface("org.freedesktop.DBus.ObjectManager")
                .storeResultsTo(objects);

            for (auto& [path, ifaces] : objects) {
                if (ifaces.count("org.bluez.Device1")) {
                    auto& props = ifaces.at("org.bluez.Device1");
                    if (skip_paths_.count(std::string(path))) continue;
                    bool by_t8520 = false;
                    if (auto proto = classifyDevice(props, &by_t8520)) {
                        device_path_ = std::string(path);
                        proto_ = *proto;
                        oxy_by_t8520_ = by_t8520;
                        std::cout << "O2Ring BLE: Found " << describeDevice(props)
                                  << (proto_ == Proto::OxyII ? " (O2Ring-S)" : "") << std::endl;
                        break;
                    }
                }
            }
        } catch (...) {}
    }

    try {
        adapter->callMethod("StopDiscovery").onInterface("org.bluez.Adapter1");
    } catch (...) {}

    return !device_path_.empty();
}

// ── Connect ────────────────────────────────────────────────────────────

bool O2RingBleClient::connect(int timeout_ms) {
    if (device_path_.empty() || !conn_) return false;

    auto device = sdbus::createProxy(*conn_, BLUEZ, sdbus::ObjectPath{device_path_});

    // Retry connect — BlueZ frequently aborts the first attempt with le-connection-abort-by-local
    bool connect_ok = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        try {
            device->callMethod("Connect").onInterface("org.bluez.Device1");
            connect_ok = true;
            break;
        } catch (const std::exception& e) {
            std::cerr << "O2Ring BLE: Connect attempt " << (attempt + 1) << "/3 failed: "
                      << e.what() << std::endl;
            // Disconnect cleanly before retry
            try { device->callMethod("Disconnect").onInterface("org.bluez.Device1"); } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
    }
    if (!connect_ok) return false;

    // Wait for ServicesResolved (fresh 15s timeout, independent of connect retries)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15000);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        try {
            sdbus::Variant resolved = device->getProperty("ServicesResolved")
                .onInterface("org.bluez.Device1");
            if (resolved.get<bool>()) {
                connected_ = true;
                std::cout << "O2Ring BLE: Connected, services resolved" << std::endl;
                return discoverGatt() && enableNotifications();
            }
        } catch (...) {}
    }

    std::cerr << "O2Ring BLE: Services not resolved (timeout)" << std::endl;
    disconnect();
    return false;
}

void O2RingBleClient::disconnect() {
    if (device_path_.empty() || !conn_) return;
    try {
        auto device = sdbus::createProxy(*conn_, BLUEZ, sdbus::ObjectPath{device_path_});
        device->callMethod("Disconnect").onInterface("org.bluez.Device1");
    } catch (...) {}
    connected_ = false;
    write_char_path_.clear();
    notify_char_path_.clear();
    reasm_buf_.clear();
    notify_proxy_.reset();
}

// ── GATT Discovery ─────────────────────────────────────────────────────

bool O2RingBleClient::discoverGatt() {
    try {
        auto proxy = sdbus::createProxy(*conn_, BLUEZ, ROOT);
        std::map<sdbus::ObjectPath, std::map<std::string, std::map<std::string, sdbus::Variant>>> objects;
        proxy->callMethod("GetManagedObjects")
            .onInterface("org.freedesktop.DBus.ObjectManager")
            .storeResultsTo(objects);

        for (auto& [path, ifaces] : objects) {
            std::string p = std::string(path);
            if (p.find(device_path_) != 0) continue;

            if (ifaces.count("org.bluez.GattCharacteristic1")) {
                auto& props = ifaces.at("org.bluez.GattCharacteristic1");
                if (props.count("UUID")) {
                    const bool oxy = proto_ == Proto::OxyII;
                    std::string uuid = props.at("UUID").get<std::string>();
                    if (uuid == (oxy ? oxyii::kWriteUuid : WRITE_UUID)) {
                        write_char_path_ = p;
                        std::cout << "O2Ring BLE: Write char: " << p << std::endl;
                        // BlueZ 5.62+ publishes the link's ATT MTU here. An
                        // O2Ring-S refuses file opens below 517, so say so.
                        if (props.count("MTU")) {
                            try {
                                const uint16_t mtu = props.at("MTU").get<uint16_t>();
                                if (mtu > 3) write_chunk_ = mtu - 3;
                                std::cout << "O2Ring BLE: MTU " << mtu << std::endl;
                                if (oxy && mtu < 517)
                                    std::cerr << "O2Ring BLE: MTU " << mtu
                                              << " is under the 517 O2Ring-S file reads need"
                                              << std::endl;
                            } catch (...) {}
                        }
                    } else if (uuid == (oxy ? oxyii::kNotifyUuid : NOTIFY_UUID)) {
                        notify_char_path_ = p;
                        std::cout << "O2Ring BLE: Notify char: " << p << std::endl;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "O2Ring BLE: GATT discovery error: " << e.what() << std::endl;
        return false;
    }

    if (write_char_path_.empty() || notify_char_path_.empty()) {
        if (proto_ == Proto::OxyII) {
            std::cerr << "O2Ring BLE: OxyII characteristics not found" << std::endl;
            if (oxy_by_t8520_) {
                // The worn identity without the service, as the protocol
                // write-up warns: pass it over until this process restarts.
                std::cerr << "O2Ring BLE: T8520 identity has no OxyII service; "
                             "waiting for its S8-AW identity" << std::endl;
                skip_paths_.insert(device_path_);
            }
        } else {
            std::cerr << "O2Ring BLE: Viatom characteristics not found" << std::endl;
        }
        return false;
    }
    return true;
}

bool O2RingBleClient::enableNotifications() {
    try {
        notify_proxy_ = sdbus::createProxy(*conn_, BLUEZ, sdbus::ObjectPath{notify_char_path_});

        notify_proxy_->uponSignal("PropertiesChanged")
            .onInterface("org.freedesktop.DBus.Properties")
            .call([this](const std::string& iface,
                          const std::map<std::string, sdbus::Variant>& changed,
                          const std::vector<std::string>&) {
                if (iface == "org.bluez.GattCharacteristic1" && changed.count("Value")) {
                    auto value = changed.at("Value").get<std::vector<uint8_t>>();
                    onNotification(value);
                }
            });
        notify_proxy_->callMethod("StartNotify")
            .onInterface("org.bluez.GattCharacteristic1");

        std::cout << "O2Ring BLE: Notifications enabled" << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "O2Ring BLE: StartNotify failed: " << e.what() << std::endl;
        return false;
    }
}

// ── Write + Notification ───────────────────────────────────────────────

void O2RingBleClient::writeCharacteristic(const std::vector<uint8_t>& data) {
    auto proxy = sdbus::createProxy(*conn_, BLUEZ, sdbus::ObjectPath{write_char_path_});

    // The original O2Ring has always been written in 20-byte pieces; the
    // O2Ring-S takes a whole frame per write once the MTU allows it.
    const size_t chunk_max = (proto_ == Proto::OxyII) ? std::max<size_t>(write_chunk_, 20) : 20;
    for (size_t i = 0; i < data.size(); i += chunk_max) {
        size_t end = std::min(i + chunk_max, data.size());
        std::vector<uint8_t> chunk(data.begin() + i, data.begin() + end);

        std::map<std::string, sdbus::Variant> options;
        options["type"] = sdbus::Variant{"command"};

        proxy->callMethod("WriteValue")
            .onInterface("org.bluez.GattCharacteristic1")
            .withArguments(chunk, options);

        if (end < data.size())
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void O2RingBleClient::onNotification(const std::vector<uint8_t>& value) {
    reasm_buf_.insert(reasm_buf_.end(), value.begin(), value.end());
    if (proto_ != Proto::OxyII) {
        processReassembly();
        return;
    }
    // OxyII: the reply the caller waits for lands in resp_buf_; anything else
    // (a late reply to an earlier request) is dropped.
    while (auto frame = oxyii::takeFrame(reasm_buf_)) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (static_cast<int>(frame->op) != oxy_wait_op_) continue;
        resp_buf_ = std::move(frame->payload);
        resp_ready_ = true;
        oxy_wait_op_ = -1;
        cv_.notify_all();
    }
}

void O2RingBleClient::processReassembly() {
    while (!reasm_buf_.empty()) {
        while (!reasm_buf_.empty() && reasm_buf_[0] != 0xAA && reasm_buf_[0] != 0x55) {
            reasm_buf_.erase(reasm_buf_.begin());
        }
        if (reasm_buf_.size() < 7) return;

        if ((reasm_buf_[1] ^ 0xFF) != reasm_buf_[2]) {
            reasm_buf_.erase(reasm_buf_.begin());
            continue;
        }

        uint16_t plen = reasm_buf_[5] | (static_cast<uint16_t>(reasm_buf_[6]) << 8);
        size_t total = 7 + plen + 1;
        if (reasm_buf_.size() < total) return;

        uint8_t want = reasm_buf_[total - 1];
        uint8_t got = crc8(reasm_buf_.data(), total - 1);
        if (want != got) {
            reasm_buf_.erase(reasm_buf_.begin());
            continue;
        }

        dispatchFrame(reasm_buf_.data(), total);
        reasm_buf_.erase(reasm_buf_.begin(), reasm_buf_.begin() + total);
    }
}

void O2RingBleClient::dispatchFrame(const uint8_t* frame, size_t len) {
    if (len < 8) return;
    uint16_t payload_len = frame[5] | (static_cast<uint16_t>(frame[6]) << 8);

    if (dl_active_) {
        const uint8_t* data = frame + 7;
        size_t to_copy = std::min(static_cast<size_t>(payload_len),
                                   static_cast<size_t>(dl_file_size_ - dl_offset_));
        if (to_copy > 0 && dl_offset_ + to_copy <= dl_buf_.size()) {
            std::copy(data, data + to_copy, dl_buf_.begin() + dl_offset_);
            dl_offset_ += to_copy;
        }

        if (dl_offset_ >= dl_file_size_) {
            std::lock_guard<std::mutex> lk(mtx_);
            resp_ready_ = true;
            cv_.notify_all();
        } else {
            dl_block_++;
            writeCharacteristic(buildCmd(0x04, dl_block_));
        }
        return;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    resp_buf_.assign(frame + 7, frame + 7 + payload_len);
    resp_ready_ = true;
    cv_.notify_all();
}

bool O2RingBleClient::sendAndWait(uint8_t cmd, uint16_t block,
                                    const std::vector<uint8_t>& payload,
                                    int timeout_ms) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        resp_ready_ = false;
        resp_buf_.clear();
    }

    writeCharacteristic(buildCmd(cmd, block, payload));

    std::unique_lock<std::mutex> lk(mtx_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                         [this]() { return resp_ready_; });
}

// ── OxyII (O2Ring-S) ───────────────────────────────────────────────────

bool O2RingBleClient::oxyRequest(uint8_t op, const std::vector<uint8_t>& payload,
                                 int timeout_ms) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        resp_ready_ = false;
        resp_buf_.clear();
        oxy_wait_op_ = op;
    }
    try {
        writeCharacteristic(oxyii::encode(op, oxy_seq_++, payload));
    } catch (const std::exception& e) {
        std::cerr << "O2Ring BLE: OxyII write 0x" << std::hex << int(op) << std::dec
                  << " failed: " << e.what() << std::endl;
        std::lock_guard<std::mutex> lk(mtx_);
        oxy_wait_op_ = -1;
        return false;
    }
    std::unique_lock<std::mutex> lk(mtx_);
    const bool ok = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                 [this]() { return resp_ready_; });
    oxy_wait_op_ = -1;
    return ok;
}

// The per-connection handshake: GET_INFO, auth 0xFF (never answered), setup
// 0x10, set time 0xC0, GET_CONFIG 0x00. The ring refuses file opens without it.
// Set time writes back the ring's own clock, as the Push-C3 miner does, so the
// ring's clock and its file names stay as the owner's app set them.
bool O2RingBleClient::oxySession() {
    if (!oxyRequest(oxyii::kOpGetInfo, {}, 5000)) {
        std::cerr << "O2Ring BLE: O2Ring-S GET_INFO timeout" << std::endl;
        return false;
    }
    auto info = oxyii::parseInfo(resp_buf_);
    if (info) {
        const auto& c = info->clock;
        std::cout << "O2Ring BLE: O2Ring-S SN=" << info->serial << " fw=" << info->firmware
                  << " clock=" << c.year << "-" << int(c.month) << "-" << int(c.day) << " "
                  << int(c.hour) << ":" << int(c.minute) << ":" << int(c.second)
                  << (info->clock_valid ? "" : " (NOT SET)") << std::endl;
    }
    const bool clock_ok = info && info->clock_valid;

    try {
        writeCharacteristic(oxyii::encode(oxyii::kOpAuth, oxy_seq_++,
                                          oxyii::authPayload(clock_ok ? oxyii::epochOf(info->clock) : 0)));
    } catch (const std::exception& e) {
        std::cerr << "O2Ring BLE: O2Ring-S auth write failed: " << e.what() << std::endl;
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!oxyRequest(oxyii::kOpSetup, {0x00}, 3000)) {
        std::cerr << "O2Ring BLE: O2Ring-S setup timeout" << std::endl;
        return false;
    }
    if (clock_ok) {
        if (!oxyRequest(oxyii::kOpSetTime, oxyii::timePayload(info->clock), 3000))
            std::cerr << "O2Ring BLE: O2Ring-S set time: no reply" << std::endl;
    } else {
        std::cerr << "O2Ring BLE: the O2Ring-S clock is not set; file reads may be refused "
                     "until its app has set it once" << std::endl;
    }
    if (!oxyRequest(oxyii::kOpGetConfig, {}, 3000))
        std::cerr << "O2Ring BLE: O2Ring-S GET_CONFIG: no reply" << std::endl;
    return true;
}

// 0xF4 clears a handle the ring's own overnight recording left open, without
// which 0xF1 hangs. A no-op when nothing is open, and some firmware does not
// answer it then, so no reply is not a failure.
void O2RingBleClient::oxyCloseFile() {
    oxyRequest(oxyii::kOpFileEnd, {}, 2000);
}

std::vector<std::string> O2RingBleClient::oxyListFiles() {
    std::vector<std::string> files;
    if (!oxySession()) return files;
    oxyCloseFile();
    if (!oxyRequest(oxyii::kOpFileList, {}, 5000)) {
        std::cerr << "O2Ring BLE: O2Ring-S file list timeout" << std::endl;
        return files;
    }
    files = oxyii::parseFileList(resp_buf_);
    if (oxyRequest(oxyii::kOpGetBattery, {}, 3000) && resp_buf_.size() >= 2)
        cached_battery_ = resp_buf_[1];
    std::cout << "O2Ring BLE: O2Ring-S " << files.size() << " files, battery "
              << cached_battery_ << "%" << std::endl;
    return files;
}

IO2RingClient::LiveReading O2RingBleClient::oxyLive() {
    LiveReading r;
    if (!oxySession() || !oxyRequest(oxyii::kOpLive, {}, 5000)) return r;
    if (auto l = oxyii::parseLive(resp_buf_)) {
        r.spo2 = l->reading ? l->spo2 : 0xFF;
        r.hr = l->reading ? l->hr : 0xFF;
        r.motion = l->motion;
        r.active = l->reading;
        r.valid = l->reading;
        cached_battery_ = l->battery;
    }
    return r;
}

// Firmware 2D010003 caps what one connection may transfer, so a pull that
// stalls part-way reconnects and carries on at its offset.
std::vector<uint8_t> O2RingBleClient::oxyDownload(const std::string& filename) {
    std::vector<uint8_t> data;
    auto start = oxyii::fileStartPayload(filename);
    if (!start) {
        std::cerr << "O2Ring BLE: " << filename << " is not an O2Ring-S file name" << std::endl;
        return data;
    }

    constexpr int kMaxResumes = 3;
    constexpr uint32_t kMaxSize = 1024 * 1024;
    uint32_t size = 0;
    bool sized = false;
    int resumes = 0;

    for (;;) {
        bool ok = oxySession();
        if (ok) {
            oxyCloseFile();
            ok = oxyRequest(oxyii::kOpFileStart, *start, 5000) && resp_buf_.size() >= 4;
        }
        if (ok) {
            const uint32_t sz = oxyii::le32(resp_buf_.data());
            if (!sized) {
                if (sz == 0 || sz > kMaxSize) {
                    std::cerr << "O2Ring BLE: " << filename << " size " << sz << " refused" << std::endl;
                    break;
                }
                size = sz;
                sized = true;
                data.reserve(size);
                std::cout << "O2Ring BLE: Downloading " << filename << " (" << size << " bytes)"
                          << std::endl;
            } else if (sz != size) {
                // Still being recorded: the next poll pulls it again.
                std::cerr << "O2Ring BLE: " << filename << " changed size " << size << " -> "
                          << sz << " across a resume" << std::endl;
                data.clear();
                break;
            }
        }

        bool ended = false;
        while (ok && data.size() < size) {
            ok = oxyRequest(oxyii::kOpFileData,
                            oxyii::fileDataPayload(static_cast<uint32_t>(data.size())), 5000);
            if (!ok) break;
            if (resp_buf_.empty()) { ended = true; break; }  // the ring has nothing more
            const size_t n = std::min(resp_buf_.size(), static_cast<size_t>(size) - data.size());
            data.insert(data.end(), resp_buf_.begin(), resp_buf_.begin() + n);
        }
        if (ok || ended) break;

        // Before the size is known there is nothing to resume.
        if (!sized || ++resumes > kMaxResumes) break;
        std::cerr << "O2Ring BLE: " << filename << " stalled at " << data.size() << "/" << size
                  << "; reconnect and resume " << resumes << "/" << kMaxResumes << std::endl;
        disconnect();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (!connect()) break;
    }

    if (connected_) oxyCloseFile();
    if (sized && data.size() < size) {
        std::cerr << "O2Ring BLE: Download incomplete: " << data.size() << "/" << size << std::endl;
        data.clear();
    } else if (!data.empty()) {
        std::cout << "O2Ring BLE: Downloaded " << data.size() << " bytes" << std::endl;
    }
    return data;
}

// ── Public API ─────────────────────────────────────────────────────────

bool O2RingBleClient::isConnected() {
    return connected_;
}

IO2RingClient::LiveReading O2RingBleClient::getLive() {
    LiveReading r;
    std::cout << "O2Ring BLE: getLive() scanning..." << std::endl;
    if (!scan()) { std::cout << "O2Ring BLE: scan failed" << std::endl; return r; }
    std::cout << "O2Ring BLE: found, connecting..." << std::endl;
    if (!connect()) { std::cout << "O2Ring BLE: connect failed" << std::endl; return r; }

    if (proto_ == Proto::OxyII) {
        r = oxyLive();
        disconnect();
        return r;
    }

    if (sendAndWait(0x17, 0, {}, 10000)) {
        if (resp_buf_.size() >= 12) {
            r.spo2 = resp_buf_[0];
            r.hr = resp_buf_[1];
            r.motion = (resp_buf_.size() > 9) ? resp_buf_[9] : 0;
            r.active = (r.spo2 != 0xFF && r.hr != 0xFF);
            r.valid = r.active && r.spo2 > 0 && r.hr > 0;
            if (resp_buf_.size() > 7) cached_battery_ = resp_buf_[7];
        }
    }

    disconnect();
    return r;
}

std::vector<std::string> O2RingBleClient::listFiles() {
    std::vector<std::string> files;
    if (!scan() || !connect()) return files;

    if (proto_ == Proto::OxyII) {
        files = oxyListFiles();
        disconnect();
        return files;
    }

    if (sendAndWait(0x14, 0, {}, 10000)) {
        size_t json_len = resp_buf_.size();
        while (json_len > 0 && resp_buf_[json_len - 1] == 0) json_len--;
        if (json_len == 0) { disconnect(); return files; }

        std::string json_str(resp_buf_.begin(), resp_buf_.begin() + json_len);
        try {
            auto j = nlohmann::json::parse(json_str);
            if (j.contains("CurBAT")) {
                cached_battery_ = std::atoi(j["CurBAT"].get<std::string>().c_str());
            }
            if (j.contains("FileList") && !j["FileList"].get<std::string>().empty()) {
                std::istringstream ss(j["FileList"].get<std::string>());
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
                    if (!tok.empty()) files.push_back(tok);
                }
            }
            std::cout << "O2Ring BLE: INFO — " << files.size() << " files, battery "
                      << cached_battery_ << "%" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "O2Ring BLE: INFO parse error: " << e.what() << std::endl;
        }
    }

    disconnect();
    return files;
}

std::vector<uint8_t> O2RingBleClient::downloadFile(const std::string& filename) {
    std::vector<uint8_t> result;
    if (!scan() || !connect()) return result;

    if (proto_ == Proto::OxyII) {
        result = oxyDownload(filename);
        disconnect();
        return result;
    }

    std::vector<uint8_t> name_payload(filename.begin(), filename.end());
    name_payload.push_back(0x00);

    if (!sendAndWait(0x03, 0, name_payload, 10000)) {
        std::cerr << "O2Ring BLE: FILE_OPEN timeout" << std::endl;
        disconnect();
        return result;
    }

    if (resp_buf_.size() < 4) {
        writeCharacteristic(buildCmd(0x05));
        disconnect();
        return result;
    }

    uint32_t file_size = resp_buf_[0] | (static_cast<uint32_t>(resp_buf_[1]) << 8) |
                          (static_cast<uint32_t>(resp_buf_[2]) << 16) |
                          (static_cast<uint32_t>(resp_buf_[3]) << 24);

    if (file_size == 0 || file_size > 256 * 1024) {
        writeCharacteristic(buildCmd(0x05));
        disconnect();
        return result;
    }

    std::cout << "O2Ring BLE: Downloading " << filename << " (" << file_size << " bytes)" << std::endl;

    dl_buf_.resize(file_size);
    dl_file_size_ = file_size;
    dl_offset_ = 0;
    dl_block_ = 0;
    dl_active_ = true;

    {
        std::lock_guard<std::mutex> lk(mtx_);
        resp_ready_ = false;
    }

    writeCharacteristic(buildCmd(0x04, 0));

    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, std::chrono::seconds(120), [this]() { return resp_ready_; });
    }

    dl_active_ = false;

    if (dl_offset_ >= file_size) {
        result.assign(dl_buf_.begin(), dl_buf_.begin() + dl_offset_);
        std::cout << "O2Ring BLE: Downloaded " << dl_offset_ << " bytes" << std::endl;
    } else {
        std::cerr << "O2Ring BLE: Download incomplete: " << dl_offset_ << "/" << file_size << std::endl;
    }

    writeCharacteristic(buildCmd(0x05));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    disconnect();
    return result;
}

} // namespace hms_cpap

#endif // WITH_BLE
