#pragma once
//
// SDD-073 D4, D8, D10: "Use this bridge as my source". One code path for the
// installer's waiting step, the web wizard, the Devices page and Settings >
// Source: POST /api/sleepbridge/<bridge>/use asks bridgeSource() what to set,
// and sets it.
//
// What it sets: the Local folder source at the bridge's own card folder, read
// with the card format of the machine the bridge's card was recognised as
// (`card_format`: "resmed" or "bmc"; a folder holding P-SERIES/ is "philips",
// SDD-080 D1, and one holding a Sefam card is "sefam"). Before the rebuild has
// recognised it,
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
    std::string format;          // "resmed" | "bmc" | "philips" (SDD-080 D1) | "sefam"
    bool        format_known{false};   // from the bridge's card or the learned card's brand
};

/// The source for `sender`, from GET /api/sleepbridge's body; nullopt with
/// `why` when the bridge is not listed or has no folder.
std::optional<BridgeSource> bridgeSource(const Json::Value& status, const std::string& sender, std::string* why);

/// "resmed", "bmc", "philips" or "sefam" for a learned card's brand label, ""
/// when it names none of them.
std::string formatForBrand(const std::string& brand);

/// SDD-080 D1: [dir] holds a Philips card (P-SERIES/<machine>/PROP.BIN or
/// PROP.TXT), as a Sleep Bridge on a DreamStation 2 rebuilds one.
bool holdsPhilipsCard(const std::string& dir);

/// [dir] holds a Sefam S.Box or SleepBox card: a session folder whose
/// manifest the Sefam reader accepts, as the upload page recognises one. The
/// rebuild names ResMed and Luna cards only, so a bridge in an S.Box is known
/// by the files it rebuilt.
bool holdsSefamCard(const std::string& dir);

/// {"t":"sleepbridge","event":<event>,"sender":<sender>,"status":<status>}.
/// Events: snapshot (on connect), hello (a bridge's own hello, never a
/// probe), card (a card statement stored), closed, rebuilt (a rebuild wrote
/// files or recognised the machine).
std::string bridgeWatchFrame(const std::string& event, const std::string& sender, const Json::Value& status);

/// {"t":"cpapdash","event":<snapshot|state>,"stream":<GET /api/cpapdash/stream>}.
std::string streamWatchFrame(const std::string& event, const Json::Value& stream);

}  // namespace hms_cpap::sleepbridge
