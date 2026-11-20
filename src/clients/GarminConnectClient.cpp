#include "clients/GarminConnectClient.h"

#include "services/MyAirClient.h"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <cstdlib>
#include <ctime>

namespace hms_cpap {

using json = nlohmann::json;

const char* const kGarminChangedMessage = "Garmin changed its sign-in; update hms-cpap";

namespace {

// python-garminconnect, garminconnect/client/constants.py.
constexpr const char* kIosClientId = "GCM_IOS_DARK";
constexpr const char* kIosLoginUa =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 18_7 like Mac OS X) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148";
constexpr const char* kNativeUa = "GCM-Android-5.23";
constexpr const char* kNativeXUa =
    "com.garmin.android.apps.connectmobile/5.23; ; Google/sdk_gphone64_arm64/google; "
    "Android/33; Dalvik/2.1.0";
constexpr const char* kDiGrantType =
    "https://connectapi.garmin.com/di-oauth2-service/oauth/grant/service_ticket";

struct Reply {
    long        status = 0;   ///< 0: no answer
    std::string body;
    std::string error;        ///< curl's, when status is 0
};

size_t writeCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

/// One request on [c] (a fresh handle, or the sign-in's with its cookies).
Reply perform(CURL* c, const std::string& url, const std::vector<std::string>& headers,
              const std::string* body) {
    Reply r;
    struct curl_slist* h = nullptr;
    for (const auto& line : headers) h = curl_slist_append(h, line.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    } else {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    else r.error = curl_easy_strerror(rc);
    curl_slist_free_all(h);
    return r;
}

Reply oneShot(const std::string& url, const std::vector<std::string>& headers,
              const std::string* body) {
    CURL* c = curl_easy_init();
    if (!c) return Reply{0, "", "curl could not start"};
    Reply r = perform(c, url, headers, body);
    curl_easy_cleanup(c);
    return r;
}

std::string urlEncode(const std::string& s) {
    CURL* c = curl_easy_init();
    if (!c) return s;
    char* out = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string r = out ? out : s;
    if (out) curl_free(out);
    curl_easy_cleanup(c);
    return r;
}

std::string form(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (const auto& [k, v] : kv) {
        if (!out.empty()) out += '&';
        out += urlEncode(k) + "=" + urlEncode(v);
    }
    return out;
}

/// The native app's headers, which connectapi and diauth expect
/// (transport.py _native_headers).
std::vector<std::string> nativeHeaders() {
    return {std::string("User-Agent: ") + kNativeUa,
            std::string("X-Garmin-User-Agent: ") + kNativeXUa,
            "X-Garmin-Paired-App-Version: 10861",
            "X-Garmin-Client-Platform: Android",
            "X-App-Ver: 10861",
            "X-Lang: en",
            "X-GCExperience: GC5",
            "Accept-Language: en-US,en;q=0.9"};
}

bool blockedStatus(long s) { return s == 403 || s == 427 || s == 429; }

/// [key] of [j] when it is a string, else "": Garmin's answers are not
/// trusted to keep their types.
std::string str(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return "";
    return j[key].get<std::string>();
}

GarminAuthResult result(GarminAuth s, std::string m) { return GarminAuthResult{s, std::move(m)}; }

GarminAuthResult unreachable(const Reply& r) {
    return result(GarminAuth::Unreachable, "Garmin did not answer (" + r.error + ")");
}

GarminAuthResult blocked(long status) {
    return result(GarminAuth::Blocked,
                  "Garmin is refusing this sign-in for now (HTTP " + std::to_string(status) +
                      "); it is tried again on the next event");
}

}  // namespace

struct GarminConnectClient::Http {
    CURL* c = nullptr;
    Http() {
        c = curl_easy_init();
        // The cookie engine, in memory: the sign-in page's cookies go back
        // with the credentials and the MFA code.
        if (c) curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
    }
    ~Http() { if (c) curl_easy_cleanup(c); }
};

GarminConnectClient::GarminConnectClient(GarminEndpoints endpoints)
    : endpoints_(std::move(endpoints)) {}

GarminConnectClient::~GarminConnectClient() { delete sso_; }

void GarminConnectClient::resetSignIn() {
    delete sso_;
    sso_ = nullptr;
    mfa_pending_ = false;
    mfa_method_.clear();
}

long long GarminConnectClient::now() const {
    return now_for_test_ ? now_for_test_ : static_cast<long long>(std::time(nullptr));
}

const std::vector<std::string>& GarminConnectClient::diClientIds() {
    static const std::vector<std::string> ids{
        "GARMIN_CONNECT_MOBILE_ANDROID_DI_2025Q2", "GARMIN_CONNECT_MOBILE_ANDROID_DI_2024Q4",
        "GARMIN_CONNECT_MOBILE_ANDROID_DI", "GARMIN_CONNECT_MOBILE_IOS_DI"};
    return ids;
}

std::string GarminConnectClient::basicAuth(const std::string& client_id) {
    const std::string raw = client_id + ":";
    std::string out(4 * ((raw.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                                  reinterpret_cast<const unsigned char*>(raw.data()),
                                  static_cast<int>(raw.size()));
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return "Basic " + out;
}

bool GarminConnectClient::expiresSoon(const std::string& jwt, long long now) {
    const std::string exp = MyAirClient::unverifiedJwtClaim(jwt, "exp");
    if (exp.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(exp.c_str(), &end);
    if (end == exp.c_str()) return false;
    return static_cast<double>(now) > v - 900.0;
}

std::string GarminConnectClient::loginQuery() const {
    return form({{"clientId", kIosClientId}, {"locale", "en-US"}, {"service", endpoints_.service_url}});
}

std::vector<std::string> GarminConnectClient::ssoPostHeaders() const {
    const std::string signin = endpoints_.sso + "/mobile/sso/en_US/sign-in?" +
                               form({{"clientId", kIosClientId}, {"service", endpoints_.service_url}});
    return {std::string("User-Agent: ") + kIosLoginUa, "Accept: application/json, text/plain, */*",
            "Accept-Language: en-US,en;q=0.9", "Content-Type: application/json",
            "Origin: " + endpoints_.sso, "Referer: " + signin};
}

GarminAuthResult GarminConnectClient::signIn(const std::string& email, const std::string& password) {
    resetSignIn();
    sso_ = new Http();
    if (!sso_->c) return result(GarminAuth::Failed, "curl could not start");

    // 1. The sign-in page, for its cookies.
    const std::string signin = endpoints_.sso + "/mobile/sso/en_US/sign-in?" +
                               form({{"clientId", kIosClientId}, {"service", endpoints_.service_url}});
    const Reply page = perform(sso_->c, signin,
                               {std::string("User-Agent: ") + kIosLoginUa,
                                "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
                                "Accept-Language: en-US,en;q=0.9"},
                               nullptr);
    if (page.status == 0) return unreachable(page);
    if (blockedStatus(page.status)) return blocked(page.status);
    if (page.status >= 400) return result(GarminAuth::Changed, kGarminChangedMessage);

    // 2. The credentials.
    const std::string body = json{{"username", email},
                                  {"password", password},
                                  {"rememberMe", true},
                                  {"captchaToken", ""}}
                                 .dump();
    const Reply r = perform(sso_->c, endpoints_.sso + "/mobile/api/login?" + loginQuery(),
                            ssoPostHeaders(), &body);
    if (r.status == 0) return unreachable(r);
    return afterCredentials(r.status, r.body);
}

GarminAuthResult GarminConnectClient::afterCredentials(long status, const std::string& body) {
    if (blockedStatus(status)) return blocked(status);
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("responseStatus") ||
        !j["responseStatus"].is_object() || !j["responseStatus"].contains("type"))
        return result(GarminAuth::Changed, kGarminChangedMessage);
    const std::string type = str(j["responseStatus"], "type");

    if (type == "MFA_REQUIRED") {
        mfa_method_ = j.contains("customerMfaInfo") ? str(j["customerMfaInfo"], "mfaLastMethodUsed") : "";
        if (mfa_method_.empty()) mfa_method_ = "email";
        mfa_pending_ = true;
        return result(GarminAuth::MfaRequired, "Garmin sent a code (" + mfa_method_ + ")");
    }
    if (type == "SUCCESSFUL") {
        const std::string ticket = str(j, "serviceTicketId");
        if (ticket.empty()) return result(GarminAuth::Changed, kGarminChangedMessage);
        return exchangeTicket(ticket);
    }
    resetSignIn();
    if (type == "INVALID_USERNAME_PASSWORD")
        return result(GarminAuth::BadPassword, "Garmin refused the email or password");
    if (type == "CAPTCHA_REQUIRED")
        return result(GarminAuth::Blocked, "Garmin asked for a CAPTCHA; it is tried again on the next event");
    if (j.contains("error") && str(j["error"], "status-code") == "429")
        return blocked(429);
    if (type.empty()) return result(GarminAuth::Changed, kGarminChangedMessage);
    return result(GarminAuth::Failed, "Garmin answered " + type);
}

GarminAuthResult GarminConnectClient::verifyMfa(const std::string& code) {
    if (!mfa_pending_ || !sso_) return result(GarminAuth::Failed, "no Garmin sign-in is waiting for a code");
    const std::string body = json{{"mfaMethod", mfa_method_},
                                  {"mfaVerificationCode", code},
                                  {"rememberMyBrowser", true},
                                  {"reconsentList", json::array()},
                                  {"mfaSetup", false}}
                                 .dump();
    const Reply r = perform(sso_->c, endpoints_.sso + "/mobile/api/mfa/verifyCode?" + loginQuery(),
                            ssoPostHeaders(), &body);
    if (r.status == 0) return unreachable(r);
    if (blockedStatus(r.status)) return blocked(r.status);
    const json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("responseStatus") ||
        !j["responseStatus"].is_object())
        return result(GarminAuth::Changed, kGarminChangedMessage);
    const std::string type = str(j["responseStatus"], "type");
    if (type != "SUCCESSFUL")
        // The sign-in stays open: another code can be tried.
        return result(GarminAuth::BadMfaCode, "Garmin did not accept the code");
    const std::string ticket = str(j, "serviceTicketId");
    if (ticket.empty()) return result(GarminAuth::Changed, kGarminChangedMessage);
    mfa_pending_ = false;
    return exchangeTicket(ticket);
}

GarminAuthResult GarminConnectClient::exchangeTicket(const std::string& ticket) {
    // The ticket is single use and the SSO session has done its job.
    resetSignIn();
    for (const auto& id : diClientIds()) {
        auto headers = nativeHeaders();
        headers.push_back("Authorization: " + basicAuth(id));
        headers.push_back("Accept: application/json,text/html;q=0.9,*/*;q=0.8");
        headers.push_back("Content-Type: application/x-www-form-urlencoded");
        headers.push_back("Cache-Control: no-cache");
        const std::string body = form({{"client_id", id},
                                       {"service_ticket", ticket},
                                       {"grant_type", kDiGrantType},
                                       {"service_url", endpoints_.service_url}});
        const Reply r = oneShot(endpoints_.diauth, headers, &body);
        if (r.status == 0) return unreachable(r);
        if (r.status == 429) return blocked(429);
        if (r.status < 200 || r.status >= 300) continue;
        const json j = json::parse(r.body, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("access_token") ||
            !j["access_token"].is_string())
            continue;
        GarminTokens t;
        t.access_token  = j["access_token"].get<std::string>();
        t.refresh_token = str(j, "refresh_token");
        const std::string claimed = MyAirClient::unverifiedJwtClaim(t.access_token, "client_id");
        t.client_id = claimed.empty() ? id : claimed;
        tokens_ = std::move(t);
        return result(GarminAuth::Ok, "signed in");
    }
    return result(GarminAuth::Changed, kGarminChangedMessage);
}

GarminAuthResult GarminConnectClient::refresh() {
    if (tokens_.refresh_token.empty() || tokens_.client_id.empty())
        return result(GarminAuth::Expired, "no Garmin refresh token");
    auto headers = nativeHeaders();
    headers.push_back("Authorization: " + basicAuth(tokens_.client_id));
    headers.push_back("Accept: application/json");
    headers.push_back("Content-Type: application/x-www-form-urlencoded");
    headers.push_back("Cache-Control: no-cache");
    const std::string body = form({{"grant_type", "refresh_token"},
                                   {"client_id", tokens_.client_id},
                                   {"refresh_token", tokens_.refresh_token}});
    const Reply r = oneShot(endpoints_.diauth, headers, &body);
    if (r.status == 0) return unreachable(r);
    if (r.status == 429) return blocked(429);
    if (r.status < 200 || r.status >= 300)
        return result(GarminAuth::Expired, "Garmin refused the refresh (HTTP " + std::to_string(r.status) + ")");
    const json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("access_token") || !j["access_token"].is_string())
        return result(GarminAuth::Changed, kGarminChangedMessage);
    tokens_.access_token = j["access_token"].get<std::string>();
    if (j.contains("refresh_token") && j["refresh_token"].is_string())
        tokens_.refresh_token = j["refresh_token"].get<std::string>();
    const std::string claimed = MyAirClient::unverifiedJwtClaim(tokens_.access_token, "client_id");
    if (!claimed.empty()) tokens_.client_id = claimed;
    return result(GarminAuth::Ok, "refreshed");
}

GarminApiResult GarminConnectClient::get(const std::string& path_and_query) {
    GarminApiResult out;
    if (tokens_.empty()) {
        out.auth = GarminAuth::Expired;
        out.message = "not signed in to Garmin";
        return out;
    }
    const auto fail = [&out](const GarminAuthResult& a) {
        out.auth = a.state == GarminAuth::Ok ? GarminAuth::Failed : a.state;
        out.message = a.message;
        return out;
    };
    if (tokens_.access_token.empty() || expiresSoon(tokens_.access_token, now())) {
        const auto a = refresh();
        if (!a.ok()) return fail(a);
    }
    const auto call = [&]() {
        auto headers = nativeHeaders();
        headers.push_back("Authorization: Bearer " + tokens_.access_token);
        headers.push_back("Accept: application/json");
        return oneShot(endpoints_.connectapi + path_and_query, headers, nullptr);
    };
    Reply r = call();
    if (r.status == 401) {
        const auto a = refresh();
        if (!a.ok()) return fail(a);
        r = call();
    }
    out.status = r.status;
    if (r.status == 0) {
        out.auth = GarminAuth::Unreachable;
        out.message = "Garmin did not answer (" + r.error + ")";
        return out;
    }
    if (r.status == 401) {
        out.auth = GarminAuth::Expired;
        out.message = "Garmin refused the session";
        return out;
    }
    if (blockedStatus(r.status)) {
        out.auth = GarminAuth::Blocked;
        out.message = "Garmin is refusing this client for now (HTTP " + std::to_string(r.status) + ")";
        return out;
    }
    if (r.status == 204) {
        out.ok = true;
        out.body = nullptr;
        return out;
    }
    if (r.status >= 400) {
        out.auth = GarminAuth::Failed;
        out.message = "Garmin answered HTTP " + std::to_string(r.status);
        return out;
    }
    out.body = json::parse(r.body, nullptr, false);
    if (out.body.is_discarded()) {
        out.body = nullptr;
        out.auth = GarminAuth::Changed;
        out.message = kGarminChangedMessage;
        return out;
    }
    out.ok = true;
    return out;
}

GarminApiResult GarminConnectClient::sleepData(const std::string& display_name, const std::string& date) {
    return get("/wellness-service/wellness/dailySleepData/" + urlEncode(display_name) + "?" +
               form({{"date", date}, {"nonSleepBufferMinutes", "60"}}));
}

const char* garminAuthName(GarminAuth a) {
    switch (a) {
        case GarminAuth::Ok:          return "ok";
        case GarminAuth::MfaRequired: return "mfa_required";
        case GarminAuth::BadPassword: return "bad_password";
        case GarminAuth::BadMfaCode:  return "bad_mfa_code";
        case GarminAuth::Blocked:     return "blocked";
        case GarminAuth::Changed:     return "changed";
        case GarminAuth::Unreachable: return "unreachable";
        case GarminAuth::Expired:     return "expired";
        case GarminAuth::Failed:      return "failed";
    }
    return "failed";
}

}  // namespace hms_cpap
