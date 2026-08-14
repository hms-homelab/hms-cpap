#pragma once

#include <QString>

#include "PreflightReport.h"

namespace cpapdash::supervisor {

/**
 * Runs `hms_cpap --preflight` and parses what it says.
 *
 * Synchronous and bounded. It is called before a start and while nothing else
 * is happening, it normally takes well under a second, and the alternative --
 * an async result the UI has to hold state for -- buys nothing at that
 * duration.
 *
 * @param exe          absolute path to hms_cpap
 * @param config_path  optional --config override. This is what lets the
 *                     configurator validate a CANDIDATE config written to a
 *                     temp file, without touching the user's real one, so the
 *                     wizard's Next button can be gated on the real validator
 *                     instead of a second opinion reimplemented in Qt.
 *
 * Two things about preflight that shape how the caller may use it: it CREATES
 * the directories it checks for writability, and it really binds the port. So
 * it is the service-DOWN validator only -- running it while the child is alive
 * always fails web_port, on our own listener.
 */
PreflightReport runPreflight(const QString& exe,
                             const QString& config_path = {},
                             int timeout_ms = 30000);

}  // namespace cpapdash::supervisor
