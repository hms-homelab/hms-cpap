#pragma once

#include <string>
#include <cpapdash/parser/VLDParser.h>

namespace hms_cpap {

/**
 * Writes a stored OximetrySession back out as the CSV a Wellue / Viatom ring's
 * own app exports, which is the file SleepHQ accepts for oximetry.
 *
 * The exact inverse of O2RingCsvParser, and deliberately narrower than it. The
 * parser accepts two dialects because both exist in the wild:
 *
 *     06:53:07 Apr 12 2026        24-hour, unquoted        (Checkme O2 Max)
 *     "11:20:29PM Jun 19, 2026"   12-hour, quoted, +comma  (O2Ring S)
 *
 * This writes the FIRST and only the first. A writer that can emit two dialects
 * has a bug waiting in the branch nobody exercises, and a reader has to be
 * permissive while a writer only has to be right once.
 *
 * Column layout is taken from real exports, not from the parser's minimum: six
 * columns, because `Motion` carries real data and the two reminder columns are
 * always present. The header text differs per device (`Oxygen Level` vs
 * `SpO2(%)`) and the parser ignores it, reading positionally.
 *
 * See docs/SDD-015.
 */
class O2RingCsvWriter {
public:
    /// The CSV body, CRLF-terminated as the real exports are.
    static std::string write(const cpapdash::parser::OximetrySession& session);

    /// The name the ring's own app would give this file:
    ///   <device>_<YYYYMMDDHHMMSS>.csv   e.g. "O2Ring S_20260619232029.csv"
    /// `device` falls back to "O2Ring" when empty.
    static std::string filenameFor(const cpapdash::parser::OximetrySession& session,
                                   const std::string& device = "");
};

}  // namespace hms_cpap
