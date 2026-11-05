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
// local hms-cpap (stream, reboot, logs, restate, config_reset, config) on
// the bridge's session, or keeps it until the next session's hello. The
// answer comes back as a result frame with the same req_id, on that session
// or a later one (a stream switch reconnects first; a stream back to the
// cloud answers, then restarts); a log pull's text comes ahead of it as kind
// 4 frames. reboot is never answered. A command still queued can be
// cancelled; one already sent cannot.
//
// Recording: the machine records while sector writes land (a batch with
// records within night_quiet_s, 15 minutes). A bridge not heard from since
// this service started is "unknown". No command is refused while it records,
// that is the owner's call, but the ones that can cost the night (reboot,
// config_reset, stream, a config set carrying ssid or pass, an update) are
// warned: without "confirm": true they answer 409 with the warning and send
// nothing (devices/Recording.h).
//
// config (firmware 1.0.1 and later): {"get": true} or {"set": {...}}, passed
// down as the bridge's own frame; the bridge is the validator of record,
// this end checks only the shape. The WiFi password, the pairing secret and
// the token go into the frame sent to the bridge and nowhere else: the
// command kept here, its listing and the log carry "***", the frame is
// scrubbed once sent, and an answer's values never carry them back.
//
// Updates: startUpdate() sends a firmware image to a bridge on its session:
// an `ota` command naming the image's size and SHA-256, then the image as
// binary kind 8 frames of 4096 bytes. The bridge writes it as it comes,
// checks it, answers the command, and restarts into it only when the answer
// is ok; its next hello carries the version it runs, and only that hello
// ends the job (no clock: a bridge that does not come back stays
// "restarting"). The image is the owner's file, or the public release
// (startReleaseUpdate), downloaded and checked against the feed's size and
// SHA-256 before anything is sent. One update at a time. The job is the
// shape of devices/OtaJob.h, kept as one JSON file per bridge.
//
#include "devices/OtaJob.h"
#include "devices/Recording.h"
#include "sleepbridge/BatchStore.h"
#include "sleepbridge/Conversation.h"
#include "sleepbridge/RebuildWorker.h"
#include "sleepbridge/Rebuilder.h"
#include "sleepbridge/ReleaseFeed.h"

#include <json/json.h>

#include <condition_variable>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    static const std::vector<std::string> k = {"stream", "reboot", "logs", "restate", "config_reset", "config"};
    return k;
}

/// The keys a bridge's config never shows or keeps here: the WiFi password,
/// the pairing secret, the token.
inline bool secretConfigKey(const std::string& k) { return k == "pass" || k == "pair" || k == "jwt"; }

/// The first firmware that takes `config`.
inline constexpr const char* kConfigSince = "1.0.1";

/// What a config command may carry, checked before anything is kept: {"get":
/// true}, or {"set": {...}} of 1 to 16 keys, each a string of at most 64
/// characters, a boolean or an integer, and no stream_url or stream_port
/// (the stream command's). Empty when it is fine; else why not.
std::string configShapeError(const Json::Value& request);

/// Whether a config set carries the network (ssid or pass): it is tested by
/// the bridge, which pauses its uplink, so it is warned while recording.
bool configTouchesNetwork(const Json::Value& request);

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
/// Sends one binary frame (header and body) on a connection; false when gone.
using BinaryOutlet = std::function<bool(const std::string& bytes)>;

/// queueCommand's answer: an HTTP status and the JSON body.
struct CommandAnswer {
    int         status{200};
    Json::Value body{Json::objectValue};
};

/// What an ESP32 application image says about itself (its app descriptor).
struct ImageInfo {
    std::string project;   // "sd_capture" for a Sleep Bridge
    std::string version;   // e.g. "1.0.0"
    std::string built;     // "<date> <time>" as compiled in
};

inline constexpr const char* kBridgeProject = "sd_capture";
inline constexpr size_t      kImageMax      = 4 * 1024 * 1024;   // a slot is 3 MB

