#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "BridgeText.h"   // Lang, langForLocale

namespace cpapdash::supervisor {

/**
 * SDD-081: this computer as an OSCAR helper, the desktop side.
 *
 * The first window offers the role (D1, F9): hms-cpap itself, or "Read OSCAR
 * for hms-cpap". The helper's window finds hms-cpap (D3, through
 * `hms_cpap --oscar-helper --discover`), takes the three OSCAR fields
 * (SDD-080 D7, checked with `--oscar-helper --check`) and writes config.json;
 * the supervisor then runs `hms_cpap --oscar-helper` and its tray shows the
 * helper's state from <data dir>/oscar-helper/status.json.
 *
 * Pure, like the rest of core/pure: the words, what is written and what the
 * tray says are decided here and covered by run_tests.
 */

inline constexpr const char* kRoleHelper = "oscar_helper";

/// The install's role: "oscar_helper", or "" for hms-cpap itself.
std::string roleOf(const std::optional<nlohmann::json>& config);

/// The helper can start: an hms-cpap address and the OSCAR data folder.
bool helperReady(const nlohmann::json& config);

/// One hms-cpap that answered the probe (D3).
struct FoundHms {
    std::string address;
    int         port = 8894;
    std::string name;
    std::string helper;   ///< the PC it already reads OSCAR on, "" for none
};

/// `--discover`'s JSON ({"found":[...]}); empty on anything else.
std::vector<FoundHms> parseDiscover(const std::string& text);

/// What the window collected.
struct HelperInputs {
    std::string host;
    int         port = 8894;
    std::string program;
    std::string data_dir;
    std::string profile = "hms-cpap";
};

/// Writes the role, the OSCAR fields and the address into `config`, every
/// other key kept. The token is kept only while the address is the same: a
/// new hms-cpap is a new pairing (D4).
void applyHelperInputs(nlohmann::json& config, const HelperInputs& in);

/// The tray's "Connect again" (D4): the token dropped, so the next hello
/// carries none.
void dropHelperToken(nlohmann::json& config);

// ── the words ──────────────────────────────────────────────────────────────

enum class HTxt {
    RoleTitle, RoleQuestion, RoleHms, RoleHmsHint, RoleHelper, RoleHelperHint, Continue,
    WindowTitle, HmsHeading, Looking, FoundOne, FoundMany, FoundNone, TypeAddress, AnotherAddress, AlreadyReads,
    OscarHeading, OscarHint, Program, DataDir, Profile, ProfileHint, Browse, Check, Checking, Save, Cancel,
    StateOk, StateEmpty, StateNotFound, StateNotRunnable, StateNoBatch, StateNoDb, StateNotSupported, StateMissing,
    StateUnknown,
    TrayConnecting, TrayConnected, TrayConnectedLast, TrayImporting, TrayUnreachable, TrayForgotten, TrayRefused,
    TrayUpdate, TrayNoAddress, TrayStopped, ConnectAgain, ChangeSettings, StartAtLogin, Quit,
    Count_
};

/// The word in `lang`. UTF-8; "%1", "%2" are filled by the window.
const char* htext(HTxt key, Lang lang);

/// The field check's state ("ok", "not_found", ...) as its words.
HTxt checkWord(const std::string& state);

/// What the tray says, from status.json.
struct TrayLine {
    HTxt        key = HTxt::TrayConnecting;
    std::string arg1;            ///< the host, or the other PC
    std::string arg2;            ///< the last import's time, "HH:MM"
    bool        connect_again = false;   ///< offer "Connect again"
    bool        problem = false;         ///< the icon says something is wrong
};
TrayLine trayLine(const nlohmann::json& status, const std::string& last_import_hhmm);

}  // namespace cpapdash::supervisor
