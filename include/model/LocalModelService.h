#pragma once
//
// SDD-013: the local sleep model's nightly batch job, and its switch.
//
// Events, never timers. The job runs when a night closes: the SDD-046 quiet
// close announces it (BurstCollectorService::addNightClosedListener, the
// listener SDD-078 added), the listener only queues the night, and the worker
// writes that night's summary into the existing summary storage with source
// "local". Nothing runs on a clock and nothing is retried on one; a night the
// model could not summarize is left, and the log says why.
//
// The switch: turning it on with no model installed downloads the pinned model
// (model/ModelFile.h) and checks its hash; a model file can be loaded instead
// for an install with no network. With an empty pin (no model published),
// turning it on is refused with the reason.
//
#include "database/IDatabase.h"
#include "model/LocalModel.h"
#include "model/ModelFile.h"

#include <json/json.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace hms_cpap {
struct AppConfig;
}

namespace hms_cpap::model {

class LocalModelService {
public:
    enum class Event { Startup, NightClosed, Download, InstallFile };

    struct Deps {
        std::shared_ptr<IDatabase> db;     ///< its own connection on a server database
        AppConfig* config = nullptr;       ///< local_model.enabled is written back here
        std::string config_path;           ///< where it is saved ("" saves nothing)
        std::string device_id;             ///< whose nights are summarized
        Pin pin = pinnedModel();
        /// The model for the file at [path]. Default: GgufModel.
        std::function<std::unique_ptr<ILocalModel>(const std::string& path)> make_model;
        /// One download (default: libcurl).
        Download download;
    };

    explicit LocalModelService(Deps deps);
    ~LocalModelService();

    /// The worker thread, waiting on events. Queues one Startup event, which
    /// checks an installed model's hash once.
    void start();
    void stop();

    // -- The events. Each returns at once; the worker does the work. ---------

    /// SDD-046: the night in DATALOG folder [date_folder] (YYYYMMDD) closed.
    /// [str] is its STR row when the card has one, taken by the caller on the
    /// burst's thread (BurstCollectorService::strRecordForDay), and rides with
    /// the event so the model is given the night the daily summary is.
    void onNightClosed(const std::string& date_folder,
                       std::optional<STRDailyRecord> str = std::nullopt);

    /// The Settings switch turned on. "" when accepted: on at once with a
    /// verified model in place, else the download is queued. Otherwise why
    /// not (no model published yet, above all), and nothing changes.
    std::string requestEnable();
    /// The Settings switch turned off. The model file stays.
    void disable();
    /// "Load model from file": [path] is the uploaded file, which this takes
    /// over (it is deleted once checked). "" when queued, else why not.
    std::string installFile(const std::string& path);

    /// What the Settings section shows: {"enabled", "published", "state":
    /// off|not_published|downloading|installing|ready|no_runtime|error,
    /// "message", "version", "installed", "runtime", "busy",
    /// "last": {"night", "how", "at", "why"}}.
    Json::Value status() const;

    /// Runs one event now, on the calling thread, as the worker would. For
    /// the suite (and the worker itself).
    void handle(Event e, const std::string& arg = "",
                const std::optional<STRDailyRecord>& str = std::nullopt);

    /// The model directory (local_model.model_dir, or <data dir>/models).
    std::string dir() const;

    /// Told status() each time a download or a loaded file finishes (put in
    /// place, or refused), on the worker's thread, so an open Settings page
    /// follows the state instead of keeping "Checking the model file..." until
    /// a reload. Set once, before start(). It must only take the event.
    void setStateListener(std::function<void(const Json::Value&)> f) { state_listener_ = std::move(f); }

private:
    void tellState();
    void run();
    void enqueue(Event e, const std::string& arg,
                 std::optional<STRDailyRecord> str = std::nullopt);
    bool enabled() const;
    void setEnabled(bool on);
    std::unique_ptr<ILocalModel> makeModel() const;
    void noteInstalled();

    Deps deps_;

    mutable std::mutex mu_;   // the state below and the config's local_model block
    std::condition_variable cv_;
    struct Queued {
        Event event;
        std::string arg;
        std::optional<STRDailyRecord> str;   ///< NightClosed only
    };
    std::deque<Queued> queue_;
    bool stopping_ = false;
    std::thread worker_;

    std::mutex work_mu_;      // one event at a time
    bool busy_ = false;
    bool installed_ = false;  // verified at Startup and after each install
    bool runtime_ready_ = false;
    std::string runtime_why_; // the model's whyNotReady() when it is not
    std::string state_;       // "" = derived from the above
    std::string message_;
    std::string last_night_, last_how_, last_at_, last_why_;
    std::function<void(const Json::Value&)> state_listener_;
};

}  // namespace hms_cpap::model
