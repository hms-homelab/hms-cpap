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
/// when both are there, unless the two name different machines and the newest
/// night's EDF header names the .tgt's: a card moved from one machine to
/// another keeps the first one's file (#94: an AirSense 10's card put in an
/// AirSense 11 holds both, each naming its own serial). With neither, the
/// serial from the header of an EDF in the newest DATALOG night (its recording
/// field's SRN=), manufacturer ResMed and no model: a Sleep Bridge's rebuilt
/// card has no identification file.
/// Nothing readable: an empty identity, no error.
MachineIdentity readIdentification(const std::string& card_root);

/// The machine an AirSense 11's own Bluetooth identified: the newest
/// <data_dir>/as11/<serial>/Identification.json, written when hms-cpap reads
/// the machine directly or a Mule & Miner streams it here. For an install with
/// no card to name the machine. Empty when there is none.
MachineIdentity readBluetoothIdentification(const std::string& data_dir);

/// What a parsed session says about the machine that wrote it.
MachineIdentity identityFromSession(const CPAPSession& session);

/// The machine this install reads: the card in [local_dir] when the source is
/// a local folder (a Sleep Bridge's folder is one), the archive otherwise
/// ([archive_dir], or <data_dir>/cpap_data when unset: where ez Share and
/// FlashAir land the card's root files), and with no card to name it the
/// AirSense 11 that named itself over Bluetooth under [data_dir].
MachineIdentity installIdentity(const std::string& local_dir, const std::string& archive_dir,
                                const std::string& data_dir);

/// #94: the name the dashboard and the report cover give the machine. The
/// configured device_name when someone chose one; when it is empty or one of
/// the names the service filled in itself ("CPAP" on a new install, "ResMed
/// AirSense 10" on a config.json that omits the key, which is every config the
/// desktop setup wizard writes), the machine's own manufacturer and model,
/// and "CPAP" when the machine has not named its model.
std::string machineName(const std::string& configured, const MachineIdentity& id);

}  // namespace hms_cpap
