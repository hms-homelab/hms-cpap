// SDD-074: the clock setting's API (see include/web/ClockApi.h).
#include "web/ClockApi.h"

#include "database/SqlDialect.h"
#include "utils/MachineClock.h"
#include "utils/MachineClocks.h"
#include "utils/MachineZone.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace hms_cpap::clockapi {

namespace {

const char* kindOf(const MachineZone& z) {
    switch (z.kind()) {
        case MachineZone::Kind::Host:  return "host";
        case MachineZone::Kind::Fixed: return "fixed";
        case MachineZone::Kind::Named: return "zone";
    }
    return "host";
}

std::string stampOf(long long now) {
    const std::tm t = wall::fieldsOf(now);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

}  // namespace

Json::Value view(IDatabase& db, const std::string& device_id, long long now, bool learnable) {
    const MachineZone zone = machine_clocks::zoneFor(device_id);
    const MachineZone host = MachineZone::host();
    Json::Value v(Json::objectValue);
    v["device_id"]       = device_id;
    v["setting"]         = zone.setting();
    v["kind"]            = kindOf(zone);
    v["offset_now"]      = static_cast<Json::Int64>(zone.offsetAt(now));
    v["offset_text"]     = MachineZone::offsetText(zone.offsetAt(now));
    v["host_zone"]       = hostZoneName();
    v["host_offset_now"] = static_cast<Json::Int64>(host.offsetAt(now));
    v["host_offset_text"] = MachineZone::offsetText(host.offsetAt(now));
    v["learned"]         = Json::Value(Json::nullValue);
    v["difference_s"]    = 0;
    v["warn"]            = false;
    if (learnable) {
        const MachineClock c = loadMachineClock(db, device_id);
        if (c.zone_known) {
            Json::Value l(Json::objectValue);
            l["offset"] = static_cast<Json::Int64>(c.zone_s);
            l["text"]   = MachineZone::offsetText(c.zone_s);
            l["at"]     = static_cast<Json::Int64>(machineClockLearnedAt(db, device_id));
            v["learned"] = l;
            const long long diff = std::llabs(c.zone_s - zone.offsetAt(now));
            v["difference_s"] = static_cast<Json::Int64>(diff);
            v["warn"]         = diff >= kWarnDifference;
        }
    }
    return v;
}

Result set(IDatabase& db, const std::string& device_id, const std::string& setting, long long now,
           bool learnable) {
    Result r;
    if (device_id.empty()) {
        r.status = 400;
        r.body["ok"] = false;
        r.body["error"] = "no device_id";
        return r;
    }
    std::string why;
    if (!machine_clocks::save(db, device_id, setting, &why)) {
        r.status = 400;
        r.body["ok"] = false;
        r.body["error"] = why;
        return r;
    }
    r.body["ok"]    = true;
    r.body["clock"] = view(db, device_id, now, learnable);
    return r;
}

Json::Value zones() {
    Json::Value v(Json::objectValue);
    Json::Value list(Json::arrayValue);
    for (const auto& z : zonedb::names()) list.append(z);
    v["zones"]  = list;
    v["source"] = zonedb::source();
    return v;
}

const std::vector<std::string>& sessionTables() {
    static const std::vector<std::string> t = {
        "cpap_session_files", "cpap_session_metrics", "cpap_breathing_summary", "cpap_events",
        "cpap_breaths",       "cpap_vitals",          "cpap_calculated_metrics"};
    return t;
}

std::vector<std::string> backupRows(IDatabase& db, const std::string& device_id, long long now,
                                    std::string* why) {
    const DbType dt = db.dbType();
    const std::string stamp = stampOf(now);
    std::vector<std::string> made;
    auto copy = [&](const std::string& table, const std::string& where) -> bool {
        const std::string bk = table + "_bk_" + stamp;
        // A table this engine never created has nothing to keep.
        if (!db.executeQueryChecked("SELECT 1 FROM " + table + " WHERE 1 = 0", {}).ok) return true;
        if (!db.executeQueryChecked("CREATE TABLE " + bk + " AS SELECT * FROM " + table + " WHERE 1 = 0", {})
                 .ok ||
            !db.executeQueryChecked("INSERT INTO " + bk + " SELECT * FROM " + table + " WHERE " + where,
                                    {device_id})
                 .ok) {
            if (why) *why = "could not back up " + table;
            return false;
        }
        made.push_back(bk);
        return true;
    };
    const std::string p = sql::param(1, dt);
    if (!copy("cpap_sessions", "device_id = " + p)) return {};
    for (const auto& t : sessionTables())
        if (!copy(t, "session_id IN (SELECT id FROM cpap_sessions WHERE device_id = " + p + ")")) return {};
    if (!copy("cpap_daily_summary", "device_id = " + p)) return {};
    return made;
}

}  // namespace hms_cpap::clockapi
