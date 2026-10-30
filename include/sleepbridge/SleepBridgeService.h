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
// last 90 s; the route closes a connection silent that long.
//
#include "sleepbridge/BatchStore.h"
#include "sleepbridge/Conversation.h"
#include "sleepbridge/RebuildWorker.h"
#include "sleepbridge/Rebuilder.h"

#include <json/json.h>

#include <condition_variable>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace hms_cpap::sleepbridge {

inline constexpr int kDefaultPort  = 8894;
inline constexpr int kSilenceSec   = 90;
inline constexpr const char* kRoute = "/sleepbridge/v1/device";

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

    /// A connection was admitted: the conversation it talks to.
    std::unique_ptr<Conversation> open(const Admission& who);
    /// What a frame meant, beyond the frames sent back.
    void apply(const std::string& sender, const Effects& fx);
    void closed(const std::string& sender);

    /// Rebuild `sender` now, on the caller's thread (tests, and the start).
    StepReport rebuildNow(const std::string& sender);

    Json::Value status() const;

private:
    struct Bridge {
        std::string machine;        // X-Coyote-Machine, the bridge's own label
        std::string card_machine;   // the rebuild's: "resmed", "luna", "" unknown
        std::string chip_id;
        int         sessions{0};
        std::time_t first_seen{0};
        std::time_t last_heard{0};
        std::time_t last_batch{0};
        Json::Value state{Json::objectValue};
        StepReport  last_step;
        bool        stepped{false};
    };

    void loop();
    void wake(const std::string& sender);

    Options          opt_;
    RebuilderFactory factory_;
    bool             available_{false};
    BatchStore       store_;

    mutable std::mutex mu_;   // bridges_
    std::map<std::string, Bridge> bridges_;

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
