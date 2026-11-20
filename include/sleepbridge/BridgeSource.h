#pragma once
//
// SDD-073 D4, D8, D10: "Use this bridge as my source". One code path for the
// installer's waiting step, the web wizard, the Devices page and Settings >
// Source: POST /api/sleepbridge/<bridge>/use asks bridgeSource() what to set,
// and sets it.
//
// What it sets: the Local folder source at the bridge's own card folder, read
// with the card format of the machine the bridge's card was recognised as
// (`card_format`: "resmed" or "bmc"). Before the rebuild has recognised it,
// the card learned on this computer names its brand (a label, but the only
// word there is); with neither, ResMed, this build's default format.
//
// Also the frames of the setup watch socket (GET ws .../api/sleepbridge/watch
// on the web port), which the setup steps wait on: one per event, never on a
// clock.
//
#include <json/json.h>

#include <optional>
#include <string>

namespace hms_cpap::sleepbridge {

struct BridgeSource {
    std::string local_dir;
    std::string format;          // "resmed" | "bmc"
    bool        format_known{false};   // from the bridge's card or the learned card's brand
};

/// The source for `sender`, from GET /api/sleepbridge's body; nullopt with
/// `why` when the bridge is not listed or has no folder.
std::optional<BridgeSource> bridgeSource(const Json::Value& status, const std::string& sender, std::string* why);

/// "resmed" or "bmc" for a learned card's brand label, "" when it names
/// neither.
std::string formatForBrand(const std::string& brand);

/// {"t":"sleepbridge","event":<event>,"sender":<sender>,"status":<status>}.
/// Events: snapshot (on connect), hello (a bridge's own hello, never a
/// probe), card (a card statement stored), closed, rebuilt (a rebuild wrote
/// files or recognised the machine).
std::string bridgeWatchFrame(const std::string& event, const std::string& sender, const Json::Value& status);

/// {"t":"cpapdash","event":<snapshot|state>,"stream":<GET /api/cpapdash/stream>}.
std::string streamWatchFrame(const std::string& event, const Json::Value& stream);

}  // namespace hms_cpap::sleepbridge
