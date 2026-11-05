#include "clients/AirSense11BleClient.h"
#include "services/As11Settings.h"
#include "services/As11StrWriter.h"

#include <algorithm>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <thread>

namespace hms_cpap {

// SDD-057. The conversation is as11::Session; the link is a ble::ITransport.
// Nothing here logs the clientId, the key, the code or a payload.

namespace {
constexpr const char* kTag = "AirSense11 BLE: ";
} // namespace

AirSense11BleClient::AirSense11BleClient(std::string data_dir, std::string address,
                                         std::shared_ptr<ble::ITransport> transport, as11::Timeouts timeouts)
    : data_dir_(std::move(data_dir)),
      address_(ble::lower(std::move(address))),
      transport_(transport ? std::move(transport) : ble::makeHostTransport(kTag)),
      session_([this](const std::vector<uint8_t>& f) { return writeFrame(f); }, timeouts) {
    if (!transport_ || !transport_->available()) {
        std::cerr << kTag << "no Bluetooth adapter: the machine cannot be read directly" << std::endl;
        return;
    }
    const auto radio = transport_->radio(false);
    std::cout << kTag << "initialized (" << transport_->backend()
              << (radio.adapter.empty() ? "" : ", adapter " + radio.adapter) << ")" << std::endl;
}

AirSense11BleClient::~AirSense11BleClient() {
    stopNightLoop();
    std::lock_guard<std::mutex> lk(op_mtx_);
    session_.pairCancel();
    dropLink();
    transport_.reset();
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

bool AirSense11BleClient::scan(int timeout_ms) {
    device_ = {};
    have_device_ = false;
    link_failure_ = as11::Result::Connect;
    // The computer's own radio first: an adapter that is off cannot discover,
    // and the OS's refusal reads like the machine being away.
    const auto radio = transport_->radio(true);
    if (radio.state != ble::Radio::Ready) {
        if (radio.state == ble::Radio::RfKilled || radio.state == ble::Radio::Off ||
            radio.state == ble::Radio::Unauthorized)
            link_failure_ = as11::Result::RadioOff;
        {
            std::lock_guard<std::mutex> lk(err_mtx_);
            last_error_ = radio.detail;
        }
        return false;   // radio(true) has logged why
    }
    // `advertising`: only a device seen in this discovery. Off, the OS's
    // cache counts too, but only under the machine's own name once that is
    // known.
    auto wanted = [this](const ble::Device& d, bool advertising) {
        if (!address_.empty() && ble::lower(d.address) != address_) return false;
        if (!as11::advertIsAirSense(d.name, d.uuids)) return false;
        if (advertising && !d.advertising) return false;
        if (!advertising && !machine_name_.empty() && d.name != machine_name_) return false;
        return true;
    };
    auto take = [this](const ble::Device& d) {
        device_ = d;
        if (device_.name.empty()) device_.name = "<unnamed AirSense 11>";
        have_device_ = true;
    };

    if (!rediscover_) {
        for (const auto& d : transport_->known()) {
            if (!wanted(d, false)) continue;
            take(d);
            std::cout << kTag << "found " << device_.name << " (already known)" << std::endl;
            return true;
        }
    }
    const auto res = transport_->discover([&](const ble::Device& d) { return wanted(d, true); }, timeout_ms);
    if (!res.started) {
        note("scan start failed: " + res.error);
        return false;
    }
    const bool found = res.device.has_value();
    if (found) take(*res.device);
    if (found) {
        rediscover_ = false;
        std::cout << kTag << "found " << device_.name << " (advertising now)" << std::endl;
    } else {
        note("machine not advertising (asleep, out of range, or connected elsewhere)");
    }
    return found;
}

bool AirSense11BleClient::connectLink(int timeout_ms) {
    if (!transport_ || !transport_->available()) {
        note("no Bluetooth adapter");
        return false;
    }
    if (!scan(kScanMs)) return false;

    auto attempt = [this, timeout_ms] {
        ble::LinkHandlers handlers;
        // A fresh conversation before the first notification can land.
        handlers.ready = [this] { session_.onConnected(); };
        handlers.notify = [this](const std::vector<uint8_t>& v) { session_.onNotify(v); };
        // The link dropping wakes whatever the session waits on.
        handlers.disconnected = [this] { session_.onDisconnect(); };
        ble::ConnectOptions options;
        options.resolve_timeout_ms = timeout_ms;
        options.resolve_poll_ms = 250;
        return transport_->connect(device_, {as11::kWriteUuid, as11::kNotifyUuid}, std::move(handlers), options);
    };
    auto res = attempt();
    // A remembered machine that would not connect: find it again by its
    // advert and connect to what that advert names, once.
    if (!res.link && !device_.advertising) {
        std::cerr << kTag << "the remembered " << device_.name
                  << " did not connect; looking for it by its advert" << std::endl;
        session_.onDisconnect();
        rediscover_ = true;
        if (!scan(kScanMs)) return false;
        res = attempt();
    }
    if (!res.link) {
        switch (res.error) {
        case ble::LinkError::Resolve: note("services not resolved in time"); break;
        case ble::LinkError::NoCharacteristics:
            note("the machine's characteristics were not found" + (res.detail.empty() ? "" : " (" + res.detail + ")"));
            break;
        case ble::LinkError::Notify: note("notifications could not be enabled: " + res.detail); break;
        default: note("connect failed"); break;
        }
        dropLink();
        return false;
    }
    link_ = std::move(res.link);
    const uint16_t mtu = link_->mtu();
    write_chunk_ = mtu > 23 ? mtu - 3 : 20;
    std::cout << kTag << "connected to " << device_.name << std::endl;
    return true;
}

void AirSense11BleClient::dropLink() {
    session_.onDisconnect();
    if (link_) link_->close();
    link_.reset();
}

bool AirSense11BleClient::writeFrame(const std::vector<uint8_t>& frame) {
    if (!link_) return false;
    // Write with response, piece by piece at the MTU, as the machine was
    // proven with: each piece is acknowledged before the next goes. A piece
    // the machine never acknowledges fails the write after the transport's
    // call timeout, and with it the pull, instead of holding the pull.
    for (size_t at = 0; at < frame.size(); at += write_chunk_) {
        const size_t end = std::min(at + write_chunk_, frame.size());
        const std::vector<uint8_t> piece(frame.begin() + static_cast<long>(at),
                                         frame.begin() + static_cast<long>(end));
        const auto w = link_->write(piece, true);
        if (w.ok) continue;
        if (w.timed_out)
            note("write to the machine timed out: the link is dead, the pull is abandoned");
        else
            note("write to the machine failed: " + w.error);
        session_.onDisconnect();
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
    // The settings, for the STR.edf the Summary is laid into: not needed for
    // the Summary itself, so a machine that will not give them costs nothing.
    if (r == as11::Result::Ok && writer_ && !serial.empty()) {
        std::string settings;
        if (session_.getSettings(&settings) == as11::Result::Ok) {
            if (writeSettings(writer_->rootFor(serial), settings))
                std::cout << kTag << "settings kept as SETTINGS/CurrentSettings.json" << std::endl;
        } else {
            std::cout << kTag << "settings not read: " << session_.lastError() << std::endl;
        }
    }
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
    std::cout << kTag << "key exchange open with " << device_.name
              << ": the machine shows its code now" << std::endl;
    return r;
}

as11::Result AirSense11BleClient::pairConfirm(const std::string& code) {
    std::lock_guard<std::mutex> lk(op_mtx_);
    as11::Credentials creds;
    as11::Result r = session_.pairConfirm(code, &creds);
    dropLink();
    // The pairing is over, whichever way: the object the OS cached for it is
    // dropped, and the first pull finds the machine by its advertisement
    // under its own name. A stale object under a generic name once took the
    // connect and then swallowed the pull.
    machine_name_ = device_.name;
    rediscover_ = true;
    if (have_device_ && transport_ && transport_->forget(device_))
        std::cout << kTag << "forgot the cached object for " << device_.name
                  << ": the next pull rediscovers the machine" << std::endl;
    have_device_ = false;
    if (r != as11::Result::Ok) {
        note(std::string("pairing failed: ") + session_.lastError());
        return r;
    }
    if (!as11::saveCredentials(as11::credentialsPath(data_dir_), creds)) {
        note("paired, but the pairing could not be saved");
        return as11::Result::Protocol;
    }
    std::cout << kTag << "paired with " << device_.name << std::endl;
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
    if (!transport_ || !transport_->available() || !writer_) return;
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
    std::cout << kTag << "streaming the night from " << device_.name << std::endl;
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
