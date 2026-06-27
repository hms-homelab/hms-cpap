#pragma once

#include <string>
#include <cpapdash/parser/VLDParser.h>

namespace hms_cpap {

// Parses a Wellue / Viatom "O2 Ring" CSV export (the file the phone app
// produces) into an OximetrySession, the same struct the BLE/VLD path yields,
// so it can be stored via DatabaseService::saveOximetrySession unchanged.
//
// Handles both export dialects seen in the wild:
//   "06:53:07 Apr 12 2026"      (24-hour, unquoted)
//   "11:20:29PM Jun 19, 2026"   (12-hour AM/PM, quoted, comma after the day)
//
// The sample interval is auto-detected from the timestamps (never assumed) so
// per-second exports don't get their duration inflated. Sentinel "no reading"
// values (SpO2 255 / HR 65535) are mapped to 0xFF so OximetrySample::valid()
// excludes them. Metrics are filled via VLDParser::calculateMetrics.
class O2RingCsvParser {
public:
    static cpapdash::parser::OximetrySession parse(const std::string& content,
                                                   const std::string& filename);
};

}  // namespace hms_cpap
