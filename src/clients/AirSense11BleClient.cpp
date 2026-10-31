#ifdef WITH_BLE

#include "clients/AirSense11BleClient.h"
#include "clients/BlueZAdapter.h"
#include "services/As11StrWriter.h"

#include <algorithm>
#include <filesystem>
#include <cctype>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>

namespace hms_cpap {

// SDD-057. The conversation is as11::Session; this file is the BlueZ link it
// runs over. Nothing here logs the clientId, the key, the code or a payload.

namespace {

const sdbus::ServiceName BLUEZ{"org.bluez"};
const sdbus::ObjectPath ROOT{"/"};
constexpr const char* kTag = "AirSense11 BLE: ";

using Props = std::map<std::string, sdbus::Variant>;
using Objects = std::map<sdbus::ObjectPath, std::map<std::string, Props>>;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

template <typename T>
std::optional<T> prop(const Props& p, const char* key) {
    auto it = p.find(key);
    if (it == p.end()) return std::nullopt;
    try { return it->second.get<T>(); } catch (...) { return std::nullopt; }
}

} // namespace

AirSense11BleClient::AirSense11BleClient(std::string data_dir, std::string address)
    : data_dir_(std::move(data_dir)),
      address_(lower(std::move(address))),
      session_([this](const std::vector<uint8_t>& f) { return writeFrame(f); }) {
    if (!findAdapter()) {
        std::cerr << kTag << "no Bluetooth adapter: the machine cannot be read directly" << std::endl;
        return;
    }
    conn_ = sdbus::createSystemBusConnection();
    conn_->enterEventLoopAsync();
    std::cout << kTag << "initialized (adapter " << adapter_path_ << ")" << std::endl;
}

AirSense11BleClient::~AirSense11BleClient() {
    stopNightLoop();
    std::lock_guard<std::mutex> lk(op_mtx_);
    session_.pairCancel();
    dropLink();
    if (conn_) conn_->leaveEventLoop();
    conn_.reset();
}

void AirSense11BleClient::note(const std::string& what) {
    {
        std::lock_guard<std::mutex> lk(err_mtx_);
        last_error_ = what;
    }
    std::cerr << kTag << what << std::endl;
}

std::string AirSense11BleClient::lastError() const {
    std::lock_guard<std::mutex> lk(err_mtx_);
    return last_error_;
}

bool AirSense11BleClient::paired() const {
    return as11::loadCredentials(as11::credentialsPath(data_dir_)).has_value();
}

bool AirSense11BleClient::pairPending() const { return session_.pairPending(); }

// ── adapter, scan, connect ──

bool AirSense11BleClient::findAdapter() {
    try {
        auto proxy = sdbus::createProxy(BLUEZ, ROOT);
        Objects objects;
        proxy->callMethod("GetManagedObjects")
            .onInterface("org.freedesktop.DBus.ObjectManager")
            .storeResultsTo(objects);
        for (auto& [path, ifaces] : objects) {
            if (ifaces.count("org.bluez.Adapter1")) {
                adapter_path_ = std::string(path);
                return true;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << kTag << "adapter lookup failed: " << e.what() << std::endl;
    }
    return false;
}

bool AirSense11BleClient::scan(int timeout_ms) {
    device_path_.clear();
    device_name_.clear();
    link_failure_ = as11::Result::Connect;
    // The computer's own radio first: BlueZ will not discover on an adapter
    // that is off, and its NotReady reads like the machine being away.
    const auto radio = bluez::ensurePowered(*conn_, adapter_path_, kTag);
    if (radio.power != bluez::Power::Ready) {
        if (radio.power == bluez::Power::RfKilled) link_failure_ = as11::Result::RadioOff;
        {
            std::lock_guard<std::mutex> lk(err_mtx_);
            last_error_ = radio.detail;
        }
        return false;
    }
    // `advertising`: only a device seen in this discovery (BlueZ sets RSSI on
    // an object while the advert comes in). Off, BlueZ's cache counts too,
    // but only under the machine's own name once that is known.
    auto look = [this](bool advertising) {
        try {
            auto proxy = sdbus::createProxy(*conn_, BLUEZ, ROOT);
            Objects objects;
            proxy->callMethod("GetManagedObjects")
                .onInterface("org.freedesktop.DBus.ObjectManager")
                .withTimeout(bluez::kCallTimeout)
                .storeResultsTo(objects);
            for (auto& [path, ifaces] : objects) {
                auto it = ifaces.find("org.bluez.Device1");
                if (it == ifaces.end()) continue;
                const auto& p = it->second;
                const auto name = prop<std::string>(p, "Name").value_or("");
                const auto uuids = prop<std::vector<std::string>>(p, "UUIDs").value_or(
                    std::vector<std::string>{});
                if (!address_.empty() && lower(prop<std::string>(p, "Address").value_or("")) != address_)
                    continue;
                if (!as11::advertIsAirSense(name, uuids)) continue;
                if (advertising && !prop<int16_t>(p, "RSSI")) continue;
                if (!advertising && !machine_name_.empty() && name != machine_name_) continue;
                device_path_ = std::string(path);
                device_name_ = name.empty() ? "<unnamed AirSense 11>" : name;
                return true;
            }
        } catch (...) {}
        return false;
    };

    if (!rediscover_ && look(false)) {
        std::cout << kTag << "found " << device_name_ << " (known to BlueZ)" << std::endl;
        return true;
    }
    auto adapter = sdbus::createProxy(*conn_, BLUEZ, sdbus::ObjectPath{adapter_path_});
    try {
        adapter->callMethod("StartDiscovery").onInterface("org.bluez.Adapter1").withTimeout(bluez::kCallTimeout);
    } catch (const std::exception& e) {
        note(std::string("scan start failed: ") + e.what());
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool found = false;
    while (!found && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        found = look(true);
    }
    try {
        adapter->callMethod("StopDiscovery").onInterface("org.bluez.Adapter1").withTimeout(bluez::kCallTimeout);
    } catch (...) {}
    if (found) {
        rediscover_ = false;
        std::cout << kTag << "found " << device_name_ << " (advertising now)" << std::endl;
    } else {
        note("machine not advertising (asleep, out of range, or connected elsewhere)");
    }
    return found;
}

bool AirSense11BleClient::connectLink(int timeout_ms) {
    if (!conn_) {
        note("no Bluetooth adapter");
        return false;
    }
    if (!scan(kScanMs)) return false;

    device_proxy_ = sdbus::createProxy(*conn_, BLUEZ, sdbus::ObjectPath{device_path_});
    // The link dropping wakes whatever the session waits on.
    device_proxy_->uponSignal("PropertiesChanged")
        .onInterface("org.freedesktop.DBus.Properties")
        .call([this](const std::string& iface, const Props& changed, const std::vector<std::string>&) {
            if (iface != "org.bluez.Device1") return;
            auto it = changed.find("Connected");
            if (it == changed.end()) return;
            try {
                if (!it->second.get<bool>()) session_.onDisconnect();
            } catch (...) {}
        });

    // BlueZ often aborts the first LE connect (le-connection-abort-by-local).
    bool up = false;
    for (int attempt = 0; attempt < 3 && !up; attempt++) {
        try {
            device_proxy_->callMethod("Connect").onInterface("org.bluez.Device1").withTimeout(bluez::kConnectTimeout);
            up = true;
        } catch (const sdbus::Error& e) {
            std::cerr << kTag << "connect attempt " << (attempt + 1) << "/3 "
                      << (bluez::timedOut(e) ? "timed out" : "failed: " + e.getMessage()) << std::endl;
            try {
                device_proxy_->callMethod("Disconnect").onInterface("org.bluez.Device1").withTimeout(bluez::kCallTimeout);
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
    }
    if (!up) {
        note("connect failed");
        dropLink();
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            sdbus::Variant v;
            device_proxy_->callMethod("Get")
                .onInterface("org.freedesktop.DBus.Properties")
                .withTimeout(bluez::kCallTimeout)
                .withArguments(std::string("org.bluez.Device1"), std::string("ServicesResolved"))
                .storeResultsTo(v);
            if (v.get<bool>()) {
                if (discoverGatt() && enableNotifications()) {
                    std::cout << kTag << "connected to " << device_name_ << std::endl;
                    return true;
                }
                dropLink();
                return false;
            }
        } catch (...) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    note("services not resolved in time");
    dropLink();
    return false;
}

bool AirSense11BleClient::discoverGatt() {
    write_char_path_.clear();
    notify_char_path_.clear();
    write_chunk_ = 20;
    try {
        auto proxy = sdbus::createProxy(*conn_, BLUEZ, ROOT);
        Objects objects;
        proxy->callMethod("GetManagedObjects")
            .onInterface("org.freedesktop.DBus.ObjectManager")
            .storeResultsTo(objects);
        for (auto& [path, ifaces] : objects) {
            const std::string p(path);
            if (p.rfind(device_path_ + "/", 0) != 0) continue;
            auto it = ifaces.find("org.bluez.GattCharacteristic1");
            if (it == ifaces.end()) continue;
            const auto uuid = lower(prop<std::string>(it->second, "UUID").value_or(""));
            if (uuid == as11::kWriteUuid) {
                write_char_path_ = p;
                // BlueZ 5.62+ publishes the link's ATT MTU on the characteristic.
                if (auto mtu = prop<uint16_t>(it->second, "MTU"); mtu && *mtu > 23)
                    write_chunk_ = *mtu - 3;
            } else if (uuid == as11::kNotifyUuid) {
                notify_char_path_ = p;
            }
        }
    } catch (const std::exception& e) {
        note(std::string("GATT discovery failed: ") + e.what());
        return false;
    }
    if (write_char_path_.empty() || notify_char_path_.empty()) {
        note("the machine's characteristics were not found");
        return false;
    }
    write_proxy_ = sdbus::createProxy(*conn_, BLUEZ, sdbus::ObjectPath{write_char_path_});
    return true;
}

bool AirSense11BleClient::enableNotifications() {
    try {
        notify_proxy_ = sdbus::createProxy(*conn_, BLUEZ, sdbus::ObjectPath{notify_char_path_});
        notify_proxy_->uponSignal("PropertiesChanged")
            .onInterface("org.freedesktop.DBus.Properties")
            .call([this](const std::string& iface, const Props& changed, const std::vector<std::string>&) {
                if (iface != "org.bluez.GattCharacteristic1") return;
                auto it = changed.find("Value");
                if (it == changed.end()) return;
                try {
                    session_.onNotify(it->second.get<std::vector<uint8_t>>());
                } catch (...) {}
            });
        // A fresh conversation before the first notification can land.
        session_.onConnected();
        notify_proxy_->callMethod("StartNotify")
            .onInterface("org.bluez.GattCharacteristic1")
            .withTimeout(bluez::kCallTimeout);
        return true;
    } catch (const std::exception& e) {
        note(std::string("notifications could not be enabled: ") + e.what());
        return false;
    }
}

void AirSense11BleClient::dropLink() {
    session_.onDisconnect();
    if (notify_proxy_) {
        try {
            notify_proxy_->callMethod("StopNotify")
                .onInterface("org.bluez.GattCharacteristic1")
                .withTimeout(bluez::kCallTimeout);
        } catch (...) {}
    }
    if (device_proxy_) {
        try {
            device_proxy_->callMethod("Disconnect").onInterface("org.bluez.Device1").withTimeout(bluez::kCallTimeout);
        } catch (...) {}
    }
    notify_proxy_.reset();
    write_proxy_.reset();
    device_proxy_.reset();
    write_char_path_.clear();
    notify_char_path_.clear();
}

bool AirSense11BleClient::writeFrame(const std::vector<uint8_t>& frame) {
    if (!write_proxy_) return false;
    // Write with response, piece by piece at the MTU, as the machine was
    // proven with: each piece is acknowledged before the next goes. A piece
    // the machine never acknowledges fails the write after kCallTimeout, and
    // with it the pull, instead of holding the pull for as long as sd-bus
    // would wait.
    std::map<std::string, sdbus::Variant> options;
    options["type"] = sdbus::Variant{std::string("request")};
    try {
        for (size_t at = 0; at < frame.size(); at += write_chunk_) {
            const size_t end = std::min(at + write_chunk_, frame.size());
            std::vector<uint8_t> piece(frame.begin() + static_cast<long>(at),
                                       frame.begin() + static_cast<long>(end));
            write_proxy_->callMethod("WriteValue")
                .onInterface("org.bluez.GattCharacteristic1")
                .withTimeout(bluez::kCallTimeout)
                .withArguments(piece, options);
        }
    } catch (const sdbus::Error& e) {
        if (bluez::timedOut(e))
            note("write to the machine timed out: the link is dead, the pull is abandoned");
        else
            note("write to the machine failed: " + e.getMessage());
        session_.onDisconnect();
        return false;
    } catch (const std::exception& e) {
        note(std::string("write to the machine failed: ") + e.what());
        return false;
    }
    return true;
}

// ── the machine's operations ──

std::vector<uint8_t> AirSense11BleClient::fetchSummary(const std::string& from) {
    // A night in progress keeps the radio; its end asks for the summary.
    if (streaming_) {
        note("the machine is streaming a night: the summary is read when it ends");
        return {};
    }
    struct Want {
        AirSense11BleClient* c;
        explicit Want(AirSense11BleClient* x) : c(x) { c->want_radio_ = true; c->wake_.notify_all(); }
        ~Want() { c->want_radio_ = false; c->wake_.notify_all(); }
    } want(this);
    std::lock_guard<std::mutex> lk(op_mtx_);
    if (session_.pairPending()) {
        note("a pairing is open: the summary waits for it");
        return {};
    }
    auto creds = as11::loadCredentials(as11::credentialsPath(data_dir_));
    if (!creds) {
        note("not paired with the machine yet");
        return {};
    }
    if (!connectLink(kConnectMs)) return {};

    std::vector<uint8_t> out;
    std::string serial, ident;
    as11::Result r = session_.open(*creds);
    if (r == as11::Result::Ok) r = session_.getState(&serial, nullptr);
    if (r == as11::Result::Ok && writer_ && session_.getIdentification(&ident) == as11::Result::Ok)
        writeIdentification(writer_->rootFor(serial), ident);
    if (r == as11::Result::Ok) r = session_.pullSpool("Summary", from, &out);
    const std::string why = session_.lastError();
    const std::string status = session_.lastSpoolStatus();
    dropLink();
    if (r == as11::Result::Ok && !serial.empty()) afterPull(serial, from, out);
    if (r != as11::Result::Ok) {
        note(std::string("summary from ") + from + " failed: " + as11::resultText(r) +
             (why.empty() ? "" : " (" + why + ")"));
        return {};
    }
    std::cout << kTag << "summary from " << from << ": " << out.size() << " bytes ("
              << (status.empty() ? "complete" : status) << ")" << std::endl;
    return out;
}

as11::Result AirSense11BleClient::pairStart() {
    if (streaming_) {
        note("the machine is in therapy: pair it when the night is over");
        return as11::Result::Connect;
    }
    want_radio_ = true;
    wake_.notify_all();
    std::lock_guard<std::mutex> lk(op_mtx_);
    want_radio_ = false;   // the pending pairing itself holds the night loop off now
    if (session_.pairPending()) {
        session_.pairCancel();
        dropLink();
    }
    if (!connectLink(kConnectMs)) return link_failure_;
    const as11::Result r = session_.pairStart();
    if (r != as11::Result::WaitCode) {
        note(std::string("pairing could not start: ") + session_.lastError());
        dropLink();
        return r;
    }
    std::cout << kTag << "key exchange open with " << device_name_
              << ": the machine shows its code now" << std::endl;
    return r;
}

as11::Result AirSense11BleClient::pairConfirm(const std::string& code) {
    std::lock_guard<std::mutex> lk(op_mtx_);
    as11::Credentials creds;
    as11::Result r = session_.pairConfirm(code, &creds);
    dropLink();
    // The pairing is over, whichever way: the object BlueZ cached for it is
    // dropped, and the first pull finds the machine by its advertisement
    // under its own name. A stale object under a generic name once took the
    // connect and then swallowed the pull.
    machine_name_ = device_name_;
    rediscover_ = true;
    if (bluez::forgetDevice(*conn_, adapter_path_, device_path_))
        std::cout << kTag << "forgot BlueZ's object for " << device_name_
                  << ": the next pull rediscovers the machine" << std::endl;
    device_path_.clear();
    if (r != as11::Result::Ok) {
        note(std::string("pairing failed: ") + session_.lastError());
        return r;
    }
    if (!as11::saveCredentials(as11::credentialsPath(data_dir_), creds)) {
        note("paired, but the pairing could not be saved");
        return as11::Result::Protocol;
    }
    std::cout << kTag << "paired with " << device_name_ << std::endl;
    return r;
}

// ── the live night ──

void AirSense11BleClient::afterPull(const std::string& serial, const std::string& from,
                                    const std::vector<uint8_t>& bytes) {
    if (!writer_) return;
    const std::string root = writer_->rootFor(serial);
    if (!bytes.empty()) keepSummary(root, from, bytes);
    // The machine's own offset, from its noon: the night's files are named on
    // its clock, as its card's are.
    int off = 0;
    if (machineOffsetFromSummaries(root, &off)) writer_->setClock(serial, fixedOffsetClock(off));
    const auto str = writeStrFromSummaries(root, strLayoutDir(data_dir_));
    if (str.written && str.created)
        std::cout << kTag << "STR.edf created: " << str.total << " day(s)" << std::endl;
    else if (str.written)
        std::cout << kTag << "STR.edf: " << str.appended << " day(s) appended, " << str.replaced
                  << " revised, " << str.total << " in the file" << std::endl;
    else std::cout << kTag << "STR.edf unchanged: " << str.why << std::endl;
}

void AirSense11BleClient::startNightLoop(std::shared_ptr<As11NightWriter> writer,
                                         std::function<void()> night_ended) {
    stopNightLoop();
    writer_ = std::move(writer);
    night_ended_ = std::move(night_ended);
    if (!conn_ || !writer_) return;
    running_ = true;
    night_thread_ = std::thread([this] { nightLoop(); });
}

void AirSense11BleClient::stopNightLoop() {
    running_ = false;
    wake_.notify_all();
    if (night_thread_.joinable()) night_thread_.join();
}

void AirSense11BleClient::nightLoop() {
    while (running_) {
        {
            // Out of the way while a pull or a pairing has the radio, or
            // before there is a pairing to open a session with.
            std::unique_lock<std::mutex> lk(wake_mtx_);
            wake_.wait_for(lk, std::chrono::seconds(5), [this] {
                return !running_ || (!want_radio_ && !session_.pairPending() && paired());
            });
        }
        if (!running_) break;
        if (want_radio_ || session_.pairPending() || !paired()) continue;
        const bool reached = streamOnce();
        if (!reached && running_) {
            // Asleep, out of range or in use elsewhere: look again later.
            std::unique_lock<std::mutex> lk(wake_mtx_);
            wake_.wait_for(lk, std::chrono::milliseconds(kRetryMs), [this] { return !running_ || want_radio_.load(); });
        }
    }
}

bool AirSense11BleClient::streamOnce() {
    std::unique_lock<std::mutex> op(op_mtx_);
    if (want_radio_ || session_.pairPending()) return true;
    auto creds = as11::loadCredentials(as11::credentialsPath(data_dir_));
    if (!creds) return true;
    if (!connectLink(kConnectMs)) return false;

    std::string serial;
    bool therapy = false;
    if (session_.open(*creds) != as11::Result::Ok || session_.getState(&serial, &therapy) != as11::Result::Ok) {
        note(std::string("session for the night failed: ") + session_.lastError());
        dropLink();
        return false;
    }
    if (std::string ident; !std::filesystem::exists(std::filesystem::path(writer_->rootFor(serial)) / "Identification.json") &&
                           session_.getIdentification(&ident) == as11::Result::Ok)
        writeIdentification(writer_->rootFor(serial), ident);

    as11::SecondAssembler assembler([this](as11::StreamBlock&& b) { writer_->onBlock(b); });
    assembler.setSerial(serial);
    therapy_ = therapy;
    session_.setStreamHandlers({
        [this, &assembler](const nlohmann::ordered_json& p) {
            if (streaming_) { assembler.onStreamData(p); reports_++; }
        },
        [this](bool t) { therapy_ = t; wake_.notify_all(); },
        [this, serial](const as11::RespEvent& e) { writer_->onEvent(serial, e); },
    });
    struct Clear {
        AirSense11BleClient* c;
        ~Clear() { c->session_.setStreamHandlers({}); c->streaming_ = false; }
    } clear{this};

    if (session_.subscribe() != as11::Result::Ok) {
        note(std::string("subscription failed: ") + session_.lastError());
        dropLink();
        return true;
    }
    // Awake, not breathing yet: stay connected until therapy, a pull or a
    // pairing wants the radio, or the machine lets go.
    while (running_ && !therapy_ && !want_radio_ && session_.connected()) {
        std::unique_lock<std::mutex> lk(wake_mtx_);
        wake_.wait_for(lk, std::chrono::seconds(1));
    }
    if (!therapy_ || !running_ || !session_.connected()) {
        dropLink();
        return true;
    }

    streaming_ = true;
    if (session_.startStream() != as11::Result::Ok) {
        note(std::string("stream refused: ") + session_.lastError());
        dropLink();
        return true;
    }
    std::cout << kTag << "streaming the night from " << device_name_ << std::endl;
    uint64_t last = reports_;
    auto last_progress = std::chrono::steady_clock::now();
    while (running_ && therapy_ && session_.connected()) {
        {
            std::unique_lock<std::mutex> lk(wake_mtx_);
            wake_.wait_for(lk, std::chrono::seconds(1));
        }
        if (reports_ != last) { last = reports_; last_progress = std::chrono::steady_clock::now(); }
        else if (std::chrono::steady_clock::now() - last_progress > std::chrono::milliseconds(kStallMs)) {
            note("the stream stalled");
            break;
        }
    }
    const bool still_up = session_.connected();
    streaming_ = false;
    assembler.flush();
    if (still_up) session_.stopStream();
    session_.setStreamHandlers({});
    writer_->onStreamEnd(serial);
    dropLink();
    std::cout << kTag << "the night's stream ended" << (still_up ? "" : " (the machine let go)") << std::endl;
    op.unlock();
    if (night_ended_) night_ended_();
    return true;
}

void AirSense11BleClient::pairCancel() {
    std::lock_guard<std::mutex> lk(op_mtx_);
    if (!session_.pairPending()) return;
    session_.pairCancel();
    dropLink();
    std::cout << kTag << "pairing cancelled" << std::endl;
}

} // namespace hms_cpap

#endif // WITH_BLE
