#include "clients/As11Session.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hms_cpap::as11 {

const char* resultText(Result r) {
    switch (r) {
    case Result::Ok:        return "ok";
    case Result::WaitCode:  return "waiting for the code on the machine's screen";
    case Result::NotPaired: return "not paired";
    case Result::Connect:   return "connect failed";
    case Result::RadioOff:  return "Bluetooth is switched off on this computer (rf-kill)";
    case Result::Verify:    return "machine refused the code or the pairing";
    case Result::Protocol:  return "no answer or a bad one";
    case Result::Spool:     return "fragment lost or stalled";
    }
    return "?";
}

// ── credentials ──

std::string credentialsPath(const std::string& data_dir) {
    return (std::filesystem::path(data_dir) / "as11.json").string();
}

std::optional<Credentials> loadCredentials(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    const auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    const auto cid = j.find("clientId");
    const auto key = j.find("masterPairKey");
    if (cid == j.end() || key == j.end() || !cid->is_string() || !key->is_string())
        return std::nullopt;
    auto raw = hexDecode(key->get<std::string>());
    if (!raw || raw->size() != 32) return std::nullopt;
    Credentials c;
    c.clientId = cid->get<std::string>();
    std::copy(raw->begin(), raw->end(), c.masterKey.begin());
    OPENSSL_cleanse(raw->data(), raw->size());
    if (!c.valid()) return std::nullopt;
    return c;
}

bool saveCredentials(const std::string& path, const Credentials& c) {
    if (!c.valid()) return false;
    nlohmann::ordered_json j;
    j["clientId"] = c.clientId;
    j["masterPairKey"] = hexEncode(c.masterKey.data(), c.masterKey.size());
    std::string text = j.dump(2) + "\n";
    const std::string tmp = path + ".tmp";
    bool ok = false;
#ifndef _WIN32
    // Created 0600 from the first byte, not chmod'ed after: the key is never
    // readable by anyone else, not even for an instant.
    ::unlink(tmp.c_str());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0600);
    if (fd >= 0) {
        ok = ::fchmod(fd, 0600) == 0;
        size_t done = 0;
        while (ok && done < text.size()) {
            const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
            if (n <= 0) ok = false;
            else done += static_cast<size_t>(n);
        }
        ok = ::fsync(fd) == 0 && ok;
        ok = ::close(fd) == 0 && ok;
    }
#else
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        ok = static_cast<bool>(out);
    }
#endif
    OPENSSL_cleanse(text.data(), text.size());
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    if (!ok || ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool clearCredentials(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}

bool advertIsAirSense(const std::string& name, const std::vector<std::string>& uuids) {
    for (const auto& u : uuids) {
        std::string lower = u;
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower == kServiceUuid) return true;
    }
    return name.rfind("ResMed ", 0) == 0;
}

// ── the session ──

Session::Session(Writer writer, Timeouts timeouts, Random random)
    : writer_(std::move(writer)), timeouts_(timeouts), random_(std::move(random)) {
    if (!random_) {
        random_ = [](uint8_t* out, size_t n) {
            if (RAND_bytes(out, static_cast<int>(n)) != 1) throw std::runtime_error("RAND_bytes");
        };
    }
}

Session::~Session() {
    std::lock_guard<std::mutex> lk(m_);
    wipePairing();
}

void Session::onConnected() {
    std::lock_guard<std::mutex> lk(m_);
    connected_ = true;
    reasm_.reset();
    cipher_.reset();
    next_id_ = 2;
    pending_id_ = -1;
    reply_.reset();
}

void Session::onDisconnect() {
    std::lock_guard<std::mutex> lk(m_);
    connected_ = false;
    cipher_.reset();
    cv_.notify_all();
}

bool Session::encrypted() const {
    std::lock_guard<std::mutex> lk(m_);
    return cipher_.has_value();
}

bool Session::connected() const {
    std::lock_guard<std::mutex> lk(m_);
    return connected_;
}

bool Session::pairPending() const {
    std::lock_guard<std::mutex> lk(m_);
    return pair_pending_;
}

std::string Session::lastError() const {
    std::lock_guard<std::mutex> lk(m_);
    return last_error_;
}

std::string Session::lastSpoolStatus() const {
    std::lock_guard<std::mutex> lk(m_);
    return spool_status_;
}

void Session::fail(const std::string& why) {
    std::lock_guard<std::mutex> lk(m_);
    last_error_ = why;
}

void Session::wipePairing() {
    pair_pending_ = false;
    srp_ = Srp{};
    OPENSSL_cleanse(server_pk_.data(), server_pk_.size());
    OPENSSL_cleanse(salt_.data(), salt_.size());
    server_pk_.clear();
    salt_.clear();
}

