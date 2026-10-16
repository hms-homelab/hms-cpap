#pragma once
//
// ListingPlan (SDD-054): which folders of the card a burst lists, and why.
//
// Until SDD-054 every burst listed the DATALOG index twice and then every date
// folder from the second-newest stored night on, and after a burst that
// downloaded, the root and every directory of the card as well: about 275
// listings an hour on an idle card and 550 to 700 during a night. On an ez
// Share each listing is a page over the card's own WiFi and about 2 s of its
// time. Now a folder is listed for a reason:
//
//   tonight     the folder the machine writes tonight, by its name, every burst
//               (before, during and after the night: a resumed session or a
//               night's first file is seen within a burst);
//   first look  a folder the index names that this run has not listed yet and
//               that can hold something new (on or after the anchor, recent, or
//               a night the database never had);
//   relist      a fault, a swap, a Sync now or a service start marked it (D6);
//   live        its last listing showed growth (a session that started before
//               the machine's noon and is still running);
//   checkpoint  growth stopped: listed again 5, 35 and 65 minutes later, the
//               last of which sees the hour of quiet (D4);
//   ceiling     a recent folder not listed for 8 hours (D6).
//
// A folder that is none of these is settled and is not listed. The machine
// writes only into the folder of the session it is running, so a folder that
// is not tonight's and has been quiet for the hour changes only with a card
// swap, an edit or a fault, which are D6's reasons.
//
// PURE: no I/O and no clock. Times are epoch seconds on the host's clock; the
// machine's clock is the host's plus `offset` (MachineClock.h). The collector
// keeps the FolderWatch rows in memory: a restart lists everything recent once
// (D6), which is what rebuilds them.
//
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap {

/// D4: after growth stops, the folder is listed again at these marks. The 65
/// minute one is the listing that sees the night's hour of quiet.
inline constexpr std::chrono::minutes kGrowthCheckpoints[] = {
    std::chrono::minutes{5}, std::chrono::minutes{35}, std::chrono::minutes{65}};

/// D6: no recent folder goes longer than this unlisted while the card answers.
inline constexpr std::chrono::hours kRelistCeiling{8};

/// D6: "recent" is the machine's today and the 3 days before it.
inline constexpr int kRecentDays = 3;

/// D9: how many edf_done folders get their remaining files per burst. A fresh
/// card's walk back leaves one per night; this keeps each burst short.
inline constexpr int kSecondPassPerBurst = 10;

/// D5: SETTINGS floors, on a card that keeps logs there.
inline constexpr std::chrono::minutes kSettingsNightFloor{15};
inline constexpr std::chrono::minutes kSettingsIdleFloor{60};

/// What this run knows about one date folder. In memory only.
struct FolderWatch {
    long long   last_listed{0};   ///< when this run last listed it; 0 = not yet
    long long   last_growth{0};   ///< when a listing last showed it grow
    long long   stopped_at{0};    ///< when a listing first showed no growth after growth
    bool        growing{false};   ///< the last listing showed growth
    bool        relist{false};    ///< D6 mark: list it on the next burst
    std::string sig;              ///< count|total KB|newest stamp of the last listing
};

/// The YYYYMMDD of the machine's clock at [host_now] + [days] days.
std::string machineDay(long long host_now, long long offset, int days = 0);

/// The folder the machine writes tonight: the ResMed noon-to-noon day,
/// date(machine_now - 12 h).
std::string tonightFolder(long long host_now, long long offset);

/// A folder dated after the machine's tomorrow cannot hold a night (a clock
/// that once ran wrong left 20991231 on a card). D7: never recent, never
/// marked, listed only once when the index finds it.
bool isFutureFolder(const std::string& folder, long long host_now, long long offset);

/// The machine's today and the kRecentDays before it, and not a future folder.
bool isRecentFolder(const std::string& folder, long long host_now, long long offset);

/// The host time of the most recent noon on the machine's clock at or before
/// [host_now]. The daily index is due once each time this moves (D1).
long long lastMachineNoon(long long host_now, long long offset);

enum class ListReason { Tonight, FirstLook, Relist, Live, Checkpoint, Ceiling };
const char* listReasonString(ListReason r);

struct PlannedListing {
    std::string folder;
    ListReason  reason;
};

/// This burst's folder listings, tonight's first, each folder once.
///
/// [card_folders] is the last index. [candidates] are the folders that may
/// hold something new (on or after the anchor, a night never imported): each
/// is listed once per run (first look). Tonight's folder is listed whether or
/// not the index named it: its first file is how a night is found.
///
/// [done] (D9): folders whose every night is stored and whose night is over.
/// Never listed again, for any reason: ResMed does not write to them.
std::vector<PlannedListing> planFolderListings(
    long long now, long long offset,
    const std::vector<std::string>& card_folders,
    const std::set<std::string>& candidates,
    const std::map<std::string, FolderWatch>& watch,
    const std::set<std::string>& done = {});

/// Fold one listing of a folder into its row. Growth is a different signature
/// from this run's previous listing, or a file stored from it on this burst.
FolderWatch observeFolder(const FolderWatch& prev, const std::string& sig,
                          bool stored_something, long long now);

/// D1: is the DATALOG index due on this burst? Until one listing succeeds in
/// this run it is due every burst; after that once per machine noon (unless a
/// night is being written) and on a reason.
struct IndexState {
    bool      ok_this_run{false};
    long long last_index{0};    ///< host time of the last successful index
    bool      reason{false};    ///< D6: recovery, swap, Sync now
};
bool indexDue(const IndexState& s, long long now, long long offset, bool night_growing);

/// D5: is the SETTINGS listing due on this burst?
struct SettingsState {
    long long last_listed{0};
    bool      reason{false};        ///< D6: start, recovery, swap, Sync now
    bool      logs_on_card{true};   ///< false once a listing showed only CurrentSettings.*
};
bool settingsDue(const SettingsState& s, long long now, bool night_in_progress,
                 bool therapy_started, bool checkpoint_listed);

/// An AirSense 11's SETTINGS holds only CurrentSettings.json and its .crc;
/// anything else (the AirSense 10's logs) means the card keeps logs there.
bool settingsListingHasLogs(const std::vector<std::string>& names);

}  // namespace hms_cpap
