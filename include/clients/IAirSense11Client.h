#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * The bridge's AirSense 11 Bluetooth endpoint, as hms-cpap uses it.
 *
 * Some AirSense 11 machines cannot power a WiFi SD card in their slot. The
 * bridge pairs with such a machine over the machine's own Bluetooth (once,
 * with the code on its screen, on the bridge's own page) and answers the
 * machine's daily summary: one record per therapy day, the figures the
 * machine writes to STR.edf. The bytes are the machine's "Summary" spool,
 * which cpapdash::parser::AirSense11SummaryParser reads.
 */
class IAirSense11Client {
public:
    virtual ~IAirSense11Client() = default;

    /**
     * The summary from `from` ("YYYY-MM-DD") onwards, as the machine sent it.
     * Empty when the bridge could not reach the machine (not paired, asleep,
     * out of range) or the bridge itself did not answer.
     */
    virtual std::vector<uint8_t> fetchSummary(const std::string& from) = 0;
};

} // namespace hms_cpap
