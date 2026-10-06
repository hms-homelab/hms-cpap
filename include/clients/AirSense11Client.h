#pragma once

#include "clients/IAirSense11Client.h"
#include "utils/FailureLogThrottle.h"

#include <curl/curl.h>
#include <string>
#include <vector>
#include <cstdint>

namespace hms_cpap {

/**
 * AirSense11Client - HTTP client for the bridge's AirSense 11 endpoint.
 *
 * Bridge API:
 *   Summary: GET /airsense11/summary?from=YYYY-MM-DD
 *            -> the machine's Summary spool, application/octet-stream;
 *               408 when the machine did not answer, 409 when not paired.
 */
class AirSense11Client : public IAirSense11Client {
public:
    explicit AirSense11Client(const std::string& base_url);
    ~AirSense11Client() override;

    AirSense11Client(const AirSense11Client&) = delete;
    AirSense11Client& operator=(const AirSense11Client&) = delete;

    std::vector<uint8_t> fetchSummary(const std::string& from) override;

    std::string getBaseURL() const { return base_url_; }

private:
    CURL* curl_;
    FailureLogThrottle http_fail_log_;
    std::string base_url_;

    // A pull is a Bluetooth connect, a session and a spool of up to 64 KB at
    // a few hundred bytes a second: minutes, not seconds, when the machine is
    // slow to wake.
    static constexpr long DOWNLOAD_TIMEOUT   = 300L;
    static constexpr long CONNECTION_TIMEOUT = 5L;

    static size_t WriteBinaryCallback(void* contents, size_t size, size_t nmemb, void* userp);
};

} // namespace hms_cpap
