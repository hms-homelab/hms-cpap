#pragma once
//
// Whether the machine behind a device is recording now, and the warning a
// person sees before a command or an update that can cost the night.
//
// Each kind judges recording from its own facts (a Mule & Miner's stream, a
// Sleep Bridge's sector writes, the night ledger for a card) and says
// "unknown" rather than guess. For the warning, unknown counts as recording:
// when in doubt, the person is told.
//
// The warning never holds anything (SDD-062 D3, D4). A warned command posted
// without "confirm": true while the machine records (or may) answers
//
//   409 {"ok": false, "warning": "recording", "reason": "<device's terms>",
//        "confirm": true, "error": "<the same, for a script's log>"}
//
// and sends nothing; posted again with "confirm": true it goes at once. Not
// recording: it goes at once, no warning. Header-only: the device services
// and their socket test share it.
//
#include <json/json.h>

#include <string>

namespace hms_cpap::devices {

struct Recording {
    std::string state{"no"};   // yes, no, unknown
    std::string reason;        // in the device's own terms; empty when "no"

    /// Whether the warning applies: yes, or unknown.
    bool warns() const { return state != "no"; }
};

/// `{"confirm": true}` in a request body: the person saw the warning.
inline bool confirmed(const Json::Value& req) {
    return req.isObject() && req["confirm"].isBool() && req["confirm"].asBool();
}

/// The 409 body for a warned command while `r` warns.
inline Json::Value recordingWarning(const Recording& r) {
    Json::Value v(Json::objectValue);
    v["ok"]      = false;
    v["warning"] = "recording";
    v["reason"]  = r.reason;
    v["recording"] = r.state;
    v["confirm"] = true;
    v["error"]   = "the machine " + std::string(r.state == "yes" ? "is" : "may be") +
                 " recording (" + r.reason + "): send again with \"confirm\": true to go ahead";
    return v;
}

}  // namespace hms_cpap::devices
