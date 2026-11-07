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
// When to read is not the unit's (it has no timer, SDD-043 D2). It is the
// owner's one choice for every DreamStation 2 here (SDD-064 D5a): once a day
// at their time (dailyCheck), or only when asked (readNow); in both, also when
// another source on this install says a night ended (nightEnded); never while
// a mask is on. Each unit decides from its own last read. A read brings at
// most max_records records; when the file says there are more, the next read
// is asked as soon as this one is stored, never on a clock.
//
// The card tree (SDD-065 D8): a read asks the miner for each record's card
// file too (`card` "both" by default; "sealed" for the card files only,
// "plain" for none), and Ds2CardTree places those bytes, as they came, in a
// card's layout for OSCAR and SleepHQ. Once a sync has ended and the tree
// grew, one more read carries `card_meta` (the tree's file counts and FD), and
// the miner answers it with the card's own PROP.BIN and LOG.SEQ. That read
// gives every stream a bookmark of this unit's own (only a stream read empty
// to its end goes from the oldest), so it brings nothing that would make its
// counts wrong; if it brings files anyway, it is asked again with the new
// counts.
//
// The bookmarks a read sends are only ones this unit's own reads returned,
// kept in its ds2.json: a bookmark from another reader's era can make the
// machine answer "nothing after it" for a stream that has records.
//
// Nothing here talks to the unit but through MuleService::queueCommand, and
// nothing here opens, changes or makes a card file: the records are the
// machine's own, and the card files are placed byte for byte as they came.
//
#include "mule/MuleService.h"
#include "services/Ds2CardTree.h"
#include "services/Ds2ReadTime.h"
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
    /// chain, trigger: "night_end" | "manual", night_sources,
    /// read_mode: "daily" | "manual", read_at: "HH:MM", next_read_at}.
    Json::Value view(const std::string& unit) const;

    /// SDD-065 D4a (SDD-064 D5a): the owner's daily read, for every unit in
    /// DreamStation 2 mode that is bonded: once `in.read_at` has come today
    /// with no read tried (or bond made) since, the unit is read, at the first
    /// check after it with no mask on. `in` carries the time, the choice and
    /// the mask; each unit's own last read and bond fill the rest. "manual"
    /// reads nothing here. How many reads were asked.
    int dailyCheck(const ds2time::Input& in);

    /// The owner's read choice as the page shows it: {read_mode, read_at}.
    void setReadSchedule(std::function<std::pair<std::string, std::string>()> f);

    /// What a read asks for besides the records: "both" (the default), "sealed"
    /// or "plain" (config ds2_ble.card).
    void setCardMode(std::function<std::string()> f);

    /// The card tree the miner's card files go to.
    Ds2CardTree& cardTree() { return tree_; }

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
        int         card_files{0};        // card files placed (P, D and E; not PROP.BIN, LOG.SEQ)
        bool        card_meta{false};     // the card's PROP.BIN and LOG.SEQ came
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
        long long   paired_at{0};  // epoch s this host first saw that bond
        std::string machine;       // the serial this unit last read
        Json::Value last_read{Json::nullValue};
        /// Per machine serial, per stream ("1".."4"): the bookmark this unit's
        /// own reads returned (hex), or "" for a stream read empty to its end.
        Json::Value own{Json::objectValue};
    };
    UnitDs2 load(const std::string& unit) const;          // mu_ held
    bool    save(const std::string& unit, const UnitDs2& u) const;   // mu_ held
    /// A bond seen: a new peer starts a new import day. mu_ held.
    void notePeer(const std::string& unit, UnitDs2& u, const std::string& peer);
    void noteRead(const std::string& unit, const Json::Value& last_read);
    /// `card_meta`: the read that ends a sync, carrying the tree's counts.
    CommandAnswer queueRead(const std::string& unit, bool automatic, bool card_meta = false);
    std::string cardMode() const;
    /// The card_meta read can go: every stream has a bookmark of the unit's
    /// own, or was read empty to its end. mu_ held.
    bool everyStreamOwned(const UnitDs2& u, const std::string& serial) const;

    Options                 opt_;
    MuleService&            svc_;
    Ds2Store                store_;
    Ds2CardTree             tree_;
    mutable std::mutex      mu_;
    std::map<std::string, int> chain_;   // reads in a row the machine said "more" to
    std::map<std::string, bool> tree_grew_;   // card files placed since the last card_meta
    std::map<std::string, int>  meta_tries_;  // card_meta reads asked again this sync
    std::function<std::string()> card_mode_;
    std::function<void()>   import_due_;
    std::function<std::vector<std::string>()> night_sources_;
    std::function<std::pair<std::string, std::string>()> schedule_;
};

}  // namespace hms_cpap::mule
