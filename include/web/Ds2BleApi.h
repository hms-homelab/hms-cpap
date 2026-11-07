#pragma once

#include "clients/Ds2BleReader.h"

#include <json/json.h>

#include <functional>
#include <string>

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
 *           read_at, next_read_at}
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

/// What the caller adds to the status: the scheduled time of day and the
/// next automatic read (epoch s, 0 = none).
struct Schedule {
    std::string read_at;
    long long next_read_at = 0;
};

Reply status(const Ds2BleReader* r, bool compiled, bool enabled, const Schedule& sched);
Reply pairStart(Ds2BleReader* r, bool compiled, bool enabled);
Reply pairConfirm(Ds2BleReader* r, const Json::Value& body);
Reply pairCancel(Ds2BleReader* r);
/// Starts a read now, off the request's thread: `start` hands it to the
/// collector (false when it could not). The page follows it on the status.
Reply read(Ds2BleReader* r, bool compiled, bool enabled, const std::function<bool()>& start);
Reply forget(Ds2BleReader* r);

}  // namespace hms_cpap::ds2api
