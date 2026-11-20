#pragma once
//
// SDD-080: a DreamStation 2 card is read through the user's own OSCAR.
//
// hms-cpap never decrypts a card and carries no OSCAR. The user installs OSCAR
// as a separate program; hms-cpap knows where it is, runs it on its own
// command line with no window, waits for it to exit, and reads back what it
// stored (OscarDb.h). This file is the command line, the process and what its
// exit means:
//
//   QT_QPA_PLATFORM=offscreen <program> --datadir <data folder> \
//       --profile <profile> --import PRS1 <card root>
//
//   - The card root is the folder holding P-SERIES/<machine>/ (OSCAR takes a
//     card root, never the machine folder, and follows no symlink).
//   - The profile is hms-cpap's own, beside the user's (SDD-080 Status), so an
//     import never touches the profile the user has open on screen.
//   - Never --upgrade-database (E4): the user's OSCAR upgrades its own folder.
//   - The exit is the event (D2, D3): 0 imported, 1 failed, 75 the running
//     OSCAR could not import now. Past the job limit the process is killed and
//     the read is "timeout", never "no data".
//
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hms_cpap::oscar {

/// OSCAR's loader for a P-SERIES card tree.
inline constexpr const char* kLoader = "PRS1";
/// The profile name Settings offers.
inline constexpr const char* kDefaultProfile = "hms-cpap";

/// The three Settings fields (D7).
struct Settings {
    std::string program;    ///< the OSCAR program
    std::string data_dir;   ///< OSCAR's data folder (holds oscar.db)
    std::string profile{kDefaultProfile};
    bool set_up() const { return !program.empty() && !data_dir.empty() && !profile.empty(); }
};

/// Where each OS's OSCAR installer puts the program, in the order looked at.
/// `home` is the user's home folder ("" for none).
std::vector<std::string> programCandidates(const std::string& home);

/// The first candidate that exists, else "".
std::string findProgram();

/// The argument vector of D2's command, program first. Never
/// --upgrade-database.
std::vector<std::string> importCommand(const Settings& s, const std::string& card_root);

/// The argument vector of the check on save: `<program> --help`.
std::vector<std::string> helpCommand(const std::string& program);

/// The job's limit in seconds, scaled to the card as the cloud's card read
/// measured it (about 1.43 s per MB read whole, three times that for a first
/// read, never under two minutes, at most an hour, plus ten minutes).
int jobLimitSeconds(uint64_t card_bytes);

/// The card root of a Philips card folder: the folder above P-SERIES when the
/// card sits at P-SERIES/<machine>/, else the folder itself.
std::string cardRootOf(const std::string& card_dir);

/// The card's total size in bytes, for the job limit.
uint64_t cardBytes(const std::string& card_root);

/// A DreamStation closing a session: a .B01 (its summary, written at mask-off)
/// under P-SERIES/. `rel` is card-relative, '/' separated.
bool isSessionClose(const std::string& rel);

// ── the process ─────────────────────────────────────────────────────────────

struct ProcessResult {
    bool        started{false};
    int         exit_code{-1};
    bool        timed_out{false};   ///< killed at the job limit
    std::string output;             ///< stdout and stderr, the last few KB
    std::string error;              ///< why it did not start
};

/// Runs one program to its exit. The interface lets the suite stand a fake
/// OSCAR in.
class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;
    /// Runs argv[0] with argv, QT_QPA_PLATFORM=offscreen in its environment,
    /// and waits for it to exit; past `limit_s` it is killed.
    virtual ProcessResult run(const std::vector<std::string>& argv, int limit_s) = 0;
};

/// The real one: a child process (fork and exec, CreateProcess on Windows).
class SystemProcessRunner : public IProcessRunner {
public:
    ProcessResult run(const std::vector<std::string>& argv, int limit_s) override;
};

// ── what the exit means (D3) ────────────────────────────────────────────────

/// The Devices tile's states.
enum class Outcome {
    NotSetUp,       ///< a Settings field is empty
    NoProgram,      ///< the program did not start
    NotSupported,   ///< the database is of an OSCAR this build was not proven on
    NoProfile,      ///< the profile is not in the data folder
    Busy,           ///< OSCAR's 75: the running OSCAR could not import now
    Timeout,        ///< killed at the job limit
    ReadNothing,    ///< exit 1 and nothing new stored
    Failed,         ///< any other failure
    NothingNew,     ///< exit 0, the database unchanged
    Imported,       ///< exit 0, the database changed
};
const char* outcomeName(Outcome o);

/// D3: the exit, with whether OSCAR's database changed across the run.
Outcome classifyExit(const ProcessResult& r, bool db_changed);

}  // namespace hms_cpap::oscar
