#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <curl/curl.h>

namespace hms_cpap {

/**
 * O2RingClient - HTTP client for O2 Ring mule ESP32-C3
 *
 * Mule API:
 *   Status:   GET /o2ring/status  -> JSON {"connected":true,"model":"1652",...}
 *   List:     GET /o2ring/files   -> JSON {"files":["20260412065307.vld",...],"battery":74}
 *   Download: GET /o2ring/files?name=X -> raw binary .vld (application/octet-stream)
 */
class O2RingClient {
public:
    explicit O2RingClient(const std::string& base_url);
    ~O2RingClient();

    O2RingClient(const O2RingClient&) = delete;
    O2RingClient& operator=(const O2RingClient&) = delete;

    /**
     * Check if O2 Ring is connected to the mule.
     * @return true if mule reports connected=true
     */
    bool isConnected();

    /**
     * List available .vld files on the O2 Ring.
     * @return filenames (e.g. "20260412065307.vld")
     */
    std::vector<std::string> listFiles();

    /**
     * Download a .vld file as raw bytes.
     * @param filename e.g. "20260412065307.vld"
     * @return file contents, empty on failure
     */
    std::vector<uint8_t> downloadFile(const std::string& filename);

    struct LiveReading {
        int spo2 = 0;       // 0-100
        int hr = 0;         // bpm
        int motion = 0;
        int vibration = 0;
        bool active = false;
        bool valid = false;  // active && spo2 > 0 && hr > 0
    };

    /**
     * Poll live SpO2/HR from ring via mule.
     * GET /o2ring/live — mule connects BLE on demand.
     */
    LiveReading getLive();

    /**
     * Get battery level from last status/list call.
     * @return 0-100 percent, or -1 if unknown
     */
    int getBattery() const { return cached_battery_; }

    std::string getBaseURL() const { return base_url_; }

private:
    CURL* curl_;
    std::string base_url_;
    int cached_battery_ = -1;

    static constexpr long DOWNLOAD_TIMEOUT  = 60L;
    static constexpr long CONNECTION_TIMEOUT = 5L;

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t WriteBinaryCallback(void* contents, size_t size, size_t nmemb, void* userp);

    /** Fetch string response from URL */
    std::string httpGet(const std::string& url);
};

} // namespace hms_cpap
