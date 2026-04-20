#ifdef WITH_BLE

#include "clients/O2RingBleClient.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <thread>
#include <sstream>

namespace hms_cpap {

static const sdbus::ServiceName BLUEZ{"org.bluez"};
static const sdbus::ObjectPath ROOT{"/"};

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
                if (props.count("Name")) {
                    std::string name = props.at("Name").get<std::string>();
                    if (name.find("O2Ring") != std::string::npos) {
                        device_path_ = std::string(path);
                        std::cout << "O2Ring BLE: Found cached " << name << std::endl;
                        return true;
                    }
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
                    if (props.count("Name")) {
                        std::string name = props.at("Name").get<std::string>();
                        if (name.find("O2Ring") != std::string::npos) {
                            device_path_ = std::string(path);
                            std::cout << "O2Ring BLE: Found " << name << std::endl;
                            break;
                        }
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
                    std::string uuid = props.at("UUID").get<std::string>();
                    if (uuid == WRITE_UUID) {
                        write_char_path_ = p;
                        std::cout << "O2Ring BLE: Write char: " << p << std::endl;
                    } else if (uuid == NOTIFY_UUID) {
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
        std::cerr << "O2Ring BLE: Viatom characteristics not found" << std::endl;
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

    for (size_t i = 0; i < data.size(); i += 20) {
        size_t end = std::min(i + 20, data.size());
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
    processReassembly();
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
