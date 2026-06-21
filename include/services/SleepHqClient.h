#pragma once
//
// SleepHqClient — client for SleepHQ's public API (https://sleephq.com/api/v1).
// Mirrors the cloud (hms-cpapdash-api) client. OAuth grant_type=password with
// the user's API client credentials (confirmed: client_credentials yields an
// anonymous token where /me fails).
//
#include <string>

namespace hms_cpap {

class SleepHqClient {
public:
    SleepHqClient(std::string client_id, std::string client_secret);

    // Fetch access token + current team id. false on failure (err set).
    bool connect(std::string& err);
    const std::string& teamId() const { return team_id_; }

    // Create a programmatic import; returns the import id ("" on failure).
    std::string createImport(std::string& err);

    // Upload one file. rel_path is the path within the SD tree (e.g. "DATALOG/20251215"
    // or "" for root files like STR.edf / Identification.*).
    bool uploadFile(const std::string& import_id, const std::string& name,
                    const std::string& rel_path, const std::string& abs_file_path,
                    std::string& err);

    // Finalize the import (process uploaded files).
    bool processFiles(const std::string& import_id, std::string& err);

    // content_hash = MD5(filename bytes + file bytes), hex.
    static std::string contentHash(const std::string& filename,
                                   const std::string& abs_file_path);

    void setBaseUrl(const std::string& base) { base_url_ = base; }

private:
    std::string client_id_, client_secret_;
    std::string base_url_{"https://sleephq.com/api/v1"};
    std::string token_url_{"https://sleephq.com/oauth/token"};
    std::string access_token_;
    std::string team_id_;

    std::string httpGet(const std::string& url, long& status) const;
    std::string httpPostForm(const std::string& url, const std::string& form, long& status) const;
};

} // namespace hms_cpap