// ── notifications ──

void Session::onNotify(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(m_);
    reasm_.feed(data, len);
    while (auto f = reasm_.next()) onFrame(*f);
}

void Session::onFrame(const Frame& f) {
    if (f.type & kEncryptedBit) {
        if (!cipher_) return;   // encrypted before a session: not ours
        auto text = cipher_->decrypt(f.payload);
        if (!text) return;      // undecodable
        onMessage(*text);
    } else {
        onMessage(std::string(f.payload.begin(), f.payload.end()));
    }
}

void Session::onMessage(const std::string& s) {
    auto m = json::parse(s, nullptr, false);
    if (m.is_discarded() || !m.is_object()) return;
    const auto id = m.find("id");
    const auto method = m.find("method");
    const bool has_method = method != m.end() && method->is_string();
    if (has_method && id == m.end()) {
        // A notification; HeartBeat needs no answer.
        const auto params = m.find("params");
        if (params == m.end() || !params->is_object()) return;
        const std::string name = method->get<std::string>();
        if (name == "SpoolFragment") onSpoolFragment(*params);
        else if (name == "StreamData") { if (handlers_.streamData) handlers_.streamData(*params); }
        else if (name == "EventNotification") onEventNotification(*params);
        return;
    }
    if (id != m.end() && id->is_number() && !has_method && pending_id_ >= 0 &&
        static_cast<int>(id->get<double>()) == pending_id_) {
        reply_ = std::move(m);
        cv_.notify_all();
    }
}

void Session::onSpoolFragment(const json& p) {
    if (spool_state_ != SpoolState::Running || spool_id_ < 0) return;
    const auto jid = p.find("spoolId");
    if (jid == p.end() || !jid->is_number() || static_cast<int>(jid->get<double>()) != spool_id_)
        return;
    const auto jseq = p.find("seq");
    const int seq = (jseq != p.end() && jseq->is_number()) ? static_cast<int>(jseq->get<double>()) : 0;
    const auto jst = p.find("status");
    const std::string status = (jst != p.end() && jst->is_string()) ? jst->get<std::string>() : "";

    if (seq != spool_seq_) {
        spool_error_ = "a fragment was lost (got " + std::to_string(seq) + ", expected " +
                       std::to_string(spool_seq_) + ")";
        spool_state_ = SpoolState::Failed;
        cv_.notify_all();
        return;
    }
    spool_seq_++;
    const auto jdata = p.find("data");
    if (jdata != p.end() && jdata->is_string() && !jdata->get_ref<const std::string&>().empty()) {
        auto bytes = base64Decode(jdata->get_ref<const std::string&>());
        // A fragment never decodes past maxFragmentSize (the machine's own cap).
        if (!bytes || bytes->size() > static_cast<size_t>(kFragmentSize) + 16) {
            spool_error_ = "fragment " + std::to_string(seq) + " does not decode";
            spool_state_ = SpoolState::Failed;
            cv_.notify_all();
            return;
        }
        spool_buf_.insert(spool_buf_.end(), bytes->begin(), bytes->end());
    }
    if (status.rfind("SPOOL_COMPLETE", 0) == 0 || status == "ERROR_DATA_UNAVAILABLE") {
        spool_status_ = status;
        spool_state_ = SpoolState::Done;
    }
    cv_.notify_all();
}

// EventNotification {"dataId": "FGState", "events": [{"value": "Therapy"}]}, or
// "TherapyEvents-RespiratoryEvents" with {event, reportTime, durationSeconds,
// backdateSeconds} per event: the machine's own scoring, live.
void Session::onEventNotification(const json& p) {
    const auto id = p.find("dataId");
    const auto evs = p.find("events");
    if (id == p.end() || !id->is_string() || evs == p.end() || !evs->is_array()) return;
    const std::string data_id = id->get<std::string>();
    if (data_id == "TherapyEvents-RespiratoryEvents") {
        if (!handlers_.respEvent) return;
        for (const auto& e : *evs) {
            if (!e.is_object()) continue;
            RespEvent r;
            if (auto v = e.find("event"); v != e.end() && v->is_string()) r.event = v->get<std::string>();
            if (auto v = e.find("reportTime"); v != e.end() && v->is_string()) r.reportTime = v->get<std::string>();
            if (auto v = e.find("durationSeconds"); v != e.end() && v->is_number()) r.durationSeconds = v->get<int>();
            if (auto v = e.find("backdateSeconds"); v != e.end() && v->is_number()) r.backdateSeconds = v->get<int>();
            if (r.event.empty() || r.reportTime.empty()) continue;
            handlers_.respEvent(r);
        }
        return;
    }
    if (data_id != "FGState" || !handlers_.fgState) return;
    for (const auto& e : *evs) {
        if (!e.is_object()) continue;
        const auto v = e.find("value");
        if (v != e.end() && v->is_string()) handlers_.fgState(v->get<std::string>() == "Therapy");
    }
}

