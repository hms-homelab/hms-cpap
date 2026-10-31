#pragma once

#include "clients/As11Protocol.h"
#include "clients/As11Stream.h"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::as11 {

/**
 * The AirSense 11's conversation over its Bluetooth link (SDD-057), with no
 * Bluetooth in it: JSON-RPC requests matched to their replies, the two-step
 * pairing, the session open and the spool pull. The transport hands it the
 * bytes each notification carries (onNotify) and says when the link comes and
 * goes; the session hands the transport whole frames to write. So the test
 * binary drives every step with synthesized frames, and AirSense11BleClient
 * is only BlueZ plumbing around it.
 *
 * The steps, as the machine expects them:
 *
 *   pairing, first time, two calls because the code appears on the machine's
 *   screen only after the exchange starts:
 *     pairStart()       StartKeyExchange {clientPk}       -> {serverPk, salt}
 *     pairConfirm(code) ConfirmKeyExchange {clientConfirmation}
 *                                                         -> {clientId, serverConfirmation, nonce}
 *     The person has under a minute between the two.
 *
 *   every later connection:
 *     open(creds)       RequestSession {clientId}         -> {challenge, nonce}
 *                       CheckSessionIntegrity {response}  -> {confirmation: true}
 *     after which every frame both ways is encrypted.
 *
 *   pullSpool("Summary", from)
 *                       StartSpool {maxSpoolSize, spoolAddress}  -> {spoolId}
 *                       PullSpoolFragments {maxFragmentSize, maxNotifications, spoolId}
 *     then SpoolFragment notifications {spoolId, seq, data (base64), status},
 *     seq from 0 with no gap, until a status of SPOOL_COMPLETE or
 *     ERROR_DATA_UNAVAILABLE.
 *
 * One operation at a time; onNotify and onDisconnect may come from another
 * thread (the transport's event loop) while one runs.
 */

enum class Result {
    Ok,
    WaitCode,      // pairStart: the machine shows its code now
    NotPaired,     // no credentials: pair first
    Connect,       // the link went away (or never came)
    RadioOff,      // the host's own Bluetooth is off and rf-kill holds it off
    Verify,        // the machine refused the code or the credentials
    Protocol,      // no reply in time, or a reply that did not parse
    Spool,         // a fragment lost, undecodable, or the spool stalled
};

const char* resultText(Result r);

/// What pairing yields and every session needs. Secret: never logged, never in
/// config.json.
struct Credentials {
    std::string clientId;
    Key masterKey{};
    bool valid() const { return !clientId.empty(); }
};

/// <data_dir>/as11.json
std::string credentialsPath(const std::string& data_dir);
/// The saved pairing, or nullopt when there is none or it does not parse.
std::optional<Credentials> loadCredentials(const std::string& path);
/// Writes {clientId, masterPairKey} with mode 0600 (on POSIX), atomically.
bool saveCredentials(const std::string& path, const Credentials& c);
bool clearCredentials(const std::string& path);

/// True when a scanned device is an AirSense 11: it advertises the 0xFD56
/// service or a name "ResMed <serial tail>".
bool advertIsAirSense(const std::string& name, const std::vector<std::string>& uuids);

struct Timeouts {
    int call_ms = 10000;       // one request to its reply
    int frag_idle_ms = 8000;   // silence between spool fragments that means a lost one
};

class Session {
public:
    /// Writes one whole frame to the machine; the transport splits it to the
    /// link's MTU. False when the write failed.
    using Writer = std::function<bool(const std::vector<uint8_t>& frame)>;
    /// Fills n random bytes (the SRP secret, each request's IV).
    using Random = std::function<void(uint8_t* out, size_t n)>;

    static constexpr int kMaxSpoolSize = 65535;
    static constexpr int kFragmentSize = 2808;
    static constexpr int kVerificationFailure = -11005;

