// TestData.h
//
// Recorded machine data the parser and integration tests read when it is
// there: real EDF nights and Lowenstein exports are not in the repository.
// HMS_CPAP_TEST_DATA names a folder laid out as
//
//     edf/                              STR.edf and the other EDF fixtures
//     asv/                              an ASV night (BRP, PLD, SA2, STR.edf)
//     lowenstein_samples/extracted_therapy/
//
// Unset (the default), only the paths relative to the build folder are
// tried, and a test whose data is absent skips as it always has.
#pragma once

#include <cstdlib>
#include <string>

/// <HMS_CPAP_TEST_DATA>/<rel>, or "" when the variable is unset.
inline std::string testDataPath(const std::string& rel) {
    const char* root = std::getenv("HMS_CPAP_TEST_DATA");
    if (!root || !*root) return "";
    std::string out(root);
    if (out.back() != '/' && out.back() != '\\') out += '/';
    return out + rel;
}