void Session::setStreamHandlers(StreamHandlers h) {
    std::lock_guard<std::mutex> lk(m_);
    handlers_ = std::move(h);
}

// ── JSON-RPC ──

std::optional<nlohmann::ordered_json> Session::call(int id, const std::string& method, const ojson& params,
                                            const char* version, bool encrypted, int* err) {
    *err = 0;
    ojson m;
    m["id"] = id;
    m["jsonrpc"] = version;
    m["method"] = method;
    if (!params.is_null()) m["params"] = params;
    std::string text = m.dump();

    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!connected_) return std::nullopt;
        if (encrypted) {
            if (!cipher_) return std::nullopt;
            std::array<uint8_t, 16> iv{};
            random_(iv.data(), iv.size());
            const auto enc = cipher_->encrypt(iv, text);
            if (!enc.empty()) frame = buildFrame(kAppEncrypted, enc);
        } else {
            frame = buildFrame(kAppPlain, std::vector<uint8_t>(text.begin(), text.end()));
        }
        OPENSSL_cleanse(text.data(), text.size());
        if (frame.empty()) return std::nullopt;
        pending_id_ = id;
        reply_.reset();
    }

    // Outside the lock: a transport may deliver the reply from inside the write.
    const bool wrote = writer_(frame);

    std::unique_lock<std::mutex> lk(m_);
    if (wrote) {
        cv_.wait_for(lk, std::chrono::milliseconds(timeouts_.call_ms),
                     [&] { return reply_.has_value() || !connected_; });
    }
    pending_id_ = -1;
    if (!reply_) return std::nullopt;
    json r = std::move(*reply_);
    reply_.reset();

    const auto e = r.find("error");
    if (e != r.end() && !e->is_null()) {
        const auto code = e->is_object() ? e->find("code") : e->end();
        *err = (e->is_object() && code != e->end() && code->is_number())
                   ? static_cast<int>(code->get<double>())
                   : -1;
        return std::nullopt;
    }
    const auto res = r.find("result");
    if (res == r.end()) return std::nullopt;
    return *res;
}

namespace {
std::optional<std::string> str(const nlohmann::ordered_json& obj, const char* key) {
    if (!obj.is_object()) return std::nullopt;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}
} // namespace

// ── pairing ──

Result Session::pairStart() {
    {
        std::lock_guard<std::mutex> lk(m_);
        wipePairing();
        last_error_.clear();
    }
    std::array<uint8_t, 32> a{};
    random_(a.data(), a.size());
    std::string clientPk;
    {
        std::lock_guard<std::mutex> lk(m_);
        const bool started = srp_.start(a);
        OPENSSL_cleanse(a.data(), a.size());
        if (!started) {
            last_error_ = "srp failed";
            return Result::Protocol;
        }
        clientPk = srp_.clientPublicHex();
    }

    ojson params;
    params["clientPk"] = clientPk;
    int ec = 0;
    auto res = call(1, "StartKeyExchange", params, "2.0", false, &ec);
    auto server_pk = res ? str(*res, "serverPk") : std::nullopt;
    auto salt = res ? str(*res, "salt") : std::nullopt;

    std::lock_guard<std::mutex> lk(m_);
    if (!server_pk || !salt) {
        wipePairing();
        if (!connected_ && !res) {
            last_error_ = "the machine disconnected during the key exchange";
            return Result::Connect;
        }
        last_error_ = ec ? "machine refused the key exchange" : "key exchange failed";
        return ec ? Result::Verify : Result::Protocol;
    }
    server_pk_ = *server_pk;
    salt_ = *salt;
    pair_pending_ = true;
    return Result::WaitCode;
}

void Session::pairCancel() {
    std::lock_guard<std::mutex> lk(m_);
    wipePairing();
}

