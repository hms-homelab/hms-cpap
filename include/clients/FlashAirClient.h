#pragma once

#include <string>
#include <vector>
#include <curl/curl.h>
#include "clients/EzShareClient.h"   // EzShareFileEntry, ListingRefused
#include "clients/IDataSource.h"
#include "utils/FailureLogThrottle.h"

namespace hms_cpap {

/**
 * FlashAirClient - HTTP client for a Toshiba FlashAir WiFi SD card (SDD-056)
 *
 * FlashAir API (verified against a W-02, firmware W2.00.02):
 *   Directory listing:  GET /command.cgi?op=100&DIR=/DATALOG/YYYYMMDD
 *                       the card root is DIR=/ (DIR= empty is a 400)
 *   File download:      GET /DATALOG/YYYYMMDD/filename.edf, Range honoured
 *
 * A missing folder answers 404 and a missing file 307 to http://flashair/,
 * both with an empty body; neither is followed. The card serves one request
 * at a time and queues the rest. Read only: nothing is ever written to it.
 */
class FlashAirClient : public IDataSource {
public:
    FlashAirClient();
    ~FlashAirClient();

    FlashAirClient(const FlashAirClient&) = delete;
    FlashAirClient& operator=(const FlashAirClient&) = delete;

    std::string sourceName() const override { return "the FlashAir"; }

    std::vector<std::string> listDateFolders() override;
    std::vector<EzShareFileEntry> listFiles(const std::string& date_folder) override;

    /** card_path is backslash-separated and card-relative, as for an ez Share;
     *  "" lists the card root. Files AND directories. */
    std::vector<EzShareFileEntry> listDir(const std::string& card_path) override;
    bool downloadByPath(const std::string& card_rel_path,
                        const std::string& local_path) override;

    bool downloadFile(const std::string& date_folder,
                      const std::string& filename,
                      const std::string& local_path) override;
    bool downloadFileRange(const std::string& date_folder,
                           const std::string& filename,
                           const std::string& local_path,
                           size_t start_byte,
                           size_t& bytes_downloaded) override;
    bool downloadRootFile(const std::string& filename, const std::string& local_path) override;

    bool supportsRange() const override { return supports_range_; }

    std::string getBaseURL() const { return base_url_; }
    void setBaseURL(const std::string& url) { base_url_ = url; }

    /** Parse an op=100 listing: a WLANSD_FILELIST header, then one
     *  <dir>,<name>,<bytes>,<attr>,<fdate>,<ftime> line per entry. */
    static std::vector<EzShareFileEntry> parseFileList(const std::string& body);

    /** A backslash card path as the FlashAir's absolute URL path:
     *  "SETTINGS\\AGL.tgt" -> "/SETTINGS/AGL.tgt", percent-encoded. */
    static std::string encodeCardPath(const std::string& path);

private:
    CURL* curl_;
    FailureLogThrottle http_fail_log_;
    std::string base_url_;
    bool supports_range_ = true;

    static constexpr long DOWNLOAD_TIMEOUT  = 300L;
    static constexpr long LOW_SPEED_LIMIT = 1024L;
    static constexpr long LOW_SPEED_TIME  = 30L;
    static constexpr long CONNECTION_TIMEOUT = 10L;

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userp);

    /** The listing of one card folder ("/" for the root). */
    std::vector<EzShareFileEntry> list(const std::string& dir);

    /** GET [url_path] into [local_path] from [start_byte] (0 = the whole file,
     *  truncating). On any failure the file is left exactly as it was. */
    bool fetch(const std::string& url_path, const std::string& local_path,
               size_t start_byte, size_t& bytes_downloaded);

    /** Every request: IPv4 only (a .local name otherwise waits 5 s on an
     *  AAAA answer the card never gives), no redirects. */
    void commonOptions(const std::string& url, long timeout);
};

} // namespace hms_cpap
