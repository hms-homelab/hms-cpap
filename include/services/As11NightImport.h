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
 * directory ("as11/<serial>/DATALOG/<night>/<file>"). That prefix is how a
 * Bluetooth row is told from a card's, whose files are "DATALOG/<night>/...".
 *
 * D5, card and Bluetooth on one install: the card wins a stretch it carries.
 * A Bluetooth stretch that overlaps a stored card session in time is not
 * imported, and a card session stored over a Bluetooth row that overlaps it
 * replaces that row (replaceBluetoothRows, called where the card path saves).
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

    /// A stored session's span, and whether it came over Bluetooth.
    struct StoredSpan {
        std::chrono::system_clock::time_point start;
        std::chrono::system_clock::time_point end;
        bool bluetooth = false;
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

    /// D5 for a card session [start, end] just stored: the Bluetooth rows it
    /// replaces, i.e. every stored Bluetooth span overlapping it.
    static std::vector<std::chrono::system_clock::time_point> bluetoothRowsReplacedBy(
        const std::chrono::system_clock::time_point& start,
        const std::chrono::system_clock::time_point& end,
        const std::vector<StoredSpan>& stored);

    /// Is this a Bluetooth night's file path, as the rows record it?
    static bool isBluetoothPath(const std::string& rel_path);

    // ── the database side ───────────────────────────────────────────────

    /// Every session of [device_id] whose span touches [start, end], with its
    /// end taken from session_end, else start plus the stored duration. One
    /// query for all three engines, through executeQuery.
    static std::vector<StoredSpan> storedSpansOverlapping(
        IDatabase& db, const std::string& device_id,
        const std::chrono::system_clock::time_point& start,
        const std::chrono::system_clock::time_point& end);

    /// D5 at the card path's save: delete the Bluetooth rows (and their
    /// children) that overlap the card session [start, end]. Returns how many.
    static int replaceBluetoothRows(IDatabase& db, const std::string& device_id,
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
