#pragma once

#include <cpapdash/parser/EDFWriter.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hms_cpap {

// The BRP, PLD and SAD files of an AirSense 11 night rebuilt from its
// Bluetooth stream, scaled the way the machine's own card files are.
//
// The parser library's writeResMedBRP/PLD/SAD spread every channel across the
// whole int16 range (digital -32768..32767 for a physical 0..40). A reader
// that takes a value as raw * gain, with no offset, as ResMed's files allow it
// to because their digital range starts where the physical one does, then
// reads every value shifted by half the range: a respiratory rate of 16 bpm
// came out as -14 and OSCAR dropped it as out of range. Flow was in L/min
// where the card has L/s, which the library's own reader multiplies by 60 for
// all ResMed files, so a Bluetooth night's flow was stored 60 times too large.
//
// Here each channel has the card's physical range and a digital range that
// puts physical zero at digital zero (flow: -2..3 L/s over -1000..1500, whose
// zero is at the same place), so raw * gain is the value whatever the reader
// does about the offset. Record lengths and sample counts are the library's.
//
// The card's files also carry three checksums, all CRC-16/CCITT-FALSE (polynomial
// 0x1021, initial 0xFFFF, no reflection, no final XOR), checked against 108 files
// of one machine's card (BRP, PLD, SA2, EVE, CSL):
//
//   - each data record ends in a "Crc16" sample: the CRC of the record's own
//     data bytes (every signal's samples before it, as stored);
//   - the patient field reads "X X X X <a> <b>", <a> the CRC of the main
//     header's bytes 25 to 255 (the recording field, dates and counts: what
//     follows the tokens) and <b> the CRC of the signal headers (bytes 256 to
//     the end of the header), in four upper-case hex digits.
//
// The files here carry them the same way, and the recording field carries the
// machine's MID= and VID= (model_id, vid; left out when 0).
//
// The oximetry file is the 11 series' own: <start>_SA2.edf, Pulse.1s then SpO2.1s
// then Crc16, and a second with no reading is -1 (stored outside the signal's
// declared range, as the card stores it), not 0.
//
// Pure functions of their input, so a test can decode what they write.

std::vector<uint8_t> writeCardBRP(const cpapdash::parser::StreamedBRP& brp, int model_id = 0, int vid = 0);
std::vector<uint8_t> writeCardPLD(const cpapdash::parser::StreamedPLD& pld, int model_id = 0, int vid = 0);
std::vector<uint8_t> writeCardSAD(const cpapdash::parser::StreamedSAD& sad, int model_id = 0, int vid = 0);

/// CRC-16/CCITT-FALSE.
uint16_t crc16CcittFalse(const uint8_t* data, size_t n);

/// CRC-32 (the zlib one: reflected 0xEDB88320, initial and final 0xFFFFFFFF).
uint32_t crc32(const uint8_t* data, size_t n);

/// A card keeps the CRC-32 of Identification.json and of SETTINGS/
/// CurrentSettings.json beside each, in Identification.crc and
/// CurrentSettings.crc: four bytes, the CRC little-endian (both of one card's
/// pairs check). Writes <json path without .json>.crc for the file as it is
/// now. False when the file cannot be read or the CRC written.
bool writeCrcFor(const std::string& json_path);

/// writeCrcFor() unless that file is there already (a card's own is kept).
bool ensureCrcFor(const std::string& json_path);

/// Sets the two header checksums in the patient field of an EDF whose signal
/// headers are in place, as above. Leaves anything it cannot read as it is.
void stampHeaderChecksums(std::vector<uint8_t>& edf);

}  // namespace hms_cpap
