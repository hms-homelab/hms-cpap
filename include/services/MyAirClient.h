#pragma once
//
// MyAirClient — reads a patient's own nights back out of ResMed's myAir.
//
// SDD-020. ResMed already scores every night on their own servers, from data
// their own machine uploaded over its own modem, and shows the patient a number.
// This fetches that number, and the four sub-scores behind it, so it can sit
// next to ours per night and per component instead of neither side seeing the
// other.
//
// THE API IS UNDOCUMENTED. The endpoints, client ids and api keys below were
// reverse engineered from the myAir web app and are the same ones the
// long-running Home Assistant integration uses. ResMed can change any of it
// without notice, so every caller must treat failure as ordinary: this is a
// second opinion on the card, never a source the rest of the system depends on.
//
// Read only, permanently. Nothing here writes to ResMed.
//
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {

/// Per-region endpoints and client identifiers. Two exist; there is no third.
struct MyAirRegion {
    std::string product;        // the rmdproduct header value
    std::string okta_host;      // e.g. resmed-ext-1.okta.com
    std::string auth_server_id; // path segment in the oauth2 URLs
    std::string client_id;      // appears in authorize and token bodies
    std::string api_key;        // the x-api-key header for AppSync
    std::string graphql_url;
    std::string redirect_url;   // must match what Okta has registered
    std::string email_factor_id;  // empty where the region has no email MFA

    std::string authnUrl() const { return "https://" + okta_host + "/api/v1/authn"; }
    std::string authorizeUrl() const {
        return "https://" + okta_host + "/oauth2/" + auth_server_id + "/v1/authorize";
    }
    std::string tokenUrl() const {
        return "https://" + okta_host + "/oauth2/" + auth_server_id + "/v1/token";
    }
    std::string mfaUrl(const std::string& factor_id) const {
        return "https://" + okta_host + "/api/v1/authn/factors/" + factor_id +
               "/verify?rememberDevice=true";
    }
};

const MyAirRegion& myAirRegionNA();
const MyAirRegion& myAirRegionEU();
/// "NA" or "EU", case-insensitive. Falls back to NA on anything else.
const MyAirRegion& myAirRegion(const std::string& code);

/// One night as ResMed scored it.
///
/// The sub-scores are the reason this is worth having: we do not have to guess
/// how myAir weights usage against mask seal against events, because it hands
/// all four over next to the total.
struct MyAirSleepRecord {
    std::string start_date;      // YYYY-MM-DD, ResMed's opinion of which day
    double total_usage_min = 0;  // minutes
    int sleep_score = 0;
    int usage_score = 0;
    int ahi_score = 0;
    int mask_score = 0;
    int leak_score = 0;
    double ahi = 0;
    int mask_pair_count = 0;
    double leak_percentile = 0;
};

/// The machine as ResMed knows it. Identity, not measurement: useful to prefill
/// an equipment profile, never to overwrite something the user typed.
struct MyAirDevice {
    std::string serial_number;
    std::string localized_name;
    std::string device_series;
    std::string device_family;
    std::string last_sleep_data_report_time;
    std::string manufacturer_name;
    std::string mask_code;
};

/// What a login attempt needs next.
enum class MyAirAuthState {
    Ok,             // signed in, tokens held
    MfaRequired,    // a code has been emailed; call verifyMfa with it
    Failed,
};

class MyAirClient {
public:
    MyAirClient(std::string username, std::string password, std::string region_code);

    /// Sign in. Returns MfaRequired when ResMed emails a code, in which case the
    /// caller must collect it from the user and call verifyMfa. The state token
    /// that ties the two halves together is held internally.
    MyAirAuthState connect(std::string& err);

    /// Finish a login that returned MfaRequired.
    MyAirAuthState verifyMfa(const std::string& code, std::string& err);

    /// The remembered-device cookie. Persist it and hand it back through
    /// setDeviceToken on the next start, or ResMed emails a code every time.
    const std::string& deviceToken() const { return device_token_; }
    void setDeviceToken(std::string token) { device_token_ = std::move(token); }

    bool isAuthenticated() const { return !access_token_.empty(); }

    /// Recent nights. There is no history endpoint; whatever we want to keep, we
    /// keep ourselves.
    ///
    /// THE WINDOW IS WHOLE MONTHS, NOT 30 DAYS. The parameters are named
    /// startMonth and endMonth and they mean it: asking for the last 30 days
    /// from 26 August returned 56 records starting at 1 July, because ResMed
    /// serves from the beginning of the month the start date falls in. Measured
    /// against a real account, not assumed.
    ///
    /// A returned night can be entirely zero. That means ResMed HAS NO DATA for
    /// that date, which is not the same as a night with no therapy, and a caller
    /// must not average the two together. On a machine whose modem reports
    /// intermittently most dates come back this way.
    bool fetchSleepRecords(std::vector<MyAirSleepRecord>& out, std::string& err);
    bool fetchDevice(MyAirDevice& out, std::string& err);

    // -- Pure helpers, exposed so they can be tested without a network ------

    /// RFC 7636 S256: base64url(SHA-256(verifier)), unpadded.
    static std::string pkceChallenge(const std::string& verifier);
    /// A fresh high-entropy verifier, alphanumerics only.
    static std::string makeCodeVerifier();
    /// base64url encode without padding.
    static std::string base64Url(const unsigned char* data, size_t len);
    /// Pull the authorization code out of the FRAGMENT of an Okta redirect. The
    /// code arrives after a '#', not a '?', which is the one genuinely
    /// surprising step in the flow.
    static std::string codeFromRedirect(const std::string& location);
    /// Read a claim out of a JWT payload WITHOUT verifying the signature. We are
    /// not the audience of this token; we are reading a value ResMed put in it
    /// for their own web app, which is where the country header comes from.
    static std::string unverifiedJwtClaim(const std::string& jwt, const std::string& claim);

    /// Parse the GraphQL payloads. Separated from the fetch so that a change in
    /// ResMed's response shape is a test failure rather than a field of zeroes.
    static bool parseSleepRecords(const std::string& body,
                                  std::vector<MyAirSleepRecord>& out, std::string& err);
    static bool parseDevice(const std::string& body, MyAirDevice& out, std::string& err);

private:
    std::string username_, password_;
    const MyAirRegion* region_;

    std::string state_token_;    // between authn and MFA verify
    std::string session_token_;  // between authn/MFA and the code exchange
    std::string access_token_;
    std::string id_token_;
    std::string device_token_;   // the DT cookie, so MFA is asked once
    std::string session_cookie_; // the sid cookie
    std::string mfa_factor_id_;
    std::string country_code_;

    bool exchangeSessionForTokens(std::string& err);
    bool graphql(const std::string& operation_name, const std::string& query,
                 std::string& body, std::string& err);
    std::string countryCode();
};

}  // namespace hms_cpap
