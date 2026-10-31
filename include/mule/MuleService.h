#pragma once
//
// The Mule & Miner local stream (docs/MULE_LOCAL_STREAM.md): a Mule & Miner
// in AirSense 11 Bluetooth mode, set to this host, streams the machine's
// night to ws://<host>:8894/mule/v1/device. This is that conversation, frames
// in and frames out, with no Drogon in it, so the unit suite drives it whole.
//
//   up    text   hello {state, fw, miner_fw, boot_reason, mode}
//                hello {"test": true, ...}: a test of this address, acked only
//                state {state}: every 60 s, and at once when the stream ends;
//                  state.as11 the stream's counters, state.tz the owner's zone
//                  (POSIX), state.resp_events the machine's scored events
//                result {req_id, ok, result, error}: a command's answer
//         binary 12-byte header (req_id u32, seq u32, flags u16, kind u16),
//                kind 1: one CDS1 stream block; kind 6: an as11_download's
//                bytes, ended by an empty frame with flags bit 0
//   down  text   ack {server_time, ping_s}, block_ack {start_ms},
//                error {error}, cmd {req_id, cmd, ...}
//
// A block is acked only once it is on disk: the unit frees it on block_ack.
// It is kept as <root>/<serial>/blocks/<start_ms>.cds1 and handed to the
// night writer, which lays it into <data_dir>/as11/<machine>/DATALOG/; a
// block sent again is acked again and kept once.
//
// When a night's stream has ended and every block the unit held has landed,
// the night's files are completed (the events included) and the machine's
// records are asked for, one command at a time: its Identification (when
// none is kept), its Summary and its settings history. The Summary makes
// STR.edf, from the model's layout.
//
// No authentication on the LAN: a unit is named by X-Device-Serial and
// nothing else.
//
#include "mule/LivePublisher.h"
#include "services/As11NightWriter.h"

#include <json/json.h>

#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::mule {

inline constexpr const char* kRoute        = "/mule/v1/device";
inline constexpr int         kPingSec      = 30;
inline constexpr int         kSilenceSec   = 90;
inline constexpr size_t      kHeaderSize   = 12;
inline constexpr uint16_t    kKindBlock    = 1;
inline constexpr uint16_t    kKindAs11     = 6;
inline constexpr size_t      kCommandsKept = 30;               // per unit, newest kept
inline constexpr size_t      kPullMax      = 4 * 1024 * 1024;  // an as11_download's bytes
inline constexpr std::time_t kInFlightSec  = 300;              // a command unanswered this long no longer blocks the next

/// The commands a unit takes from a local hms-cpap.
const std::vector<std::string>& localCommands();

/// X-Device-Serial: 1 to 64 of [A-Za-z0-9._-], not starting with '.'.
bool validSerial(const std::string& serial);

/// What config {"hms_url"} may carry: "" (the cloud), or an IP address or a
/// hostname of 1 to 63 letters, digits, '.' and '-', not starting or ending
/// with '.' or '-'. The unit builds ws://<host>:8894/mule/v1/device itself.
bool validUnitHost(const std::string& host);

/// The 12-byte binary header.
struct FrameHeader {
    uint32_t req_id{0};
    uint32_t seq{0};
    uint16_t flags{0};
    uint16_t kind{0};
};
std::optional<FrameHeader> parseHeader(const std::string& frame);
std::string encodeHeader(const FrameHeader& h);

/// Sends one text frame on a connection; false when it is gone.
using Outlet = std::function<bool(const std::string& text)>;

struct CommandAnswer {
    int         status{200};
    Json::Value body{Json::objectValue};
};

class MuleService {
public:
    struct Options {
        std::string root;       // <data_dir>/mule: what each unit sent, as it came
        std::string data_dir;   // the night writer's: <data_dir>/as11/<machine>/
        int         port{8894};
    };

    MuleService(Options opt, std::shared_ptr<IAs11NightSink> sink);

    /// The night as it happens, for the rest of hms-cpap. `night_changed`
    /// runs after every block stored and every event taken: the host wakes
    /// its sync with it, so the database, the charts and the realtime sensors
    /// follow the stream within a block instead of at the next interval.
    /// `publisher` puts each event and therapy on and off on MQTT. Either may
    /// be empty.
    void setLive(std::function<void()> night_changed, std::shared_ptr<LivePublisher> publisher);

    const Options& options() const { return opt_; }

    /// An admitted upgrade: its connection id. `out` sends on it; `peer_ip`
    /// is the socket's peer address, which is where the unit's own control
    /// server (http://<peer_ip>/) answers.
    uint64_t open(const std::string& serial, Outlet out, const std::string& peer_ip = {});