/// Reads the image's descriptor: the ESP32 image magic, then the app
/// descriptor's. nullopt with `why` when the bytes are not an application
/// image at all; the project is reported, not judged, so the caller can
/// refuse one that is not a Sleep Bridge's.
std::optional<ImageInfo> inspectImage(const std::string& image, std::string* why);

/// The command and the chunks an update sends, as frames: the `ota` text
/// frame first, then one kind 8 binary frame per 4096 bytes (the last
/// flagged). Pure, so the suite checks the wire without a bridge.
std::string otaCommandFrame(uint64_t req_id, size_t size, const std::string& sha256_hex);
std::string firmwareChunkFrame(uint64_t req_id, uint32_t seq, bool last, const std::string& body);

class SleepBridgeService {
public:
    struct Options {
        std::string root;        // <data_dir>/sleepbridge
        int         port{kDefaultPort};
        double      quiet_s{2.0};
        double      most_s{60.0};   // a busy stream is rebuilt at least this often
        // A night is open on a bridge while sector writes land: a batch with
        // records within this long. The machine writes through therapy and
        // for a minute or two after the mask comes off; 15 quiet minutes is
        // its downtime.
        double      night_quiet_s{15 * 60.0};
        // Where each bridge's update job is kept, one <sender>.json each, so
        // a restart reads it back. Empty: in memory only.
        std::string jobs_dir;
        // Downloads a release image (startReleaseUpdate). Empty: no release
        // updates.
        FeedGet     fetch;
        /// Called after a rebuild that wrote files, off the rebuild's locks:
        /// what landed in <root>/<sender>/ this step. The receiver's seam to
        /// the rest of hms-cpap (a sync of the card folder, the live night).
        std::function<void(const std::string& sender, const StepReport& step)> on_rebuilt;
        /// Called every `tick_s` while the service runs, on the rebuild
        /// thread, for whatever keeps time off the stream (a mask that went
        /// quiet). Empty: no ticks.
        std::function<void()> on_tick;
        double                tick_s{30.0};
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
    /// a text frame on it (commands), `bin` a binary one (an update's image);
    /// empty for a connection nothing is sent on.
    std::unique_ptr<Conversation> open(const Admission& who, Outlet out = {}, BinaryOutlet bin = {});
    /// What a frame meant, beyond the frames sent back. Call it after the
    /// frames in fx.texts were sent: a hello's ack goes ahead of any command.
    void apply(const std::string& sender, const Effects& fx);
    void closed(const Conversation& conv);

    /// One command to `sender`: {"cmd": name, ...its arguments} ("url" for
    /// stream, an optional "n" for logs, "get" or "set" for config), and
    /// "confirm": true once the person saw the recording warning. 400 for a
    /// command or argument the bridge does not take, 404 for a bridge never
    /// seen, 409 for config below firmware 1.0.1, 409 with the warning for a
    /// warned command while the machine records and no confirm; otherwise
    /// 200 with the req_id and whether it was sent now or waits for the
    /// bridge.
    CommandAnswer queueCommand(const std::string& sender, const Json::Value& request);

    /// Drops a command still queued (never sent). 404 for a req_id not
    /// listed, 409 for one already sent.
    CommandAnswer cancelCommand(uint64_t req_id);

    /// Stops listing a bridge that is away: its row here, its queued
    /// commands and its update job go; its batches and files stay. 409 while
    /// it is connected. It comes back as new on its next hello.
    CommandAnswer forget(const std::string& sender);

    /// Whether the machine behind `sender` is recording now (D5's window).
    devices::Recording recording(const std::string& sender) const;

    /// The release feed this service reads on each hello and lists for each
    /// bridge in status(); null: none. It outlives the service.
    void setReleaseFeed(ReleaseFeed* feed) { feed_ = feed; }

    /// Rebuild `sender` now, on the caller's thread (tests, and the start).
    StepReport rebuildNow(const std::string& sender);

