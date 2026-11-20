#pragma once
//
// SDD-074: the "Machine clock" (and "Ring clock") on the Devices page.
//
//   GET  /api/clock/zones                 {"zones": [...], "source"}: the OS zone database
//   PUT  /api/devices/clock               {"device_id", "setting"}: "" (this computer),
//                                          "+10:00", or a zone name; 400 with the reason
//                                          when it is not one
//   POST /api/devices/clock/reread        {"device_id"}: back up the device's rows, then
//                                          read every archived night again (D3, Albin's
//                                          decision 3)
//
// and, on GET /api/devices, a "clock" object on each machine's card and on the
// ring's (view() below). Pure functions over IDatabase, so the suite covers
// them on every engine; the routes only route.
//
#include "database/IDatabase.h"

#include <json/json.h>

#include <string>
#include <vector>

namespace hms_cpap::clockapi {

/// The learned offset and the effective one differ by this much or more: the
/// Devices page warns (Albin's decision 1).
constexpr long long kWarnDifference = 15 * 60;

/// {"device_id", "setting", "kind": "host"|"fixed"|"zone", "offset_now",
///  "host_zone", "host_offset_now", "learned": {"offset", "text", "at"} | null,
///  "difference_s", "warn"}
Json::Value view(IDatabase& db, const std::string& device_id, long long now, bool learnable);

struct Result {
    int status{200};
    Json::Value body{Json::objectValue};
};

/// Store a device's setting (machine_clocks::save) and answer its new view.
Result set(IDatabase& db, const std::string& device_id, const std::string& setting, long long now,
           bool learnable);

/// {"zones": [...], "source": "zoneinfo:/usr/share/zoneinfo" | "std::chrono::tzdb"}
Json::Value zones();

/// The tables a machine's nights live in, backed up before a re-read.
const std::vector<std::string>& sessionTables();

/// Copy every row of [device_id]'s nights into "<table>_bk_<stamp>" tables
/// (created empty from the table, then filled), the same on every engine.
/// Returns the names created; empty with [why] when any copy failed.
std::vector<std::string> backupRows(IDatabase& db, const std::string& device_id, long long now,
                                    std::string* why);

}  // namespace hms_cpap::clockapi
