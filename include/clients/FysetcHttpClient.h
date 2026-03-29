#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <curl/curl.h>

namespace hms_cpap {

/**
 * Response from GET /poll on the Fysetc poll server.
 *
 * status values:
 *   "ok"        - diff available (new_dirs, new_files, deltas populated)
 *   "pending"   - scan not finished yet, wait next_poll_sec
 *   "no_change" - nothing changed since last /ack
 */
struct FysetcPollResponse {
    std::string status;
    int cycle = 0;
    int64_t polled_at = 0;
    int next_poll_sec = 65;

    struct DirEntry  { std::string path; std::string name; };
    struct FileEntry { std::string path; std::string name; int64_t size = 0; };
    struct DeltaEntry { std::string path; std::string name; int64_t from = 0; int64_t to = 0; };

    std::vector<DirEntry> new_dirs;
    std::vector<FileEntry> new_files;
    std::vector<DeltaEntry> deltas;
};

/**
 * Response from POST /ack on the Fysetc poll server.
 */
struct FysetcAckResponse {
    std::string status;
    int n_acked = 0;
    int next_poll_sec = 65;
};

/**
 * FysetcHttpClient - HTTP client for the Fysetc SD WiFi Pro poll server
 *
 * Protocol:
 *   POST /init       - negotiate poll interval, seed known files
 *   GET  /poll       - get diff (new files, deltas) since last /ack
 *   GET  /file       - stream raw EDF bytes from offset
 *   POST /ack        - confirm received byte offsets
 *   GET  /api/status - health check
 *
 * The Fysetc device announces itself via POST to cpap.local:8893/fysetc/announce,
 * providing its IP. This client uses that IP as its base URL.
 */
class FysetcHttpClient {
public:
    FysetcHttpClient();
    ~FysetcHttpClient();

    FysetcHttpClient(const FysetcHttpClient&) = delete;
    FysetcHttpClient& operator=(const FysetcHttpClient&) = delete;

    void setBaseURL(const std::string& url);
    std::string getBaseURL() const { return base_url_; }

    /**
     * POST /init - negotiate poll interval and seed known files.
     *
     * @param poll_interval_sec Desired poll interval
     * @param known_files Files hms-cpap already has (path, confirmed_offset)
     * @param out_poll_interval Negotiated poll interval from device
     * @return true on success
     */
    bool init(int poll_interval_sec,
              const std::vector<std::pair<std::string, int64_t>>& known_files,
              int& out_poll_interval);

    /**
     * GET /poll - get diff since last /ack.
     *
     * @return Poll response, or nullopt on HTTP/parse error
     */
    std::optional<FysetcPollResponse> poll();

    /**
     * GET /file?path=...&offset=N - stream raw EDF bytes.
     *
     * @param path Relative path (e.g., "DATALOG/20260328/BRP.EDF")
     * @param offset Byte offset to start from
     * @param out_status HTTP status code (200=ok, 500=SD busy, 404=not found, -1=error)
     * @return Raw bytes, empty on failure
     */
    std::vector<uint8_t> fetchFile(const std::string& path, int64_t offset, int& out_status);

    /**
     * POST /ack - confirm received byte offsets.
     *
     * @param confirmed_offsets Vector of (path, confirmed_offset)
     * @return Ack response, or nullopt on error
     */
    std::optional<FysetcAckResponse> ack(
        const std::vector<std::pair<std::string, int64_t>>& confirmed_offsets);

    /**
     * GET /api/status - health check.
     *
     * @return true if device responds with valid JSON
     */
    bool healthCheck();

private:
    CURL* curl_;
    std::string base_url_;

    static constexpr long DOWNLOAD_TIMEOUT  = 60L;
    static constexpr long CONNECTION_TIMEOUT = 10L;

    static size_t WriteStringCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t WriteBinaryCallback(void* contents, size_t size, size_t nmemb, void* userp);

    /** Perform HTTP GET, return response body (empty on error) */
    std::string httpGet(const std::string& url, long timeout = CONNECTION_TIMEOUT);

    /** Perform HTTP GET, return raw bytes + HTTP status */
    std::vector<uint8_t> httpGetBinary(const std::string& url, int& out_status);

    /** Perform HTTP POST with JSON body, return response body */
    std::string httpPostJson(const std::string& url, const std::string& json_body);
};

} // namespace hms_cpap
