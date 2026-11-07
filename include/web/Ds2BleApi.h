#pragma once

#include "clients/Ds2BleReader.h"

#include <json/json.h>

#include <functional>
#include <string>
#include <vector>

namespace hms_cpap::ds2api {

/**
 * The Devices page's DreamStation 2 endpoints (SDD-064), as plain functions
 * over Ds2BleReader so the answers are tested without a web server or
 * Bluetooth. CpapController only routes to them.
 *
 *   GET    /api/ds2/ble/status
 *          {compiled, enabled, backend, os_dialog, paired, busy,
 *           machine: {name, address, serial, model, firmware, newest_session} | null,
 *           pairing: {state, code, seconds_left, machine_name, error},
 *           last_read: {at, attempt_at, records, error} | null,
 *           import_from, first_read_done,
 *           trigger: "night_end" | "manual", night_sources: [...],
 *           read_mode: "daily" | "manual", read_at: "HH:MM", next_read_at}
 *   POST   /api/ds2/ble/import-from    {date} -> {ok, import_from}
 *   POST   /api/ds2/ble/pair/start     {ok, state}; 409 while busy
 *   POST   /api/ds2/ble/pair/confirm   {match: bool} -> {ok, state}; 409 when no code waits
 *   POST   /api/ds2/ble/pair/cancel    {ok, state: "cancelled"}
 *   POST   /api/ds2/ble/read           202 {ok, result: "reading"}; 409 while busy or not paired
 *   DELETE /api/ds2/ble/pairing        {ok, unpaired_os}; 409 while busy
 *
 * `state` and `result` are stable codes the page translates; `error` is the
 * English detail, for the log.
 */
struct Reply {
    int status = 200;
    Json::Value body;
};

/// What starts a read besides "Read now" (SDD-064 D5): another source this
/// install has saying a night ended. `sources` are the ones that can say it
/// here (e.g. "sleep_bridge", "o2ring"); none: only "Read now".
struct Trigger {
    std::vector<std::string> sources;
    // D5a: the owner's choice ("daily" at read_at, or "manual") and the next
    // daily read (epoch s, 0 with "manual" or nothing paired).
    std::string read_mode = "daily";
    std::string read_at = "11:00";
    long long next_read_at = 0;
};

Reply status(const Ds2BleReader* r, bool compiled, bool enabled, const Trigger& trigger);
/// POST /api/ds2/ble/import-from {date: "YYYY-MM-DD"}: nights from that local
/// day on are imported (D9). 400 for a bad date, 409 when nothing is paired.
Reply setImportFrom(Ds2BleReader* r, const Json::Value& body);
Reply pairStart(Ds2BleReader* r, bool compiled, bool enabled);
Reply pairConfirm(Ds2BleReader* r, const Json::Value& body);
Reply pairCancel(Ds2BleReader* r);
/// Starts a read now, off the request's thread: `start` hands it to the
/// collector (false when it could not). The page follows it on the status.
Reply read(Ds2BleReader* r, bool compiled, bool enabled, const std::function<bool()>& start);
Reply forget(Ds2BleReader* r);

}  // namespace hms_cpap::ds2api
