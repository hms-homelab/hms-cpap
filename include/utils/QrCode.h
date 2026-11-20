#pragma once
//
// SDD-073 D3: a QR code of this computer's address, for the person typing it
// on their phone. One encoder here serves both setup front ends (the desktop
// app and the web wizard ask GET /api/sleepbridge/qr), so the two can never
// draw different codes for the same address.
//
// Byte mode, error correction level M, versions 1 to 10 (up to 213 bytes,
// far beyond an address). The mask is chosen by the standard's penalty rules.
// Pure: no I/O.
//
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap::qr {

/// The modules, row by row, true for dark. No quiet zone: the caller draws
/// at least four light modules around it.
struct Code {
    int version{0};
    int size{0};
    int mask{0};
    std::vector<std::vector<bool>> modules;
};

/// The code for `text` (its bytes, as given), or nullopt when it is empty or
/// longer than version 10 at level M holds.
std::optional<Code> encode(const std::string& text);

/// The rows as strings of '1' (dark) and '0', for the JSON answer.
std::vector<std::string> rows(const Code& code);

}  // namespace hms_cpap::qr
