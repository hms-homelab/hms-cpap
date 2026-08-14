#include "utils/DbProbeCli.h"

#include "services/SetupService.h"

#include <nlohmann/json.hpp>

namespace hms_cpap {

namespace {

/// Reads a key as a string whatever the caller wrote it as.
///
/// A GUI spin box hands back a number and a text field hands back a string, and
/// both are legitimate ways to say "port 5432". Refusing one of them would make
/// the contract depend on which widget the caller happened to use.
std::string str(const nlohmann::json& j, const char* key,
                const std::string& fallback = "") {
    if (!j.contains(key) || j[key].is_null()) return fallback;
    const auto& v = j[key];
    if (v.is_string())          return v.get<std::string>();
    if (v.is_number_integer())  return std::to_string(v.get<long long>());
    if (v.is_boolean())         return v.get<bool>() ? "true" : "false";
    return fallback;
}

int intval(const nlohmann::json& j, const char* key, int fallback = 0) {
    if (!j.contains(key) || j[key].is_null()) return fallback;
    const auto& v = j[key];
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_string()) {
        try { return std::stoi(v.get<std::string>()); } catch (...) { return fallback; }
    }
    return fallback;
}

DbProbeCliResult fail(const std::string& message) {
    nlohmann::json out;
    out["ok"]             = false;
    out["error"]          = message;
    out["schema_present"] = false;
    out["session_count"]  = 0;
    return {out.dump(2) + "\n", 1};
}

DbProbeCliResult render(const SetupService::DbProbe& p) {
    nlohmann::json out;
    out["ok"]             = p.ok;
    out["error"]          = p.error;
    out["schema_present"] = p.schema_present;
    out["session_count"]  = p.session_count;
    return {out.dump(2) + "\n", p.ok ? 0 : 1};
}

}  // namespace

DbProbeCliResult runDbProbeCli(const std::string& request_json, bool provision) {
    nlohmann::json req;
    try {
        req = nlohmann::json::parse(request_json);
    } catch (const std::exception& e) {
        return fail(std::string("the request is not valid JSON: ") + e.what());
    }
    if (!req.is_object())
        return fail("the request must be a JSON object");

    // test-db defaults to sqlite because that is the zero-configuration answer
    // and the wizard's first screen offers it. create-db has no such default:
    // there is no server to create a database on, so an omitted type is a
    // caller mistake worth naming rather than a silent sqlite no-op.
    const std::string type = str(req, "type", provision ? "" : "sqlite");
    if (type.empty())
        return fail("no database type given; expected postgresql or mysql");
    if (!SetupService::supportsBackend(type))
        return fail("this build cannot open '" + type + "'");

    if (provision) {
        // Admin credentials live only inside this call: they are not written to
        // config.json, not echoed in the response, and not logged.
        return render(SetupService::provisionDatabase(
            type,
            str(req, "host"), intval(req, "port"),
            str(req, "name"), str(req, "user"), str(req, "password"),
            str(req, "admin_user"), str(req, "admin_password")));
    }

    return render(SetupService::probeDatabase(
        type,
        str(req, "host"), intval(req, "port"),
        str(req, "name"), str(req, "user"), str(req, "password"),
        str(req, "sqlite_path")));
}

}  // namespace hms_cpap