Result Session::pairConfirm(const std::string& code, Credentials* out) {
    std::string m1;
    {
        std::lock_guard<std::mutex> lk(m_);
        last_error_.clear();
        if (!pair_pending_) {
            last_error_ = "no key exchange open: start pairing first";
            return Result::Protocol;
        }
        if (code.empty()) {
            wipePairing();
            last_error_ = "no code";
            return Result::Protocol;
        }
        if (!connected_) {
            wipePairing();
            last_error_ = "the machine disconnected before the code came";
            return Result::Connect;
        }
        auto r = srp_.challenge(server_pk_, salt_, code);
        if (!r) {
            wipePairing();
            last_error_ = "key exchange failed";
            return Result::Protocol;
        }
        m1 = *r;
    }

    ojson params;
    params["clientConfirmation"] = m1;
    int ec = 0;
    auto res = call(1, "ConfirmKeyExchange", params, "2.0", false, &ec);

    std::lock_guard<std::mutex> lk(m_);
    Result r = Result::Ok;
    const auto client_id = res ? str(*res, "clientId") : std::nullopt;
    const auto m2 = res ? str(*res, "serverConfirmation") : std::nullopt;
    const auto nonce = res ? str(*res, "nonce") : std::nullopt;
    if (!res) {
        if (!connected_ && !ec) {
            last_error_ = "the machine disconnected before it confirmed";
            r = Result::Connect;
        } else {
            last_error_ = ec ? "machine refused the code" : "no answer to the confirmation";
            r = ec ? Result::Verify : Result::Protocol;
        }
    } else if (!client_id || !m2 || !nonce || client_id->empty() || client_id->size() > 32) {
        last_error_ = "confirmation reply malformed";
        r = Result::Protocol;
    } else if (!srp_.verify(*m2)) {
        last_error_ = "machine's proof did not verify";
        r = Result::Verify;
    } else if (out) {
        out->clientId = *client_id;
        out->masterKey = srp_.masterKey();
    }
    wipePairing();
    return r;
}

// ── the session with saved credentials ──

Result Session::open(const Credentials& creds) {
    fail("");
    if (!creds.valid()) {
        fail("not paired with the machine yet");
        return Result::NotPaired;
    }
    ojson params;
    params["clientId"] = creds.clientId;
    int ec = 0;
    auto res = call(1, "RequestSession", params, "2.0", false, &ec);
    const auto challenge = res ? str(*res, "challenge") : std::nullopt;
    const auto nonce_hex = res ? str(*res, "nonce") : std::nullopt;
    std::optional<std::string> response;
    std::optional<std::vector<uint8_t>> nonce;
    if (challenge && nonce_hex) {
        response = challengeResponse(creds.masterKey, *challenge);
        nonce = hexDecode(*nonce_hex);
        if (nonce && nonce->size() > 64) nonce.reset();
    }
    if (!response || !nonce) {
        if (ec == kVerificationFailure) {
            fail("machine no longer knows this pairing");
            return Result::Verify;
        }
        fail(ec ? "machine refused the session" : "no answer to the session request");
        return ec ? Result::Verify : Result::Protocol;
    }

    params = ojson::object();
    params["response"] = *response;
    res = call(1, "CheckSessionIntegrity", params, "2.0", false, &ec);
    const bool ok = res && res->is_object() && res->contains("confirmation") &&
                    (*res)["confirmation"].is_boolean() && (*res)["confirmation"].get<bool>();
    if (!ok) {
        fail(res ? "session integrity refused" : "no answer to the integrity check");
        return res ? Result::Verify : Result::Protocol;
    }
    std::lock_guard<std::mutex> lk(m_);
    cipher_.emplace(creds.masterKey, *nonce);
    next_id_ = 2;
    return Result::Ok;
}

