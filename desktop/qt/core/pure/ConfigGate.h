#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ConfigModel.h"
#include "FieldSpec.h"

namespace cpapdash::supervisor {

/**
 * SDD-016: is this thing configured well enough to run?
 *
 * The SUPERVISOR answers that question, by reading config.json, before anything
 * is launched. That is the whole division of labour: the parent decides whether
 * the child is ready, and if it is not, the parent opens a window and helps the
 * user fix it.
 *
 * The tempting alternative -- start the child and see whether it comes up, or
 * ask it over HTTP -- gets this backwards. It puts the burden of reporting
 * "I am not configured" on the process that cannot start when it is not
 * configured, so the one case you most need an answer for is exactly the case
 * where nothing is there to answer. A first run has no config, no service, and
 * no port to talk to; polling /health there is asking a question of something
 * that does not exist yet.
 *
 * It is also the wrong shape for a user. A supervisor that spawns a doomed
 * child and reports "hms_cpap exited with code 1" has told someone their
 * software is broken. A supervisor that reads the file, sees no data source,
 * and OPENS THE CONFIGURATION WINDOW has told them what to do next.
 *
 * Pure: no Qt, no filesystem. The caller reads the file; this decides what it
 * means. That keeps the decision testable in run_tests on every platform.
 */

enum class Readiness {
    NeedsSetup,   ///< open the wizard
    Ready,        ///< start the child
};

/// Why setup is needed, so the wizard can say something specific rather than
/// dropping the user on a generic welcome screen.
enum class SetupReason {
    None,
    NoConfigFile,      ///< first run on this machine
    Unparseable,       ///< the file exists but is not JSON
    SetupIncomplete,   ///< setup_complete is false
    Invalid,           ///< settings present but unusable, e.g. no data source
};

struct GateResult {
    Readiness   readiness = Readiness::NeedsSetup;
    SetupReason reason    = SetupReason::NoConfigFile;

    /// Populated when reason == Invalid: exactly what is wrong, from the same
    /// rule set the settings dialog uses, so the wizard can land on the field
    /// that caused it.
    std::vector<Issue> issues;

    /// One sentence for the window, written for the person reading it.
    std::string summary;

    bool needsSetup() const { return readiness == Readiness::NeedsSetup; }
};

/**
 * Decide from the contents of config.json.
 *
 * @param config  the parsed file, or nullopt when it is absent
 * @param parse_failed  true when the file EXISTS but could not be parsed.
 *        Distinguished because it is the one state where the supervisor must
 *        not quietly write over the file: the user hand-edited it and still
 *        wants what is in there, so the wizard has to offer that choice rather
 *        than assume it.
 */
GateResult evaluateConfig(const std::optional<nlohmann::json>& config,
                          bool parse_failed = false);

}  // namespace cpapdash::supervisor
