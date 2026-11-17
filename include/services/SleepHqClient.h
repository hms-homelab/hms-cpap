#pragma once
//
// SleepHqClient — client for SleepHQ's public API (https://sleephq.com/api/v1).
// Mirrors the cloud (hms-cpapdash-api) client. OAuth grant_type=password with
// the user's API client credentials (confirmed: client_credentials yields an
// anonymous token where /me fails).
//
#include <nlohmann/json.hpp>

#include <string>

namespace hms_cpap {

class SleepHqClient {
public:
    SleepHqClient(std::string client_id, std::string client_secret);

    // Fetch access token + current team id. false on failure (err set).
    bool connect(std::string& err);

    // Create a programmatic import; returns the import id ("" on failure).
    // `device_id` is SleepHQ's device TYPE (GET /v1/devices); 0 sends none and
    // SleepHQ reads the files as a CPAP card. A ring CSV in a CPAP import
    // fails: "We couldn't find any CPAP machine data in this upload".
    std::string createImport(std::string& err, int device_id = 0);

    // The form createImport posts. Split out so it is testable without a network.
    static std::string importForm(int device_id);

    // SleepHQ's "O2 Ring" (Viatom, Pulse Oximetry Ring) in GET /v1/devices.
    static constexpr int kO2RingDeviceId = 69184;

    // What SleepHQ made of an import: "complete", "failed", or still working
    // ("pending", "processing", ...). False when it could not be read.
    // `failed_reason` is SleepHQ's own words for a failed one.
    bool importStatus(const std::string& import_id, std::string& status,
                      std::string& failed_reason, std::string& err);

    // The pure half: GET /imports/{id}'s answer to its status and reason.
    static bool parseImportStatus(const nlohmann::json& body, std::string& status,
                                  std::string& failed_reason);

    // The serial of the team's own O2 Ring, for the CSV's name. SleepHQ reads
    // the digits after "O2Ring " in a ring CSV's filename as the ring's serial
    // and files onto the machine with that serial. A team whose ring machine
    // has one REFUSES a CSV that names none ("We could not find <file> in your
    // upload"), and a wrong one makes a second ring machine. "" when the team
    // has no such machine, or the list could not be read.
    std::string o2RingSerial(std::string& err);

    // The pure half: GET /teams/{team}/machines' answer to the serial of the
    // most recently updated Viatom O2 Ring whose serial is all digits.
    static std::string pickO2RingSerial(const nlohmann::json& machines);

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

    // Tests only: talk to a local server with a token already in hand.
    void setBaseUrl(const std::string& base) { base_url_ = base; }
    void setToken(const std::string& token, const std::string& team_id) {
        access_token_ = token;
        team_id_ = team_id;
    }

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
