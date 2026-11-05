#pragma once
//
// GET /api/devices: every device this hms-cpap reads or talks to, in one
// answer, so the Devices page makes one call per refresh (SDD-062 D8).
//
//   {"devices": [{"kind": "mule"|"sleepbridge"|"o2ring"|"ezshare"|"flashair"|"as11_ble",
//                 "id", "key", "name", "default_name", "own_name",
//                 "present": "yes"|"no"|"unknown", "last_heard",
//                 "recording": "yes"|"no"|"unknown", "recording_reason",
//                 "version", "forgettable", "detail": {...the kind's own}}],
//    "stream_port"}
//
// Built from what each service already reports (the inputs below), so it is
// a pure function the suite drives with fakes. Every kind says "unknown"
// rather than guess. A card for each kind configured or ever connected,
// nothing for a kind not in use (D2). Order: here now first, then by last
// heard, never heard last. A forgotten device is left out until it is heard
// from after it was forgotten; then its forgotten mark is cleared.
//
// Commands stay on each kind's own routes.
//
#include "devices/DevicesStore.h"

#include <json/json.h>

#include <ctime>
#include <map>
#include <string>

namespace hms_cpap::devices {

struct DeviceInputs {
    std::time_t now{0};

    // Mule & Miner: GET /api/mule, and per unit its firmware (latest,
    // update_available, manifest error, the job). Null: the stream is off.
    Json::Value                        mule{Json::nullValue};
    std::map<std::string, Json::Value> mule_firmware;

    // Sleep Bridge: GET /api/sleepbridge. Null: the stream is off.
    Json::Value sleepbridge{Json::nullValue};

    // The card source (Settings): transport ezshare, flashair or local.
    std::string transport;
    std::string source_url;
    std::string format;
    bool        ezshare_range{false};
    // What the bursts found (BurstCollectorService::CardFacts as JSON:
    // last_attempt, answering, last_answered, health, consecutive_failures).
    Json::Value source{Json::nullValue};
    // The last Test of this address, when it named a Mule & Miner (D11).
    Json::Value source_probe{Json::nullValue};

    // The O2 ring (Settings), and what the last talk with it found.
    bool        o2_enabled{false};
    std::string o2_mode;
    std::string o2_url;
    bool        ble_compiled{false};
    Json::Value o2{Json::nullValue};   // OximetryService::Facts as JSON

    // The AirSense 11 over this host's Bluetooth (Settings) and its status
    // (GET /api/airsense11/ble/status, the pull's facts included).
    bool        as11_enabled{false};
    std::string as11_address;
    int         as11_pull_hours{6};
    Json::Value as11{Json::nullValue};

    // The latest night in the ledger: "yes" while it is live, "no", or
    // "unknown" when the ledger cannot be read.
    std::string ledger_live{"unknown"};

    int stream_port{8894};
};

/// The answer above. `store` supplies the names and the forgotten marks, and
/// has a mark cleared when a forgotten device is heard from again.
Json::Value buildDevices(const DeviceInputs& in, DevicesStore& store);

/// The kinds a device can be renamed under, and the two that can be forgotten.
bool knownKind(const std::string& kind);
bool forgettableKind(const std::string& kind);

}  // namespace hms_cpap::devices