    explicit Session(Writer writer, Timeouts timeouts = {}, Random random = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ── the transport's side ──
    /// The link is up and notifications are on: a fresh conversation.
    void onConnected();
    /// The link dropped: whatever waits gives up now.
    void onDisconnect();
    /// The bytes of one notification (a frame, part of one, or several).
    void onNotify(const uint8_t* data, size_t len);
    void onNotify(const std::vector<uint8_t>& v) { onNotify(v.data(), v.size()); }

    // ── pairing ──
    /// StartKeyExchange. WaitCode when the machine answered: its screen shows
    /// the code now and the exchange stays open for pairConfirm. Any other
    /// result closes it.
    Result pairStart();
    bool pairPending() const;
    /// ConfirmKeyExchange with the code read off the machine. Ok fills *out;
    /// the exchange is closed whatever the result.
    Result pairConfirm(const std::string& code, Credentials* out);
    void pairCancel();

    // ── with saved credentials ──
    /// RequestSession and CheckSessionIntegrity; Ok means encrypted from here.
    Result open(const Credentials& creds);
    bool encrypted() const;
    /// The link is up (between onConnected and onDisconnect).
    bool connected() const;
    /// A spool by name ("Summary") from `from` ("YYYY-MM-DD", read from
    /// midnight UTC): its fragments joined, in *out.
    Result pullSpool(const std::string& name, const std::string& from, std::vector<uint8_t>* out);
    /// The status the last spool ended on (SPOOL_COMPLETE...,
    /// ERROR_DATA_UNAVAILABLE), empty when it did not end.
    std::string lastSpoolStatus() const;

    /// Why the last operation failed, in words fit for a log or the UI. Never
    /// holds a key, a code or the clientId.
    std::string lastError() const;

    // ── the live night (SDD-057 step 4), in an open session ──

    /// What the machine pushes once subscribed and streaming. Called on the
    /// transport's thread; a handler must not call back into the Session.
    struct StreamHandlers {
        std::function<void(const nlohmann::ordered_json& params)> streamData;   // StreamData
        std::function<void(bool therapy)> fgState;                              // FGState changed
        std::function<void(const RespEvent& e)> respEvent;                      // scored event
    };
    void setStreamHandlers(StreamHandlers h);

    /// Get ["SerialNumber","FGState"]: the machine's serial (it names the
    /// night's files) and whether it is in therapy now.
    Result getState(std::string* serial, bool* therapy);
    /// SubscribeEvent on FGState and the machine's scored respiratory events,
    /// in ONE call: each SubscribeEvent replaces the set.
    Result subscribe();
    /// StartStream with the channels the bridge was proven with, every 40 ms,
    /// reported every 200 ms.
    Result startStream();
    /// StartStream with no channels: the machine stops reporting.
    void stopStream();
    /// Get ["IdentificationProfiles"]: the result object as the machine sent
    /// it, key order kept, which is what a card's Identification.json holds
    /// under "FlowGenerator".
    Result getIdentification(std::string* result_json);

private:
    using json = nlohmann::ordered_json;
    using ojson = nlohmann::ordered_json;

    // One request and its reply's "result"; nullopt on no reply, a write
    // failure, a dropped link or an error reply (whose code goes in *err).
    std::optional<json> call(int id, const std::string& method, const ojson& params,
                             const char* version, bool encrypted, int* err);
    void onFrame(const Frame& f);          // m_ held
    void onMessage(const std::string& s);  // m_ held
    void onSpoolFragment(const json& p);   // m_ held
    void onEventNotification(const json& p);   // m_ held
    StreamHandlers handlers_;
    void fail(const std::string& why);
    void wipePairing();                    // m_ held

    Writer writer_;
    Timeouts timeouts_;
    Random random_;

    mutable std::mutex m_;
    std::condition_variable cv_;
    bool connected_ = false;
    Reassembler reasm_;
    std::optional<Cipher> cipher_;
    int next_id_ = 2;
    std::string last_error_;

    int pending_id_ = -1;
    std::optional<json> reply_;

    bool pair_pending_ = false;
    Srp srp_;
    std::string server_pk_, salt_;

    enum class SpoolState { Idle, Running, Done, Failed };
    SpoolState spool_state_ = SpoolState::Idle;
    int spool_id_ = -1;
    int spool_seq_ = 0;
    std::vector<uint8_t> spool_buf_;
    std::string spool_status_, spool_error_;
};

} // namespace hms_cpap::as11
