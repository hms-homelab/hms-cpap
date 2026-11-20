#pragma once

#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * SDD-057 D9: a night hms-cpap wrote from an AirSense 11's Bluetooth stream
 * is a session like a night read from files.
 *
 * As11NightWriter lays the stream out as the machine's card would, under
 * <data dir>/as11/<serial>/DATALOG/<night>/, but nothing read that folder:
 * the burst cycle walks the configured source (a card, a WiFi card, a local
 * folder) and nothing else, so the night sat on disk with no cpap_sessions
 * row and the dashboard showed only the card's copy, when there was one.
 *
 * This walks every machine's DATALOG under as11/ and puts each mask-on
 * stretch through the same grouping and parser a card folder goes through
 * (SessionDiscoveryService::groupLocalFolder, EDFParser::parseSession), under
 * the install's device id, with its files recorded relative to the data
 * directory ("as11/<serial>/DATALOG/<night>/<file>") and their source,
 * "ble-as11" (SDD-077 D1). The source, not the path, is how a Bluetooth row is
 * told from a card's.
 *
 * D5, card and Bluetooth on one install: the card wins a stretch it carries.
 * A Bluetooth stretch that overlaps a stored card session in time is not
 * imported, and a card session stored over a constructed row that overlaps it
 * replaces that row (replaceConstructedFiles, SDD-077 D2, called by every card
 * path after it saves).
 * The two copies of one night need not match: on a bench the machine split
 * the session at its noon boundary into two card files while the stream had
 * one, so the test is overlap, never equality of starts.
 *
 * A stretch is read once per process while unchanged (its files' sizes), and
 * again when it grew; the newest stretch of each machine is read once at
 * start even when stored, since it may have grown while hms-cpap was down.
 */
class As11NightImport {
public:
    As11NightImport(std::string data_dir, std::shared_ptr<IDatabase> db,
                    std::string device_id, std::string device_name);

    /// What one run did.
    struct Result {
        int imported = 0;     ///< stretches stored for the first time
        int grown = 0;        ///< stored stretches read again because they grew
        int card_wins = 0;    ///< stretches left to the card (D5)
        int failed = 0;       ///< stretches the parser refused
        /// Every session saved this run, oldest first; the last is the night
        /// to publish.
        std::vector<CPAPSession> saved;
    };

    /// One pass over <data dir>/as11/. [removed] is SDD-029's set: a removed
    /// night is not put back.
    Result run(const std::set<std::string>& removed);

    /// The root the Bluetooth nights are under.
    std::string root() const;

    // ── the decisions, pure ─────────────────────────────────────────────

    /// A stored session's span, and whether it was constructed: it has files
    /// and none of them is "card" (SDD-077 D1). A row with no file rows at all
    /// is not known to be constructed, so it is never replaced.
    struct StoredSpan {
        std::chrono::system_clock::time_point start;
        std::chrono::system_clock::time_point end;
        bool constructed = false;
    };

    /// Two stretches of therapy touch: inclusive on both ends, so a card
    /// checkpoint that starts the second the stream's ends still counts.
    static bool overlaps(const std::chrono::system_clock::time_point& a_start,
                         const std::chrono::system_clock::time_point& a_end,
                         const std::chrono::system_clock::time_point& b_start,
                         const std::chrono::system_clock::time_point& b_end);

    /// D5 for a Bluetooth stretch [start, end]: does a stored card session
    /// overlap it? True means the card has this stretch and the Bluetooth
    /// copy is not stored (or no longer is).
    static bool cardHolds(const std::chrono::system_clock::time_point& start,
                          const std::chrono::system_clock::time_point& end,
                          const std::vector<StoredSpan>& stored);

    /// D5 for a card session [start, end] just stored: the constructed rows it
    /// replaces, i.e. every stored constructed span overlapping it.
    static std::vector<std::chrono::system_clock::time_point> constructedRowsReplacedBy(
        const std::chrono::system_clock::time_point& start,
        const std::chrono::system_clock::time_point& end,
        const std::vector<StoredSpan>& stored);

    // ── the database side ───────────────────────────────────────────────

    /// Every session of [device_id] whose span touches [start, end], with its
    /// end taken from session_end, else start plus the stored duration, and
    /// whether its files are constructed (their source). One query for all
    /// three engines, through executeQuery.
    static std::vector<StoredSpan> storedSpansOverlapping(
        IDatabase& db, const std::string& device_id,
        const std::chrono::system_clock::time_point& start,
        const std::chrono::system_clock::time_point& end);

    /// SDD-077 D2: a native card file always replaces a constructed one. Every
    /// card path (the collector, a local folder, the SD and ZIP backfill, the
    /// reparse) calls this after it saves the card session [start, end]: each
    /// stored session overlapping it whose files are not "card" is deleted
    /// with its cpap_session_files rows. The constructed EDFs on disk stay
    /// (SDD-070 reads them). Returns how many rows it replaced.
    static int replaceConstructedFiles(IDatabase& db, const std::string& device_id,
                                       const std::chrono::system_clock::time_point& start,
                                       const std::chrono::system_clock::time_point& end);

    /// Delete one session row and its children, by its start.
    static bool deleteSessionAt(IDatabase& db, const std::string& device_id,
                                const std::chrono::system_clock::time_point& start);

    /// The prefix every Bluetooth night's rel_path carries.
    static constexpr const char* kRelPrefix = "as11/";

private:
    std::string data_dir_;
    std::shared_ptr<IDatabase> db_;
    std::string device_id_;
    std::string device_name_;
    /// "serial/night/prefix" -> the stretch's total bytes when last read.
    std::map<std::string, uint64_t> seen_;
};

}  // namespace hms_cpap
