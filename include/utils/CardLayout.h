#pragma once
//
// CardLayout - what a configured local directory actually IS.
//
// WHY THIS EXISTS. A ResMed card writes STR.edf and DATALOG/ as SIBLINGS at its
// root. That is the layout the machine produces, so it is a fixed fact and not
// something to be probed, guessed at, or recovered from.
//
// The code did not treat it that way. `local_dir` was documented as the DATALOG
// directory, so STR resolution reached UPWARD with parent_path(), and then, when
// that missed, searched INSIDE DATALOG as a fallback. STR is never in there. The
// fallback could not succeed; its only effect was to make a misconfiguration
// present itself as a missing file, which is the most expensive way to be wrong.
//
// SDD-010 pins the contract: `local_dir` is the card ROOT. This classifier is
// how that contract is enforced, once, in one place.
//
// WHY IT HARD FAILS. Auto-correcting a DATALOG path up one level was considered
// and rejected. Silently repairing the value makes the contract negotiable
// again, and a rule that repairs itself is a rule nobody can depend on. This
// follows the SDD-005 rule already in force: never discover a configuration
// error through a retry, validate up front and report the cause AND the remedy.
//
// WHY IT IS PURE. It answers a question about a path and changes nothing. That
// keeps it unit testable against a temp directory, with no service, no database
// and no card.
//
#include <string>

namespace hms_cpap {

/// What the configured directory turned out to be.
enum class LocalDirLayout {
    /// Contains DATALOG/. This is the card root, which is what we want.
    Root,
    /// Holds YYYYMMDD folders directly, so the user pointed at DATALOG itself.
    /// Usable-looking and wrong: sessions would be found, STR never would.
    IsDatalog,
    /// Neither. A wrong path, an empty one, or a share that is not mounted.
    Unusable,
};

/// Classify a configured local directory.
///
/// Root wins when DATALOG/ is present, without looking at anything else: a card
/// root that also happens to contain a stray eight-digit folder is still a root.
/// An empty or missing path is Unusable rather than a special case, because from
/// the user's side "I typed it wrong" and "the share did not mount" need the
/// same answer.
LocalDirLayout classifyLocalDir(const std::string& path);

/// The DATALOG directory beneath a card root, in the casing actually found on
/// disk. Returns `<root>/DATALOG` when nothing matches, so callers always get a
/// usable path to name in an error message.
std::string datalogDirFor(const std::string& root);

/// Stable identifier for logs and JSON. Never translated.
const char* localDirLayoutString(LocalDirLayout layout);

/// What went wrong, written for the person reading it rather than the developer.
/// Empty when the layout is Root.
std::string localDirProblem(LocalDirLayout layout, const std::string& path);

/// What to do about it. Empty when the layout is Root. A failure without a
/// remedy is how a user ends up filing a support ticket, which is exactly the
/// outcome this is meant to prevent.
std::string localDirRemedy(LocalDirLayout layout, const std::string& path);

}  // namespace hms_cpap