    /// What a firmware update needs to know about a unit, from its session.
    struct Brief {
        bool        known{false};
        bool        online{false};
        bool        night_open{false};   // streaming, or a night still landing
        std::string ip;                  // the peer of its live session
        std::string fw, miner_fw, machine;
        uint64_t    hellos{0};           // sessions begun: rises when the unit comes back
        std::time_t last_heard{0};
    };
    Brief brief(const std::string& serial) const;
    void onText(uint64_t conn, const std::string& frame);
    void onBinary(uint64_t conn, const std::string& frame);
    void closed(uint64_t conn);

    /// One command to a unit: {"cmd": name, ...its arguments}. 400 for a
    /// command or argument the unit does not take from here, 404 for a unit
    /// never seen; otherwise 200 with its req_id. Sent when nothing else is in
    /// flight on the unit's session, else queued; the answer shows in status().
    CommandAnswer queueCommand(const std::string& serial, const Json::Value& request);

    Json::Value status() const;

    std::string unitDir(const std::string& serial) const;
    std::string blocksDir(const std::string& serial) const;
    /// The machine's card root, where the night writer lays its night.
    std::string machineRoot(const std::string& machine) const;

private:
    struct Command {
        uint64_t    req_id{0};
        std::string cmd;
        Json::Value frame;               // what goes down, req_id included
        bool        automatic{false};    // asked by this host after a night
        std::string state{"queued"};     // queued, sent, answered
        uint64_t    conn{0};             // the session it went on
        std::time_t queued_at{0}, sent_at{0}, answered_at{0};
        Json::Value result{Json::nullValue};
        std::string bytes;               // an as11_download's kind 6 bytes
        bool        bytes_done{false};
        bool        bytes_cut{false};
        std::string outcome;             // what this host made of an as11_download
    };

    struct Unit {
        std::string machine;             // the AirSense 11's serial, from its blocks
        std::string tz;
        std::string clock_for;           // the machine the writer's clock was set for
        std::string summary_from{"2000-01-01"};
        bool        streaming{false};
        bool        end_pending{false};  // the stream ended; the night has not landed yet
        int         sessions{0};
        uint64_t    live{0};
        std::time_t first_seen{0}, last_heard{0}, last_block{0};
        uint64_t    blocks_stored{0}, blocks_resent{0}, events{0};
        std::string fw, miner_fw, mode, boot_reason;
        std::string ip;                  // the live session's peer
        uint64_t    hellos{0};
        Json::Value state{Json::objectValue};
        std::vector<Command> commands;   // oldest first
        std::vector<Json::Value> pending_events;   // before a block named the machine
        bool        loaded{false};
    };

    struct Conn {
        std::string serial;
        Outlet      out;
        std::string ip;
        bool        session{false};      // its own hello came: not a test
    };

    Unit& unit(const std::string& serial);   // mu_ held; loads unit.json once
    void  save(const std::string& serial, const Unit& u) const;   // mu_ held
    void  send(uint64_t conn, const std::string& text);
    void  hello(uint64_t conn, const std::string& serial, const Json::Value& frame);
    void  stateOf(const std::string& serial, const Json::Value& state);
    void  feedEvents(const std::string& serial, const Json::Value& events);
    void  block(uint64_t conn, const std::string& serial, const FrameHeader& h,
                const std::string& frame);
    void  pullBytes(const std::string& serial, const FrameHeader& h, const std::string& frame);
    void  result(const std::string& serial, const Json::Value& frame);
    void  nightLanded(const std::string& serial);   // mu_ not held
    void  afterPull(const std::string& serial, Command& c);   // mu_ held
    uint64_t enqueue(Unit& u, const std::string& cmd, Json::Value frame, bool automatic);   // mu_ held
    /// Sends the oldest queued command when nothing is in flight.
    void  flush(const std::string& serial);
    void  applyClock(const std::string& serial, Unit& u);   // mu_ held
    static Json::Value commandJson(const Command& c);

    Options                         opt_;
    std::shared_ptr<IAs11NightSink> sink_;
    std::function<void()>           night_changed_;
    std::shared_ptr<LivePublisher>  live_;

    mutable std::mutex              mu_;
    std::map<std::string, Unit>     units_;
    std::map<uint64_t, Conn>        conns_;
    uint64_t                        next_conn_{1};
    uint64_t                        next_req_{0};
};

}  // namespace hms_cpap::mule
