#pragma once

#include "clients/IAirSense11Pairing.h"

#include <json/json.h>

#include <string>

namespace hms_cpap::as11api {

/**
 * The Settings page's AirSense 11 pairing endpoints (SDD-057), as plain
 * functions over IAirSense11Pairing so the answers are tested without a web
 * server or Bluetooth. CpapController only routes to them.
 *
 *   GET  /api/airsense11/ble/status
 *        {compiled, enabled, available, paired, pairing}
 *   POST /api/airsense11/ble/pair/start
 *        {ok, state: "wait_code" | "failed", result, error?}
 *   POST /api/airsense11/ble/pair/confirm   {code}
 *        {ok, state: "paired" | "failed", result, error?}
 *   POST /api/airsense11/ble/pair/cancel
 *        {ok, state: "cancelled"}
 *
 * `result` is a stable code the page translates (ok, wait_code, not_paired,
 * connect, radio_off, verify, protocol, spool, not_compiled, disabled,
 * no_pairing, bad_code); `error` is the English detail, for the log.
 * radio_off is the host's own Bluetooth held off by rf-kill, which is not
 * the machine being out of range.
 */
struct Reply {
    int status = 200;
    Json::Value body;
};

/// The stable code for a result, as `result` carries it.
const char* resultCode(as11::Result r);

/// A code typed by a person: spaces dropped, digits only, 4 to 8 of them.
/// Empty when it is not one.
std::string normaliseCode(const std::string& typed);

Reply status(const IAirSense11Pairing* p, bool compiled, bool enabled);
Reply start(IAirSense11Pairing* p, bool compiled, bool enabled);
Reply confirm(IAirSense11Pairing* p, bool compiled, bool enabled, const std::string& code);
Reply cancel(IAirSense11Pairing* p);

} // namespace hms_cpap::as11api
