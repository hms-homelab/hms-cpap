#pragma once
//
// SDD-080 D2 to D7: the DreamStation 2 card read through OSCAR, start to end.
//
// One worker thread, woken only by an event: a session closing on the card (a
// .B01 landing, D2) or "Read now" on the Devices page. It runs the user's
// OSCAR on the card (OscarCard.h), waits for its exit, and on an import reads
// OSCAR's database (OscarDb.h) for the sessions hms-cpap does not hold yet,
// or that grew, and stores each one as every reader does:
//
//   saveSession, its card files (source "card", SDD-077 D1),
//   replaceConstructedFiles (the card wins over a "ble-ds2" copy, D5),
//   closed on its own data end, then the nights' daily rows.
//
// A close that arrives while a read runs is absorbed by ONE more read after
// it, never a queue. There is no timer, no poll and no schedule: the close
// starts a read, OSCAR's exit ends it.
//
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"
#include "services/OscarCard.h"

#include <json/json.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hms_cpap::oscar {

class OscarCardReader {
public:
    struct Options {
        std::function<Settings()>    settings;    ///< the three Settings fields, read at each start
        std::function<std::string()> card_root;   ///< the card root, "" when there is none
        std::shared_ptr<IDatabase>   db;
        std::string                  device_id;
        std::string                  device_name;
        std::shared_ptr<IProcessRunner> runner;   ///< null: the system's
        std::function<std::set<std::string>()> removed;   ///< SDD-029's removed nights
        /// After a read stored sessions: the newest, and how many. On the
        /// reader's thread.
        std::function<void(const CPAPSession& newest, int stored)> on_stored;
        bool containerised{false};   ///< Docker or the HA add-on (D8)
    };

    struct ReadResult {
        Outcome     outcome{Outcome::Failed};
        int         stored{0};     ///< sessions saved this read (new or grown)
        std::string version;       ///< the OSCAR that made the database
        std::string message;       ///< for the log and the tile
    };

    explicit OscarCardReader(Options o);
    ~OscarCardReader();
    OscarCardReader(const OscarCardReader&) = delete;
    OscarCardReader& operator=(const OscarCardReader&) = delete;

    void start();
    void stop();

    /// A session closed, or "Read now": a read starts, or one more after the
    /// one running. Any thread.
    void request(const std::string& why);

    /// The session closes among `rels` (card-relative paths under `card_root`;
    /// empty: every .B01 of the card): any .B01 that is new or changed since
    /// it was last noted (its modification time AND size) requests a read.
    /// True when one was requested.
    bool noteCloses(const std::string& card_root, const std::vector<std::string>& rels = {});

    /// The Devices tile's OSCAR line.
    Json::Value status() const;

    /// One read, on the calling thread (the worker's body; tests drive it).
    ReadResult readOnce();

    /// D7's check on save: the program answers --help with its batch import,
    /// the folder holds an oscar.db that passes D6's gate, the profile is in
    /// it. Codes, not sentences: the page words them in its language.
    ///   {"ok", "program": {"state", "found"}, "data_dir": {"state", "version", "schema"},
    ///    "profile": {"state"}, "profiles": [...]}
    static Json::Value check(const Settings& s, IProcessRunner& runner, bool containerised);

private:
    void run();
    /// Stores one decoded session. True when saved.
    bool storeOne(CPAPSession& s, long long session_number, const std::string& card_root);

    Options opt_;
    std::shared_ptr<IProcessRunner> runner_;
    std::thread worker_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool pending_{false};
    bool stop_{false};
    bool running_{false};
    std::string why_;

    // What the tile shows.
    std::string last_state_{"never"};
    std::string last_version_;
    std::string last_message_;
    std::time_t last_read_at_{0};
    int last_stored_{0};

    /// "<serial>/<session>" -> the end (ms) stored. Worker only.
    std::map<std::string, long long> done_;
    /// A .B01's card-relative path -> (mtime, size) when last noted.
    std::map<std::string, std::pair<long long, uint64_t>> closes_;
};

}  // namespace hms_cpap::oscar
