#pragma once

#include <string>

namespace hms_cpap {

/**
 * SDD-016: the database probe, reachable without an HTTP server.
 *
 * The Qt supervisor has to answer "can I actually open this database?" at two
 * moments, and the existing HTTP endpoints answer at NEITHER of them:
 *
 *   - First run. The configurator is what the user sees BEFORE hms_cpap has
 *     ever started, so there is no server on 8893 to ask.
 *   - After setup. `/api/setup/test-db` and `/api/setup/create-db` return 403
 *     once `setup_complete` is true, so the settings dialog cannot re-check
 *     credentials on an installed system.
 *
 * So the supervisor runs the probe the same way it already runs `--preflight`:
 * spawn the service binary, hand it a request, read a report, look at the exit
 * code. One process contract for both, and one implementation of "reachable"
 * rather than a second copy compiled into a GUI that could disagree with the
 * service about what it means.
 *
 * The alternative -- linking SetupService into the supervisor -- would drag
 * libpqxx and libmysql into a desktop process and create exactly that second
 * opinion. Rejected deliberately.
 *
 * Request JSON (both modes; unknown keys ignored, every key optional):
 *
 *     {"type":"postgresql","host":"...","port":5432,"name":"...",
 *      "user":"...","password":"...","sqlite_path":"..."}
 *
 * `--create-db` additionally reads `admin_user` and `admin_password`. Those are
 * request-scoped: never persisted, never echoed back, never logged. They exist
 * only for the duration of the call, matching the HTTP handler.
 *
 * Response JSON, identical to what POST /api/setup/test-db returns so the two
 * paths cannot drift:
 *
 *     {"ok":true,"error":"","schema_present":false,"session_count":0}
 *
 * On a malformed request or an unsupported backend the response carries `ok`
 * false and a human-readable `error`, rather than an HTTP status the caller
 * would have to translate.
 */
struct DbProbeCliResult {
    std::string json;      ///< the response body, always valid JSON
    int exit_code = 1;     ///< 0 when ok, 1 otherwise -- same contract as --preflight
};

/**
 * Run a database probe from a JSON request.
 *
 * @param request_json  the request body, as read from stdin
 * @param provision     false = probe only (test-db); true = create then probe
 *                      (create-db)
 *
 * Never throws: a parse failure is a result with `ok` false, because the caller
 * is a GUI that has to render something either way.
 */
DbProbeCliResult runDbProbeCli(const std::string& request_json, bool provision);

}  // namespace hms_cpap
