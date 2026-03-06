#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <curl/curl.h>

namespace hms_cpap {

/**
 * File entry from ez Share directory listing
 *
 * Timestamps come from the SD card (file modification time).
 * Sizes can be compared between bursts to detect in-progress writes.
 */
struct EzShareFileEntry {
    std::string name;       // Display name (e.g., "20260204_001809_CSL.edf")
    int size_kb = 0;        // Size in KB from listing
    bool is_dir = false;    // true for <DIR> entries
    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0, second = 0;

    /** Get file modification time as time_point (from directory listing) */
    std::chrono::system_clock::time_point getModTime() const;
};

/**
 * EzShareClient - HTTP client for ez Share WiFi SD card
 *
 * ez Share API (verified against real hardware):
 *   Directory listing:  GET /dir?dir=A:DATALOG%5CYYYYMMDD
 *   File download:      GET /download?file=DATALOG%5CYYYYMMDD%5Cfilename.edf
 *
 * Note: Card's own href links have a firmware bug (drops first char of path).
 * We construct download URLs ourselves using the correct DATALOG prefix.
 * Backslash (%5C) is required; forward slashes return 404.
 */
class EzShareClient {
public:
    EzShareClient();
    ~EzShareClient();

    EzShareClient(const EzShareClient&) = delete;
    EzShareClient& operator=(const EzShareClient&) = delete;

    /**
     * List available date folders in DATALOG (e.g., ["20260203", "20260204"])
     */
    std::vector<std::string> listDateFolders();

    /**
     * List files in a date folder
     * @param date_folder e.g., "20260203"
     * @return File entries with name and size
     */
    std::vector<EzShareFileEntry> listFiles(const std::string& date_folder);

    /**
     * Download a single file from a date folder
     * @param date_folder e.g., "20260203"
     * @param filename Display name from listFiles (e.g., "20260204_001809_CSL.edf")
     * @param local_path Local path to save
     */
    bool downloadFile(const std::string& date_folder,
                      const std::string& filename,
                      const std::string& local_path);

    /**
     * Download file using HTTP Range (incremental/resume)
     * @param date_folder e.g., "20260203"
     * @param filename Display name
     * @param local_path Local path to append data
     * @param start_byte Byte offset to start from (0 = full download)
     * @param bytes_downloaded Output: total bytes downloaded in this call
     * @return true if successful
     */
    bool downloadFileRange(const std::string& date_folder,
                          const std::string& filename,
                          const std::string& local_path,
                          size_t start_byte,
                          size_t& bytes_downloaded);

    /**
     * Download complete CPAP session (BRP, EVE, SAD, PLD, CSL EDF files)
     * @param date_folder e.g., "20260203"
     * @param local_dir Local directory to save files
     * @return true if at least EVE + one data file downloaded
     */
    bool downloadSession(const std::string& date_folder,
                         const std::string& local_dir);

    /**
     * Download a file from the SD card root (not inside a date folder)
     * Used for STR.EDF which lives at DATALOG root level.
     *
     * @param filename e.g., "STR.EDF"
     * @param local_path Local path to save
     */
    bool downloadRootFile(const std::string& filename, const std::string& local_path);

    std::string getBaseURL() const { return base_url_; }
    void setBaseURL(const std::string& url) { base_url_ = url; }

private:
    CURL* curl_;
    std::string base_url_;

    static constexpr long DOWNLOAD_TIMEOUT  = 60L;   // large BRP files ~2 MB
    static constexpr long CONNECTION_TIMEOUT = 10L;

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userp);

    /** Fetch HTML from a URL */
    std::string httpGet(const std::string& url);

    /** Parse ez Share HTML directory listing */
    std::vector<EzShareFileEntry> parseDirectoryListing(const std::string& html);
};

} // namespace hms_cpap
