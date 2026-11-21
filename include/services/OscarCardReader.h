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
#include "services/OscarDb.h"

#include <json/json.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hms_cpap::oscar {

/// The OSCAR half of a read (SDD-080 D2 to D6, SDD-081 D5.2): the gate, the
/// profile and the signature on the database, the headless import of
/// `card_root`, the gate and the signature again, the exit classified. On
/// Imported and NothingNew `heads` is the profile's sessions. The native
/// reader and the helper run this same function.
struct ImportRun {
    Outcome                  outcome{Outcome::Failed};
    std::string              version;
    int                      schema{0};
    std::string              message;
    std::vector<SessionHead> heads;
};
ImportRun runImport(const Settings& s, const std::string& card_root, IProcessRunner& runner);

/// SDD-081 D5: the second OSCAR half, the helper on a home PC. Used instead of
/// the OSCAR on this computer while a helper is remembered.
struct RemoteRead {
    Outcome     outcome{Outcome::Failed};
    std::string version;
    std::string message;
    std::string slice_dir;   ///< holds the returned oscar.db; "" when none came back
};
class IRemoteOscar {
public:
    virtual ~IRemoteOscar() = default;
    /// A helper is remembered: reads are its.
    virtual bool remembered() const = 0;
    /// {"remembered","name","connected","version","update"} for the tile.
    virtual Json::Value status() const = 0;
    /// The heads OSCAR holds, as {key, start_ms, end_ms}: the keys wanted back.
    using Want = std::function<std::vector<std::string>(const std::vector<std::string>& keys,
                                                        const std::vector<std::pair<long long, long long>>& spans)>;
    /// One read on the helper (D5.1 to D5.4): the changed P-SERIES files of
    /// `card_root`, the import, `want`, the small oscar.db into `slice_dir`.
    /// Blocks until the helper answers or its connection ends.
    virtual RemoteRead read(const std::string& card_root, const Want& want, const std::string& slice_dir) = 0;
};

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
        /// SDD-081: the helper, and the folder its returned file is kept in
        /// for one pass. Null: OSCAR on this computer only.
        std::shared_ptr<IRemoteOscar> remote;
        std::string                   slice_dir;
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
    /// The store half's filter (D4): not stored yet, or grew; not a removed
    /// night; not already held as a card row (then noted as done).
    bool wanted(const std::string& key, long long start_ms, long long end_ms, const std::set<std::string>& removed);
    /// The store half on an open, gated database: every wanted session of
    /// `profile` read and stored. The count stored; [landed] gets each stored
    /// session's start (SDD-013 D-701-3).
    int storeFrom(OscarDb& db, const std::string& profile, const std::string& card_root,
                  const std::set<std::string>& removed, std::optional<CPAPSession>& newest,
                  std::vector<std::chrono::system_clock::time_point>& landed);
    ReadResult readRemote(const std::string& root);
    ReadResult finish(ReadResult r, Outcome o, std::string msg);
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
