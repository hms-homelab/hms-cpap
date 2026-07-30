#pragma once
//
// SyncFolderState (SDD-008): whether a night's FILES are all here, which is a
// different question from whether the therapy ended.
//
// A transfer from the Mule and Miner that starts and does not finish leaves the
// night stuck showing as live forever: BurstCollectorService closes a session
// exactly one way, when all checkpoint files are unchanged, and has no branch
// for the TRANSFER failing rather than the therapy continuing.
//
// The shape here mirrors what hms-cpapdash-api learned in production, where the
// same problem was fought four separate times:
//
//   stable        (ticket #25)  a night showed stuck at a fraction even once
//                               every file had arrived, so "done" must key off
//                               a stable SIGNATURE, not a clock.
//   str_due       (ticket 39)   STR debt is EXPLICIT STATE, retried ON RECOVERY
//                               and NEVER ON A TIMER, so a dead card is not
//                               hammered and "not here yet" stays
//                               distinguishable from "never coming".
//   sidecars_due  (ticket 41)   ezShare listings are KB-ROUNDED, so an EVE/CSL
//                               file growing inside one KB bucket is invisible
//                               to a size comparison and its events are lost.
//   resync_count  (2026-07-22)  unbounded re-arming produced 339 re-pulls of
//                               one folder in three days.
//
// THERE ARE NO GRACE WINDOWS ANYWHERE IN HERE, by decision. State derives only
// from observed facts: signature stability, files stored, STR parsed. If a card
// is unreachable the state simply does not advance, which is the honest answer,
// and the next successful listing moves it.
//
// PURE: no DB, no I/O, no clock. The collector reads a row, calls advance(),
// and writes the result back.
//
#include <chrono>
#include <string>

namespace hms_cpap {

/// The persisted per-folder row. Mirrors cpap_sync_folders.
struct FolderLedger {
    std::string date_folder;          ///< YYYYMMDD
    bool        files_listed{false};  ///< the card has given us its file list
    bool        complete{false};      ///< every listed file is stored locally
    bool        stable{false};        ///< signature unchanged since the last burst
    long long   last_total_size{-1};  ///< -1 == never observed
    int         last_file_count{-1};  ///< -1 == never observed
    bool        str_due{false};       ///< STR debt, armed at close, cleared on parse
    /// Which STR day this folder is waiting for (YYYYMMDD), recorded when the
    /// debt is armed. NOT the same as date_folder: a folder is named for the
    /// local calendar date a session STARTED, while ResMed keys a therapy day
    /// from noon, so a session beginning after midnight belongs to the PREVIOUS
    /// STR day (see DATE(session_start - INTERVAL '12 hours') in the daily
    /// summary aggregation). Matching the two by string would leave every
    /// post-midnight night, which is most of them, permanently partial.
    std::string str_day;
    bool        sidecars_due{false};  ///< EVE/CSL refetch debt, same transition
    long long   resync_size{-1};      ///< signature at which debt was last armed
    int         resync_count{0};      ///< re-arms at that signature
};

/// What one burst actually saw for this folder.
struct FolderObservation {
    int       file_count{0};
    long long total_size{0};
    /// True when every file the listing named is stored locally. Separate from
    /// stability on purpose: a folder can have stopped growing while a file is
    /// still being written, and reporting done on stability alone is what made a
    /// night sit at a fraction forever.
    bool      all_files_stored{false};
};

struct FolderTransition {
    FolderLedger next;
    /// The not-stable -> (stable AND complete) edge. Fires once per settling.
    bool closed{false};
    /// Debt was armed on this pass. False when the cap refused to re-arm.
    bool armed_debt{false};
    /// The cap refused: this folder has already been re-armed kMaxResyncArms
    /// times at this exact signature and is not converging.
    bool resync_exhausted{false};
};

/// How many times debt may be re-armed at the SAME signature.
///
/// Enough for the normal close, refetch, reopen, re-close, confirm cycle. Any
/// more and a night whose refetches keep coming back different loops forever,
/// which upstream measured as 339 re-pulls of one folder in three days. A
/// signature change resets the count, because that is real progress.
inline constexpr int kMaxResyncArms = 3;

/// The ResMed therapy day a session belongs to, as YYYYMMDD.
///
/// ResMed keys a therapy day from NOON, so a session that begins after midnight
/// belongs to the PREVIOUS day. This is the same rule the daily-summary
/// aggregation uses (DATE(session_start - INTERVAL '12 hours')).
///
/// This exists as its own function because it is the easiest thing in SDD-008 to
/// get wrong: a DATALOG folder is named for the local calendar date a session
/// STARTED, so for any night that crosses midnight the folder name and the STR
/// date differ by one. Comparing those two strings directly would leave most
/// nights waiting forever for an STR record that had already arrived, and every
/// one of them would report as partial.
std::string strDayForSessionStart(const std::chrono::system_clock::time_point& start);

/// Advance one folder by one burst observation. Pure.
FolderTransition advanceFolder(const FolderLedger& prev, const FolderObservation& obs);

/// Clears the STR debt. Call when a non-empty STR parses for this night.
FolderLedger clearStrDebt(const FolderLedger& in);

/// Clears the sidecar refetch debt. Call once EVE/CSL have been refetched.
FolderLedger clearSidecarDebt(const FolderLedger& in);

/// What the night should report.
///
/// Partial means the transfer settled and the machine's own daily record never
/// arrived. That is a fact about state, not an inference from elapsed time.
enum class NightState { Live, Complete, Partial };

NightState nightState(const FolderLedger& l);
const char* nightStateString(NightState s);

}  // namespace hms_cpap