    /// A card learned on a computer (SDD-061): the CBL1 payload of a
    /// .cpapcard. 400 when it does not pass the content check (the body names
    /// the check); otherwise it is kept as the install's latest learn and
    /// given to every bridge banked here except one that read another layout
    /// from its own card (D5), each rebuilt from scratch so it goes first.
    /// 201 with what was kept and, per bridge, whether it took it and why not.
    /// A bridge first seen later takes it at its first rebuild, unless
    /// another bridge already holds that card.
    CommandAnswer learnCard(const std::string& cbl1, const std::string& file_name = {});

    /// A night is open on `sender`: sector writes landed within night_quiet_s.
    bool nightOpen(const std::string& sender) const;

    /// One firmware image to `sender`, on its session, now. 400 when the
    /// bytes are not a Sleep Bridge image, 404 for a bridge never seen, 409
    /// while it is offline or an update runs, 409 with the recording warning
    /// while the machine records and `confirm` is false; otherwise 200 with
    /// the req_id and the job as it starts. The frames go on a thread of
    /// their own; the job is in status(), update() and ota().
    CommandAnswer startUpdate(const std::string& sender, const std::string& image, bool confirm = false,
                              const std::string& file_name = {});
    /// The same from the public release for the bridge's board: 409 when
    /// the feed has no newer release for it (the reason says why: not
    /// published yet, no board stated, already current). The job starts in
    /// "preparing": the image is downloaded and checked against the feed's
    /// size and SHA-256 first, and nothing is sent when it does not match.
    CommandAnswer startReleaseUpdate(const std::string& sender, bool confirm = false);
    /// The last update job of `sender`, in the shape scripts read before the
    /// job object (kept one release), or null.
    Json::Value update(const std::string& sender) const;
    /// The last update job of `sender` as devices/OtaJob.h's object, or null.
    Json::Value ota(const std::string& sender) const;
    /// Returns once the running update's frames have all been handed to the
    /// connection (or it failed on the way). Tests, and stop().
    void waitForUpdateSend();

    Json::Value status() const;

private:
    struct UpdateJob {
        devices::OtaJob job;            // what every page reads
        uint64_t    req_id{0};
        ImageInfo   image;
        std::string sha256;
        std::time_t sent_at{0};
        std::time_t answered_at{0};
        Json::Value result{Json::nullValue};
    };
    static Json::Value updateJson(const UpdateJob& j);
    /// Under mu_: a phase change, stamped and written to the job's file.
    void jobChanged(const std::string& sender, UpdateJob& j);
    /// Under mu_: the refusals every update start shares; empty answer: none.
    std::optional<CommandAnswer> updateRefusal(const std::string& sender, bool confirm, bool* recording_now);
    UpdateJob newJob(const std::string& sender, const std::string& source, const std::string& target_version,
                     size_t total, bool confirmed_while_recording);
    void sendUpdate(const std::string& sender, std::string image);
    void fetchAndSend(const std::string& sender, FeedImage img);
    /// Under mu_: the job of `sender` ends failed.
    void failJob(const std::string& sender, uint64_t req_id, const std::string& reason, const std::string& detail);

    struct Command {
        uint64_t    req_id{0};
        std::string cmd;
        Json::Value frame;              // what goes down, req_id included; secrets until sent
        Json::Value shown;              // its arguments as listed: secrets as "***"
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
        double      last_batch_mono{0};   // steady clock, for the night rule
        Json::Value state{Json::objectValue};
        StepReport  last_step;
        bool        stepped{false};
        std::vector<Command> commands;   // oldest first
        std::optional<UpdateJob> update;   // the last firmware update, if any
        LearnedUse  learned;   // how its current rebuilder used a learned card
    };

    /// A bridge seen for the first time takes the install's latest learn
    /// when it holds none, has no geometry yet, and no other bridge holds
    /// that card (SDD-061 D5). Under rebuild_mu_.
    void offerLearned(const std::string& sender);

    struct Conn {
        Admission    who;
        Outlet       out;
        BinaryOutlet bin;
        bool         session{false};   // its own hello came: not a probe
    };

    devices::Recording recordingLocked(const std::string& sender) const;   // mu_ held
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
    ReleaseFeed*     feed_{nullptr};

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

    std::thread update_thread_;   // one update at a time (sendUpdate)
};

}  // namespace hms_cpap::sleepbridge
