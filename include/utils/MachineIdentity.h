#pragma once
//
// SDD-050: the machine says what it is.
//
// Home Assistant's device card used to say "AirSense 10" for every machine,
// an AirCurve, a Prisma or an S.Box alike. The card names its machine, so the
// model, serial and firmware come from there, and a field the card does not
// give is left out rather than filled with somebody else's machine.
//
// ResMed: the card-root identification file. Identification.json on the 11
//         series, Identification.tgt (lines of "#KEY value") before it.
// Others: what their parser read off the card into the session.
//
#include "parsers/CpapdashBridge.h"

#include <string>

namespace hms_cpap {

struct MachineIdentity {
    std::string manufacturer;
    std::string model;
    std::string serial;
    std::string firmware;

    bool empty() const {
        return manufacturer.empty() && model.empty() && serial.empty() && firmware.empty();
    }
    bool operator==(const MachineIdentity& o) const {
        return manufacturer == o.manufacturer && model == o.model &&
               serial == o.serial && firmware == o.firmware;
    }
    bool operator!=(const MachineIdentity& o) const { return !(*this == o); }
};

/// "AirCurve11VAuto" -> "AirCurve 11 VAuto", "AirSense_10_AutoSet" ->
/// "AirSense 10 AutoSet". Underscores become spaces, and a space goes where
/// letters meet digits. Anything else is passed through as written.
std::string readableModel(const std::string& product_name);

/// The ResMed identification file in [card_root]. Identification.json wins
/// when both are there. Absent or unreadable: an empty identity, no error.
MachineIdentity readIdentification(const std::string& card_root);

/// What a parsed session says about the machine that wrote it.
MachineIdentity identityFromSession(const CPAPSession& session);

}  // namespace hms_cpap
