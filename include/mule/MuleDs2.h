#pragma once
//
// SDD-065: a DreamStation 2 read through a Mule & Miner, over the unit's
// local socket. The miner holds the Bluetooth bond and walks the machine
// (push-c3 SDD-043); this host keeps what a read needs and what it brings,
// exactly as the direct read keeps them (SDD-064):
//
//   - the bookmarks, per machine serial and stream, in <data_dir>/ds2.json
//     (Ds2Store): sent with every ds2_read, advanced only after the record
//     they follow is on disk;
//   - the records, as <data_dir>/ds2/<serial>/... files (Ds2Store), which
//     the same Ds2NightImport turns into nights;
//   - per unit, <mule root>/<unit>/ds2.json: the day nights are imported
//     from (the day this host first saw the bond, unless the person picks
//     another; the same machine bonded again keeps its day), the machine the
//     unit last read, and the last read's outcome.
//
// When to read is not the unit's (it has no timer, SDD-043 D2) and not a
// timer here (SDD-064 D5): another source on this install says a night ended
// (nightEnded), or the person presses Read now (readNow). A read brings at
// most max_records records; when the file says there are more, the next read
// is asked as soon as this one is stored, never on a clock.
//
// Nothing here talks to the unit but through MuleService::queueCommand, and
// nothing is decrypted: the records are the machine's plaintext.
//
#include "mule/MuleService.h"
#include "services/Ds2Store.h"

#include <json/json.h>

#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hms_cpap::mule {

class MuleDs2 {
public:
    struct Options {
        std::string data_dir;      // Ds2Store's: <data_dir>/ds2.json and <data_dir>/ds2/
        std::string units_root;    // MuleService's root: <root>/<unit>/ds2.json
        int         max_records{32};   // per read, the batch push-c3 SDD-043 is sized for
        int         max_chain{64};     // reads in a row while the machine says "more"
    };

    MuleDs2(Options opt, MuleService& service);

    /// MuleService's answer hook: a ds2_read's file is stored, a ds2_bond
    /// that bonded starts the import day.
    void onAnswer(const MuleService::Answered& a);

    /// What the Devices page and nightEnded ask. 200 {ok, result: "reading",
    /// req_id}; 404 for a unit never seen; 409 {ok: false, result} with
    /// result offline, mode_off, not_paired or busy.
    CommandAnswer readNow(const std::string& unit, bool automatic = false);

    /// POST .../ds2/import-from {date}: nights from that local day on.
    /// 400 for a date that is not one, 404 for a unit never seen.
    CommandAnswer setImportFrom(const std::string& unit, const std::string& ymd);

    /// Another source here says a night ended (and no mask is on): every
    /// unit in DreamStation 2 mode, bonded and online, is read. How many
    /// reads were asked.
    int nightEnded(const std::string& by);

    /// Per unit, for the Devices page: {import_from, import_from_default,
    /// machine, model, firmware, newest_session, first_read_done,
    /// last_read: {at, attempt_at, records, walked_past, error, more} | null,
    /// chain, trigger: "night_end" | "manual", night_sources}.
    Json::Value view(const std::string& unit) const;

    /// The collector's import, run when a read stored records.
    void setImportDue(std::function<void()> f);
    /// The sources that can say a night ended here (the collector's list).
    void setNightSources(std::function<std::vector<std::string>()> f);

    /// One .ds2r stored for `unit`: records first, each stream's bookmark
    /// after its record. Public for the tests.
    struct Applied {
        bool        file_ok{false};       // it was a .ds2r
        bool        stored_ok{false};     // every record kept and every bookmark saved
        std::string serial;               // the machine, from the file's identity answers
        int         records{0};           // records written
        int         walked_past{0};       // before the import day: bookmarked, never kept
        bool        more{false};          // the machine has more after this read
        std::string error;                // "" or a token: SDD-043 §4's, stream_broken, storage, ...
        std::vector<uint32_t> sessions;   // with a new summary or events record
    };
    Applied apply(const std::string& unit, const std::string& bytes);

private:
    struct UnitDs2 {
        std::string import_from;   // "YYYY-MM-DD", local
        std::string peer;          // the bond's address when import_from was set
        std::string machine;       // the serial this unit last read
        Json::Value last_read{Json::nullValue};
    };
    UnitDs2 load(const std::string& unit) const;          // mu_ held
    bool    save(const std::string& unit, const UnitDs2& u) const;   // mu_ held
    /// A bond seen: a new peer starts a new import day. mu_ held.
    void notePeer(const std::string& unit, UnitDs2& u, const std::string& peer);
    void noteRead(const std::string& unit, const Json::Value& last_read);
    CommandAnswer queueRead(const std::string& unit, bool automatic);

    Options                 opt_;
    MuleService&            svc_;
    Ds2Store                store_;
    mutable std::mutex      mu_;
    std::map<std::string, int> chain_;   // reads in a row the machine said "more" to
    std::function<void()>   import_due_;
    std::function<std::vector<std::string>()> night_sources_;
};

}  // namespace hms_cpap::mule
