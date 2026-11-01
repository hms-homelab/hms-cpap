#pragma once

#include <cpapdash/parser/EDFWriter.h>

#include <cstdint>
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
// Pure functions of their input, so a test can decode what they write.

std::vector<uint8_t> writeCardBRP(const cpapdash::parser::StreamedBRP& brp);
std::vector<uint8_t> writeCardPLD(const cpapdash::parser::StreamedPLD& pld);
std::vector<uint8_t> writeCardSAD(const cpapdash::parser::StreamedSAD& sad);

}  // namespace hms_cpap
