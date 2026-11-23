#pragma once
//
// SDD-080 D4, D6: what the user's OSCAR stored, read back from its database.
//
// OSCAR 2.x keeps everything in one SQLite file, oscar.db, in its data folder.
// hms-cpap only reads it, with its own SQL, and only after OSCAR's process has
// exited (D2). Nothing here is OSCAR code: the tables and columns below are
// what OSCAR 2.1.1's database holds, observed from its output.
//
//   - The connection is opened normally and made query-only
//     (PRAGMA query_only = ON), never opened read-only: a read-only open of a
//     WAL database fails when its -shm cannot be made (D6). No statement can
//     write.
//   - Before anything is read, the version gate: app_preferences.VersionString
//     and MAX(schema_version.version) must be a pair this build was proven on
//     (E10). Anything else is "not supported yet" and nothing is read.
//   - Everything is scoped to ONE profile (profiles.username): hms-cpap imports
//     into a profile of its own, beside the user's (SDD-080 Status).
//
// Samples: event_data.data_blob holds little-endian int16 values, value =
// sample * gain + offset. An evenly sampled list (event_lists.event_type 0)
// runs from first_time every `rate` ms; a timed list (type 1) carries its
// times in time_blob as little-endian uint32 ms after first_time. Channel
// types: 4 a flag, 8 a minor flag, 16 a span, 32 data (waveform, trends).
//
#include "parsers/CpapdashBridge.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace hms_cpap::oscar {

/// The OSCAR versions and database schemas this build was proven on (E10).
/// A new OSCAR release is let in only once it has been proven too.
struct ProvenVersion {
    const char* version_prefix;   ///< VersionString starts with it ("2.1.1+4f146246")
    int         schema;           ///< MAX(schema_version.version)
};
const std::vector<ProvenVersion>& provenVersions();

struct Gate {
    bool        ok{false};
    std::string version;   ///< VersionString as OSCAR wrote it, "" when absent
    int         schema{0};
    std::string why;       ///< set when !ok
};

/// One session OSCAR stored, before its samples are read.
struct SessionHead {
    long long   row_id{0};        ///< sessions.id
    long long   session_id{0};    ///< OSCAR's session number (the card's)
    std::string serial;           ///< machines.serial_number
    std::string brand;
    std::string model;
    std::string model_number;
    long long   start_ms{0};
    long long   end_ms{0};
    long long   duration_ms{0};
    bool        enabled{true};
    bool        summary_only{false};
    // OSCAR's own counts (session_summaries), a cross-check only: the
    // session's counts come from its events, as every machine's do.
    int oa{0}, ca{0}, h{0}, ua{0}, rera{0};
};

/// "<serial>/<session_id>": a session's identity in OSCAR.
std::string sessionKey(const SessionHead& s);

class OscarDb {
public:
    ~OscarDb();
    OscarDb(const OscarDb&) = delete;
    OscarDb& operator=(const OscarDb&) = delete;
    OscarDb(OscarDb&& o) noexcept;
    OscarDb& operator=(OscarDb&& o) noexcept;

    /// Opens <data_dir>/oscar.db query-only. nullopt with `why` when the file
    /// is missing or SQLite refuses it.
    static std::optional<OscarDb> open(const std::string& data_dir, std::string* why);

    /// D6's version gate.
    Gate gate();

    /// Every profile in the folder (profiles.username), for the Settings list.
    std::vector<std::string> profiles();

    /// The database's signature for one profile: sessions, summaries, channel
    /// rows and the newest session end. It tells "imported" from "nothing new"
    /// without reading OSCAR's log. "" when the profile is not there.
    std::string signature(const std::string& profile);

    /// Every session of the profile's machines, oldest first. The profile's
    /// journal (machines.loader_name "Journal") is not a machine and is left
    /// out.
    std::vector<SessionHead> sessions(const std::string& profile);

    /// One session decoded into what every reader hands saveSession: its
    /// times, the machine, every timed event, the per-minute rows and the
    /// breaths from the flow, the trends folded into the minutes, and the
    /// metrics. nullptr with `why` when its samples cannot be read.
    std::unique_ptr<CPAPSession> read(const SessionHead& head, const std::string& device_id,
                                      const std::string& device_name, std::string* why);

    /// One read transaction around a whole pass (D6).
    bool begin();
    void end();

    /// SDD-081 D5.4: a small oscar.db at `out_file` holding only what read()
    /// and gate() need, for only the sessions named by `keys` (sessionKey) of
    /// `profile`: app_preferences' VersionString, schema_version, the
    /// profile's row, its machines, those sessions' sessions,
    /// session_summaries, session_channels, event_lists and event_data rows,
    /// and the channels they name. Each table is made as this database made
    /// it. Written through a second connection of its own; this one stays
    /// query-only. An existing `out_file` is replaced. False with `why`.
    bool writeSlice(const std::string& profile, const std::vector<std::string>& keys, const std::string& out_file,
                    std::string* why);

private:
    explicit OscarDb(sqlite3* db) : db_(db) {}
    sqlite3* db_{nullptr};
    bool in_tx_{false};
};

/// An OSCAR time (ms since the epoch, a real instant) as that instant.
std::chrono::system_clock::time_point sessionClock(long long oscar_ms);

/// SDD-083 F1 (SDD-074 D4/D6): an OSCAR time on the machine's clock, the
/// label every session is held as, as the Bluetooth and plain card readers
/// hold a DreamStation's (putOnMachineClock). Nights split at the machine's
/// noon. OscarDb::read returns its sessions on this clock.
std::chrono::system_clock::time_point sessionLabel(long long oscar_ms, const std::string& device_id);

}  // namespace hms_cpap::oscar
