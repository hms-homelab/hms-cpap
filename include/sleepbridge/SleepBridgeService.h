#pragma once
//
// The Sleep Bridge local stream, put together: the bank (BatchStore), one
// rebuild per bridge (RebuildWorker), and what the status page shows.
//
// The rebuild runs on its own thread, woken by the receiver when a batch is
// banked and run once the stream has been quiet for `quiet_s` (2 s), so a
// burst of writes is rebuilt once, not per batch, and at least every `most_s`
// (60 s) while writes keep coming. At start every bridge in
// the bank is rebuilt once, so a restart catches up with what arrived before.
//
// A bridge is online while a connection is open and it was heard within the
// last 90 s; the route closes a connection silent that long. A connection is
// the bridge's session from its own hello; a test hello (probe, portal_test)
// is answered and is never a session.
//
// Commands: queueCommand() sends one of the commands a bridge takes from a
// local hms-cpap (stream, reboot, logs, restate, config_reset) on the
// bridge's session, or keeps it until the next session's hello. The answer
// comes back as a result frame with the same req_id, on that session or a
// later one (a stream switch reconnects first); a log pull's text comes
// ahead of it as kind 4 frames. No command is refused while the machine
// records: that is the owner's call. reboot is never answered.
//
#include "sleepbridge/BatchStore.h"
#include "sleepbridge/Conversation.h"
#include "sleepbridge/RebuildWorker.h"
#include "sleepbridge/Rebuilder.h"

#include <json/json.h>

#include <condition_variable>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hms_cpap::sleepbridge {

inline constexpr int kDefaultPort  = 8894;
inline constexpr int kSilenceSec   = 90;
inline constexpr const char* kRoute = "/sleepbridge/v1/device";

/// The commands a bridge takes from a local hms-cpap.
inline const std::vector<std::string>& localCommands() {
    static const std::vector<std::string> k = {"stream", "reboot", "logs", "restate", "config_reset"};
    return k;
}

inline constexpr size_t kCommandsKept = 20;           // per bridge, newest kept
inline constexpr size_t kCommandLogMax = 256 * 1024;  // a log pull's text, kept up to this

/// What a stream command's url may be: "" (the cloud), or an IP address or
/// hostname with nothing else; the port is the command's own `port` field
/// (8894 when left out). A bridge connects to
/// ws://<host>:<port>/sleepbridge/v1/device.
bool validBridgeHost(const std::string& host);
/// 1 to 65535.
bool validBridgePort(int port);

/// Sends one text frame on a connection; false when it is gone.
using Outlet = std::function<bool(const std::string& text)>;

/// queueCommand's answer: an HTTP status and the JSON body.
struct CommandAnswer {
    int         status{200};
    Json::Value body{Json::objectValue};
};

class SleepBridgeService {
public:
    struct Options {
        std::string root;        // <data_dir>/sleepbridge
        int         port{kDefaultPort};
        double      quiet_s{2.0};
        double      most_s{60.0};   // a busy stream is rebuilt at least this often
    };

    /// `factory` empty (or answering nullptr): this build has no rebuild, and
    /// available() is false.
    SleepBridgeService(Options opt, RebuilderFactory factory);
    ~SleepBridgeService();

    bool available() const { return available_; }
    const Options& options() const { return opt_; }
    BatchStore& store() { return store_; }

    void start();
    void stop();

    /// A connection was admitted: the conversation it talks to. `out` sends
    /// a frame on it (commands); empty for a connection nothing is sent on.
    std::unique_ptr<Conversation> open(const Admission& who, Outlet out = {});
    /// What a frame meant, beyond the frames sent back. Call it after the
    /// frames in fx.texts were sent: a hello's ack goes ahead of any command.
    void apply(const std::string& sender, const Effects& fx);
    void closed(const Conversation& conv);

    /// One command to `sender`: {"cmd": name, ...its arguments} ("url" for
    /// stream, an optional "n" for logs). 400 for a command or argument the
    /// bridge does not take, 404 for a bridge never seen; otherwise 200 with
    /// the req_id and whether it was sent now or waits for the bridge.
    CommandAnswer queueCommand(const std::string& sender, const Json::Value& request);

    /// Rebuild `sender` now, on the caller's thread (tests, and the start).
    StepReport rebuildNow(const std::string& sender);

    Json::Value status() const;

private:
    struct Command {
        uint64_t    req_id{0};
        std::string cmd;
        Json::Value frame;              // what goes down, req_id included
        std::string state{"queued"};    // queued, sent, answered
        std::time_t queued_at{0};
        std::time_t sent_at{0};
        std::time_t answered_at{0};
        Json::Value result{Json::nullValue};
        std::string logs;               // a log pull's kind 4 text
        bool        logs_cut{false};    // longer than kCommandLogMax
    };

    struct Bridge {
        std::string machine;        // X-Coyote-Machine, the bridge's own label
        std::string card_machine;   // the rebuild's: "resmed", "luna", "" unknown
        std::string chip_id;
        int         sessions{0};
        uint64_t    live{0};        // the connection of its latest session
        std::time_t first_seen{0};
        std::time_t last_heard{0};
        std::time_t last_batch{0};
        Json::Value state{Json::objectValue};
        StepReport  last_step;
        bool        stepped{false};
        std::vector<Command> commands;   // oldest first
    };

    struct Conn {
        Admission who;
        Outlet    out;
        bool      session{false};   // its own hello came: not a probe
    };

    void loop();
    void wake(const std::string& sender);
    void startSession(const std::string& sender, uint64_t conn);
    /// Sends every queued command of `sender` on its live session.
    void flush(const std::string& sender);
    static Json::Value commandJson(const Command& c);

    Options          opt_;
    RebuilderFactory factory_;
    bool             available_{false};
    BatchStore       store_;

    mutable std::mutex mu_;   // bridges_, conns_, next_conn_, next_req_
    std::map<std::string, Bridge> bridges_;
    std::map<uint64_t, Conn>      conns_;
    uint64_t                      next_conn_{1};
    // Seeded from the clock, so a late result for a command sent before a
    // restart does not land on a new one. Below 2^31: a log frame carries
    // the req_id in 32 bits.
    uint64_t                      next_req_{0};

    std::mutex rebuild_mu_;   // workers_: one rebuild at a time
    std::map<std::string, std::unique_ptr<RebuildWorker>> workers_;

    std::mutex              q_mu_;
    std::condition_variable q_cv_;
    std::set<std::string>   dirty_;
    uint64_t                events_{0};
    bool                    stop_{false};
    std::thread             thread_;
};

}  // namespace hms_cpap::sleepbridge
