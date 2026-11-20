#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace hms_cpap {

class IO2RingClient {
public:
    virtual ~IO2RingClient() = default;

    struct LiveReading {
        int spo2 = 0;
        int hr = 0;
        int motion = 0;
        int vibration = 0;
        /// The ring's own run status byte (an O2Ring-S live reply; 3 is ended).
        /// -1 where the client does not report one.
        int run = -1;
        bool active = false;
        bool valid = false;
    };

    virtual bool isConnected() = 0;
    virtual std::vector<std::string> listFiles() = 0;
    virtual std::vector<uint8_t> downloadFile(const std::string& filename) = 0;
    virtual LiveReading getLive() = 0;
    virtual int getBattery() const = 0;
};

} // namespace hms_cpap
