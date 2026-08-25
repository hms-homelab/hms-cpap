#pragma once

#include "clients/IO2RingClient.h"
#include <string>
#include <vector>
#include <cstdint>
#include <curl/curl.h>
#include "utils/FailureLogThrottle.h"

namespace hms_cpap {

/**
 * O2RingClient - HTTP client for O2 Ring mule ESP32-C3
 *
 * Mule API:
 *   Status:   GET /o2ring/status  -> JSON {"connected":true,"model":"1652",...}
 *   List:     GET /o2ring/files   -> JSON {"files":["20260412065307.vld",...],"battery":74}
 *   Download: GET /o2ring/files?name=X -> raw binary .vld (application/octet-stream)
 */
class O2RingClient : public IO2RingClient {
public:
    explicit O2RingClient(const std::string& base_url);
    ~O2RingClient();

    O2RingClient(const O2RingClient&) = delete;
    O2RingClient& operator=(const O2RingClient&) = delete;

    /**
     * Check if O2 Ring is connected to the mule.
     * @return true if mule reports connected=true
     */
    bool isConnected() override;

    /**
     * List available .vld files on the O2 Ring.
     * @return filenames (e.g. "20260412065307.vld")
     */
    std::vector<std::string> listFiles() override;

    /**
     * Download a .vld file as raw bytes.
     * @param filename e.g. "20260412065307.vld"
     * @return file contents, empty on failure
     */
    std::vector<uint8_t> downloadFile(const std::string& filename) override;

    LiveReading getLive() override;

    /**
     * Get battery level from last status/list call.
     * @return 0-100 percent, or -1 if unknown
     */
    int getBattery() const override { return cached_battery_; }

    std::string getBaseURL() const { return base_url_; }

private:
    CURL* curl_;
    // Collapses repeated identical HTTP failures (offline device) into one
    // line plus periodic summaries, instead of one error per poll cycle.
    FailureLogThrottle http_fail_log_;
    std::string base_url_;
    int cached_battery_ = -1;

    // A full night does not fit in 60s. Measured on the bench 2026-08-25 against
    // a real O2Ring through the mule: ~620 B/s of streaming once connected, plus
    // ~15s of connect + service discovery + FILE_OPEN before the first byte. A
    // 3885-byte file took 22s end to end; Lee's real overnight .vld is 37,995
    // bytes, which lands around 75-80s and would have been abandoned mid-transfer.
    static constexpr long DOWNLOAD_TIMEOUT  = 300L;
    static constexpr long CONNECTION_TIMEOUT = 5L;

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t WriteBinaryCallback(void* contents, size_t size, size_t nmemb, void* userp);

    /** Fetch string response from URL */
    std::string httpGet(const std::string& url);
};

} // namespace hms_cpap
