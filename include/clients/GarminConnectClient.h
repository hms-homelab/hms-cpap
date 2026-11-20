#pragma once
//
// SDD-078 D1: a native Garmin Connect client, doing what the Python
// garminconnect library does for one purpose: the night's sleep data.
//
// THE API IS UNOFFICIAL. The flow is python-garminconnect's "mobile iOS"
// strategy (garminconnect/client/sso.py, session.py, transport.py,
// constants.py), the one that needs no browser:
//
//   1. GET  {sso}/mobile/sso/en_US/sign-in?clientId=GCM_IOS_DARK&service=...
//      (sets the SSO cookies; a credential POST without them is refused)
//   2. POST {sso}/mobile/api/login?clientId=GCM_IOS_DARK&locale=en-US&service=...
//      {"username","password","rememberMe","captchaToken"}; the answer's
//      responseStatus.type is SUCCESSFUL (serviceTicketId), MFA_REQUIRED
//      (customerMfaInfo.mfaLastMethodUsed), INVALID_USERNAME_PASSWORD, ...
//   3. POST {sso}/mobile/api/mfa/verifyCode (same query) {"mfaMethod",
//      "mfaVerificationCode","rememberMyBrowser","reconsentList","mfaSetup"}
//      when Garmin asked for a code: SUCCESSFUL carries the serviceTicketId
//   4. POST {diauth} (diauth.garmin.com/di-oauth2-service/oauth/token), Basic
//      <client_id>:, grant_type .../oauth/grant/service_ticket, for each DI
//      client id in turn: the access and refresh tokens
//   5. the same endpoint, grant_type refresh_token, refreshes them
//   6. GET {connectapi}/... with "Bearer <access_token>":
//      /userprofile-service/socialProfile (displayName),
//      /wellness-service/wellness/dailySleepData/{displayName}?date=...&nonSleepBufferMinutes=60,
//      /device-service/deviceservice/mylastused (lastUsedDeviceName)
//
// Every answer that does not have the shape these steps expect (a page that
// is not JSON, no responseStatus, no ticket, no token) is GarminAuth::Changed:
// "Garmin changed its sign-in", never a silent nothing (D1).
//
// One instance is one sign-in at a time: the SSO cookies between the login
// and the MFA code live in it. Not thread-safe; its owner serialises calls.
//
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace hms_cpap {

/// Where Garmin is. The defaults are Garmin's; the suite points them at a
/// local fake.
struct GarminEndpoints {
    std::string sso        = "https://sso.garmin.com";
    std::string diauth     = "https://diauth.garmin.com/di-oauth2-service/oauth/token";
    std::string connectapi = "https://connectapi.garmin.com";
    /// The service the mobile sign-in is for; the ticket is exchanged for it.
    std::string service_url = "https://mobile.integration.garmin.com/gcm/ios";
};

/// Garmin's session: what the DI token exchange hands back. Kept as settings
/// (AppConfig::Garmin) so a restart needs no new sign-in.
struct GarminTokens {
    std::string access_token;
    std::string refresh_token;
    std::string client_id;
    bool empty() const { return access_token.empty() && refresh_token.empty(); }
};

enum class GarminAuth {
    Ok,
    MfaRequired,   ///< Garmin sent a code; verifyMfa with it
    BadPassword,   ///< Garmin refused the email or password
    BadMfaCode,    ///< Garmin refused the code; another can be tried
    Blocked,       ///< 403/427/429 or a CAPTCHA: Garmin is refusing this client for now
    Changed,       ///< an answer not in the shape above: Garmin changed its sign-in
    Unreachable,   ///< no answer at all
    Expired,       ///< the refresh token was refused: a new sign-in is needed
    Failed,        ///< anything else, said in the message
};

struct GarminAuthResult {
    GarminAuth  state = GarminAuth::Failed;
    std::string message;
    bool ok() const { return state == GarminAuth::Ok; }
};

/// One connectapi call.
struct GarminApiResult {
    bool           ok = false;
    long           status = 0;
    nlohmann::json body;          ///< null for 204 or an unreadable body
    GarminAuth     auth = GarminAuth::Ok;   ///< why, when the call failed on the session
    std::string    message;
};

/// "Garmin changed its sign-in; update hms-cpap" (D1), the words the card shows.
extern const char* const kGarminChangedMessage;

class GarminConnectClient {
public:
    explicit GarminConnectClient(GarminEndpoints endpoints = {});
    ~GarminConnectClient();
    GarminConnectClient(const GarminConnectClient&) = delete;
    GarminConnectClient& operator=(const GarminConnectClient&) = delete;

    /// Steps 1, 2 and 4. MfaRequired keeps this sign-in's cookies for verifyMfa.
    GarminAuthResult signIn(const std::string& email, const std::string& password);
    /// Step 3 then 4, for a sign-in that returned MfaRequired.
    GarminAuthResult verifyMfa(const std::string& code);
    bool mfaPending() const { return mfa_pending_; }
    /// Forget a half-done sign-in (its cookies and the MFA state).
    void resetSignIn();

    /// Step 5. Expired when Garmin refuses the refresh token.
    GarminAuthResult refresh();

    void setTokens(GarminTokens t) { tokens_ = std::move(t); }
    const GarminTokens& tokens() const { return tokens_; }

    /// Step 6, for a path with its query. The access token is refreshed
    /// first when it ends within 15 minutes (its JWT exp), and once more on a
    /// 401; a refresh Garmin refuses answers with auth Expired.
    GarminApiResult get(const std::string& path_and_query);

    GarminApiResult socialProfile() { return get("/userprofile-service/socialProfile"); }
    /// get_sleep_data(date): [date] is YYYY-MM-DD, Garmin's calendar date
    /// (the morning the sleep ended).
    GarminApiResult sleepData(const std::string& display_name, const std::string& date);
    GarminApiResult lastUsedDevice() { return get("/device-service/deviceservice/mylastused"); }

    // -- Pure helpers, for the suite ----------------------------------------

    /// Whether [jwt]'s exp claim is within 15 minutes of [now] (or past). A
    /// token whose exp cannot be read is taken as good, as garminconnect does.
    static bool expiresSoon(const std::string& jwt, long long now);
    /// "Basic base64(<client_id>:)".
    static std::string basicAuth(const std::string& client_id);
    /// The DI client ids tried in turn for the ticket exchange.
    static const std::vector<std::string>& diClientIds();

    /// Test seam: what "now" is for expiresSoon (0: the system clock).
    void setNowForTest(long long now) { now_for_test_ = now; }

private:
    struct Http;   // libcurl, one handle per sign-in (its cookie jar)

    GarminAuthResult afterCredentials(long status, const std::string& body);
    GarminAuthResult exchangeTicket(const std::string& ticket);
    std::string loginQuery() const;
    std::vector<std::string> ssoPostHeaders() const;
    long long now() const;

    GarminEndpoints endpoints_;
    GarminTokens    tokens_;
    Http*           sso_ = nullptr;
    bool            mfa_pending_ = false;
    std::string     mfa_method_;
    long long       now_for_test_ = 0;
};

const char* garminAuthName(GarminAuth a);

}  // namespace hms_cpap