Result Session::pullSpool(const std::string& name, const std::string& from,
                          std::vector<uint8_t>* out) {
    if (out) out->clear();
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        last_error_.clear();
        spool_status_.clear();
        if (!cipher_) {
            last_error_ = "no session open";
            return Result::Protocol;
        }
        id = next_id_++;
    }
    ojson addr;
    addr[name]["fromDateTime"] = from.substr(0, 10) + "T00:00:00.000Z";
    ojson params;
    params["maxSpoolSize"] = kMaxSpoolSize;
    params["spoolAddress"] = addr;
    int ec = 0;
    auto res = call(id, "StartSpool", params, "1.0", true, &ec);
    int spool_id = -1;
    if (res && res->is_object() && res->contains("spoolId") && (*res)["spoolId"].is_number())
        spool_id = static_cast<int>((*res)["spoolId"].get<double>());
    if (spool_id < 0) {
        fail(ec ? "machine refused the spool" : "no answer to StartSpool");
        return Result::Protocol;
    }

    {
        // Armed before the pull: fragments may follow its reply at once.
        std::lock_guard<std::mutex> lk(m_);
        spool_id_ = spool_id;
        spool_seq_ = 0;
        spool_buf_.clear();
        spool_error_.clear();
        spool_state_ = SpoolState::Running;
        id = next_id_++;
    }
    params = ojson::object();
    params["maxFragmentSize"] = kFragmentSize;
    params["maxNotifications"] = 0;
    params["spoolId"] = spool_id;
    res = call(id, "PullSpoolFragments", params, "1.0", true, &ec);

    std::unique_lock<std::mutex> lk(m_);
    Result r = Result::Ok;
    if (!res) {
        last_error_ = "machine refused the pull";
        r = Result::Protocol;
    } else {
        // Fragments come back to back; silence means a lost one.
        for (;;) {
            const int before = spool_seq_;
            cv_.wait_for(lk, std::chrono::milliseconds(timeouts_.frag_idle_ms), [&] {
                return spool_state_ != SpoolState::Running || !connected_ || spool_seq_ != before;
            });
            if (spool_state_ == SpoolState::Done) break;
            if (spool_state_ == SpoolState::Failed) {
                last_error_ = spool_error_;
                r = Result::Spool;
                break;
            }
            if (!connected_) {
                last_error_ = "machine disconnected mid-spool";
                r = Result::Spool;
                break;
            }
            if (spool_seq_ == before) {
                last_error_ = "spool stalled";
                r = Result::Spool;
                break;
            }
        }
    }
    if (r == Result::Ok && out) *out = std::move(spool_buf_);
    spool_buf_.clear();
    spool_id_ = -1;
    spool_state_ = SpoolState::Idle;
    return r;
}

// ── the live night ──

Result Session::getState(std::string* serial, bool* therapy) {
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        last_error_.clear();
        if (!cipher_) { last_error_ = "no session open"; return Result::Protocol; }
        id = next_id_++;
    }
    int ec = 0;
    auto res = call(id, "Get", ojson::array({"SerialNumber", "FGState"}), "1.0", true, &ec);
    if (!res || !res->is_object()) {
        fail(ec ? "machine refused the state request" : "no answer to Get");
        return Result::Protocol;
    }
    if (serial) *serial = str(*res, "SerialNumber").value_or("");
    if (therapy) *therapy = str(*res, "FGState").value_or("") == "Therapy";
    return Result::Ok;
}

Result Session::subscribe() {
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        last_error_.clear();
        if (!cipher_) { last_error_ = "no session open"; return Result::Protocol; }
        id = next_id_++;
    }
    ojson params;
    params["dataIds"] = ojson::array({"FGState", "TherapyEvents-RespiratoryEvents"});
    int ec = 0;
    if (!call(id, "SubscribeEvent", params, "1.0", true, &ec)) {
        fail(ec ? "machine refused the subscription" : "no answer to SubscribeEvent");
        return Result::Protocol;
    }
    return Result::Ok;
}

Result Session::startStream() {
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        last_error_.clear();
        if (!cipher_) { last_error_ = "no session open"; return Result::Protocol; }
        id = next_id_++;
    }
    ojson params;
    params["dataIds"] = streamIds();
    params["sampleIntervalMs"] = kIntervalMs;
    params["reportIntervalMs"] = 200;
    int ec = 0;
    if (!call(id, "StartStream", params, "1.0", true, &ec)) {
        fail(ec ? "machine refused the stream" : "no answer to StartStream");
        return Result::Protocol;
    }
    return Result::Ok;
}

void Session::stopStream() {
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!cipher_ || !connected_) return;
        id = next_id_++;
    }
    ojson params;
    params["dataIds"] = ojson::array();
    params["sampleIntervalMs"] = kIntervalMs;
    params["reportIntervalMs"] = 200;
    int ec = 0;
    const int saved = timeouts_.call_ms;
    timeouts_.call_ms = std::min(saved, 3000);   // the link may already be going
    call(id, "StartStream", params, "1.0", true, &ec);
    timeouts_.call_ms = saved;
}

Result Session::getIdentification(std::string* result_json) {
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        last_error_.clear();
        if (!cipher_) { last_error_ = "no session open"; return Result::Protocol; }
        id = next_id_++;
    }
    int ec = 0;
    auto res = call(id, "Get", ojson::array({"IdentificationProfiles"}), "1.0", true, &ec);
    if (!res || !res->is_object() || !res->contains("IdentificationProfiles")) {
        fail(ec ? "machine refused the identity" : "no answer to Get");
        return Result::Protocol;
    }
    if (result_json) *result_json = res->dump();
    return Result::Ok;
}

} // namespace hms_cpap::as11
