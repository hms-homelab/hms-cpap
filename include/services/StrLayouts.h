#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * The STR layouts built into the binary (cmake/StrLayouts.cmake, from
 * str-layouts/<product code>.edfh). A layout is an STR.edf header with no
 * records, cut from a card of that model with the patient and recording
 * fields blanked: the signals, their ranges and their order, which is what
 * laying a Bluetooth night out as the card would takes.
 */
struct BuiltinStrLayout {
    int                  product_code;
    const unsigned char* bytes;
    size_t               size;
};

/// Every built-in layout, by product code, in ascending order.
const BuiltinStrLayout* builtinStrLayouts(size_t* count);

/// The layout for one product code: the file <layout_dir>/<code>.edfh when
/// one is there (an owner's own cut wins), else the built-in one; empty when
/// neither exists.
std::vector<uint8_t> strLayoutFor(int product_code, const std::string& layout_dir);

}  // namespace hms_cpap
