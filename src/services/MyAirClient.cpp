#include "services/MyAirClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>
#include <ctime>

namespace hms_cpap {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------
// Two exist. The values are ResMed's own, taken from their web app, and are
// constants in the sense that we cannot choose them; if ResMed rotates one, this
// stops working until the new value is read off the app again.

const MyAirRegion& myAirRegionNA() {
    static const MyAirRegion r{
        "myAir",
        "resmed-ext-1.okta.com",
        "aus4ccsxvnidQgLmA297",
        "0oa4ccq1v413ypROi297",
        "da2-cenztfjrezhwphdqtwtbpqvzui",
        "https://graphql.myair-prd.dht.live/graphql",
        "https://myair.resmed.com",
        ""  // North America has no email factor configured
    };
    return r;
}

const MyAirRegion& myAirRegionEU() {
    static const MyAirRegion r{
        "myAir EU",
        "id.resmed.eu",
        "aus2uznux2sYKTsEg417",
        "0oa2uz04d2Pks2NgR417",
        "da2-o66oo6xdnfh5hlfuw5yw5g2dtm",
        "https://graphql.hyperdrive.resmed.eu/graphql",
        "https://myair.resmed.eu",
        "emfg9cmjqxEPr52cT417"
    };
    return r;
}

const MyAirRegion& myAirRegion(const std::string& code) {
    std::string up;
    for (unsigned char c : code) up.push_back(static_cast<char>(std::toupper(c)));
    return up == "EU" ? myAirRegionEU() : myAirRegionNA();
}

// ---------------------------------------------------------------------------
// HTTP plumbing
// ---------------------------------------------------------------------------

namespace {

size_t writeCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

size_t headerCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    static_cast<std::string*>(userdata)->append(buffer, size * nitems);
    return size * nitems;
}

/// Case-insensitive lookup of one response header out of a raw header blob.
std::string headerValue(const std::string& headers, const std::string& name) {
    std::istringstream in(headers);
    std::string line;
    const std::string want = name + ":";
    while (std::getline(in, line)) {
        if (line.size() < want.size()) continue;
        bool match = true;
        for (size_t i = 0; i < want.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(line[i])) !=
                std::tolower(static_cast<unsigned char>(want[i]))) { match = false; break; }
        }
        if (!match) continue;
        std::string v = line.substr(want.size());
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        return v;
    }
    return "";
}

/// Pull one cookie's value out of every Set-Cookie line in a header blob.
std::string cookieValue(const std::string& headers, const std::string& name) {
    std::istringstream in(headers);
    std::string line;
    while (std::getline(in, line)) {
        const std::string lower_prefix = "set-cookie:";
        if (line.size() < lower_prefix.size()) continue;
        bool is_cookie = true;
        for (size_t i = 0; i < lower_prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(line[i])) != lower_prefix[i]) {
                is_cookie = false; break;
            }
        }
        if (!is_cookie) continue;
        const auto pos = line.find(name + "=");
        if (pos == std::string::npos) continue;
        const auto start = pos + name.size() + 1;
        auto end = line.find(';', start);
        if (end == std::string::npos) end = line.size();
        std::string v = line.substr(start, end - start);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        return v;
    }
    return "";
}

struct HttpResult {
    long status = 0;
    std::string body;
    std::string headers;
};

HttpResult httpRequest(const std::string& url,
                       const std::vector<std::string>& header_lines,
                       const std::string& body,      // empty for GET
                       const std::string& cookies,   // "a=b; c=d"
                       bool follow_redirects) {
    HttpResult res;
    CURL* c = curl_easy_init();
    if (!c) return res;

    struct curl_slist* h = nullptr;
    for (const auto& line : header_lines) h = curl_slist_append(h, line.c_str());

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    if (h) curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (!body.empty()) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    if (!cookies.empty()) curl_easy_setopt(c, CURLOPT_COOKIE, cookies.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, follow_redirects ? 1L : 0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &res.headers);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &res.status);
    if (h) curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return res;
}

std::string urlEncode(const std::string& s) {
    CURL* c = curl_easy_init();
    if (!c) return s;
    char* out = curl_easy_escape(c, s.c_str(), static_cast<int>(s.size()));
    std::string result = out ? out : s;
    if (out) curl_free(out);
    curl_easy_cleanup(c);
    return result;
}

bool parseJson(const std::string& s, json& out) {
    out = json::parse(s, nullptr, false);
    return !out.is_discarded();
}

/// A JSON field that ResMed sometimes sends as a number and sometimes as a
/// string. Accepting both is cheaper than discovering which on a Sunday.
double numberOf(const json& j, const char* key, double fallback = 0) {
    if (!j.contains(key) || j[key].is_null()) return fallback;
    if (j[key].is_number()) return j[key].get<double>();
    if (j[key].is_string()) {
        try { return std::stod(j[key].get<std::string>()); } catch (...) {}
    }
    return fallback;
}

std::string stringOf(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return "";
    if (j[key].is_string()) return j[key].get<std::string>();
    return j[key].dump();
}

}  // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

std::string MyAirClient::base64Url(const unsigned char* data, size_t len) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (static_cast<unsigned>(data[i]) << 16) |
                           (i + 1 < len ? static_cast<unsigned>(data[i + 1]) << 8 : 0u) |
                           (i + 2 < len ? static_cast<unsigned>(data[i + 2]) : 0u);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        if (i + 1 < len) out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        if (i + 2 < len) out.push_back(kAlphabet[v & 0x3F]);
    }
    return out;  // unpadded, which is what RFC 7636 wants
}

std::string MyAirClient::makeCodeVerifier() {
    unsigned char buf[40];
    if (RAND_bytes(buf, sizeof(buf)) != 1) return "";
    std::string s = base64Url(buf, sizeof(buf));
    // Strip to alphanumerics. The spec allows -._~ as well; dropping them costs
    // a little entropy out of 320 bits and avoids every escaping question.
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](unsigned char c) { return !std::isalnum(c); }),
            s.end());
    return s;
}

std::string MyAirClient::pkceChallenge(const std::string& verifier) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, verifier.data(), verifier.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }
    EVP_MD_CTX_free(ctx);
    return base64Url(digest, len);
}

std::string MyAirClient::codeFromRedirect(const std::string& location) {
    // The code comes back in the FRAGMENT, after '#', because the authorize call
    // asks for response_mode=fragment. Reading it as a query string finds
    // nothing and looks exactly like a failed login.
    const auto hash = location.find('#');
    if (hash == std::string::npos) return "";
    const std::string fragment = location.substr(hash + 1);

    std::istringstream in(fragment);
    std::string pair;
    while (std::getline(in, pair, '&')) {
        const auto eq = pair.find('=');
        if (eq == std::string::npos) continue;
        if (pair.substr(0, eq) == "code") return pair.substr(eq + 1);
    }
    return "";
}

std::string MyAirClient::unverifiedJwtClaim(const std::string& jwt, const std::string& claim) {
    const auto first = jwt.find('.');
    if (first == std::string::npos) return "";
    const auto second = jwt.find('.', first + 1);
    if (second == std::string::npos) return "";

    std::string payload = jwt.substr(first + 1, second - first - 1);
    // base64url back to base64, then pad.
    for (auto& c : payload) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (payload.size() % 4) payload.push_back('=');

    std::string decoded(payload.size(), '\0');
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(&decoded[0]),
                                  reinterpret_cast<const unsigned char*>(payload.data()),
                                  static_cast<int>(payload.size()));
    if (n <= 0) return "";
    decoded.resize(static_cast<size_t>(n));
    // EVP_DecodeBlock counts the padding it added, so trim any trailing NULs.
    while (!decoded.empty() && decoded.back() == '\0') decoded.pop_back();

    json j;
    if (!parseJson(decoded, j) || !j.is_object()) return "";
    return stringOf(j, claim.c_str());
}

bool MyAirClient::parseSleepRecords(const std::string& body,
                                    std::vector<MyAirSleepRecord>& out, std::string& err) {
    json j;
    if (!parseJson(body, j)) { err = "myAir returned something that is not JSON"; return false; }
    if (j.contains("errors") && !j["errors"].empty()) {
        err = "myAir GraphQL error: " + j["errors"].dump().substr(0, 300);
        return false;
    }
    if (!j.contains("data") || !j["data"].contains("getPatientWrapper")) {
        err = "myAir response has no getPatientWrapper";
        return false;
    }
    const auto& wrapper = j["data"]["getPatientWrapper"];
    if (!wrapper.contains("sleepRecords") || !wrapper["sleepRecords"].contains("items")) {
        err = "myAir response has no sleepRecords.items";
        return false;
    }
    const auto& items = wrapper["sleepRecords"]["items"];
    if (!items.is_array()) { err = "myAir sleepRecords.items is not an array"; return false; }

    out.clear();
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        MyAirSleepRecord r;
        r.start_date      = stringOf(item, "startDate");
        r.total_usage_min = numberOf(item, "totalUsage");
        r.sleep_score     = static_cast<int>(numberOf(item, "sleepScore"));
        r.usage_score     = static_cast<int>(numberOf(item, "usageScore"));
        r.ahi_score       = static_cast<int>(numberOf(item, "ahiScore"));
        r.mask_score      = static_cast<int>(numberOf(item, "maskScore"));
        r.leak_score      = static_cast<int>(numberOf(item, "leakScore"));
        r.ahi             = numberOf(item, "ahi");
        r.mask_pair_count = static_cast<int>(numberOf(item, "maskPairCount"));
        r.leak_percentile = numberOf(item, "leakPercentile");
        if (r.start_date.empty()) continue;  // a night with no date is not a night
        out.push_back(std::move(r));
    }
    return true;
}

bool MyAirClient::parseDevice(const std::string& body, MyAirDevice& out, std::string& err) {
    json j;
    if (!parseJson(body, j)) { err = "myAir returned something that is not JSON"; return false; }
    if (j.contains("errors") && !j["errors"].empty()) {
        err = "myAir GraphQL error: " + j["errors"].dump().substr(0, 300);
        return false;
    }
    if (!j.contains("data") || !j["data"].contains("getPatientWrapper")) {
        err = "myAir response has no getPatientWrapper";
        return false;
    }
    const auto& wrapper = j["data"]["getPatientWrapper"];
    if (!wrapper.contains("fgDevices") || !wrapper["fgDevices"].is_array() ||
        wrapper["fgDevices"].empty()) {
        err = "myAir reports no flow generator on this account";
        return false;
    }
    const auto& d = wrapper["fgDevices"][0];
    out.serial_number  = stringOf(d, "serialNumber");
    out.localized_name = stringOf(d, "localizedName");
    out.device_series  = stringOf(d, "deviceSeries");
    out.device_family  = stringOf(d, "deviceFamily");
    out.last_sleep_data_report_time = stringOf(d, "lastSleepDataReportTime");
    out.manufacturer_name = stringOf(d, "fgDeviceManufacturerName");

    // The mask is a separate list, and an account can legitimately have none.
    if (wrapper.contains("masks") && wrapper["masks"].is_array() && !wrapper["masks"].empty()) {
        out.mask_code = stringOf(wrapper["masks"][0], "maskCode");
    }
    return true;
}

// ---------------------------------------------------------------------------
// The login flow
// ---------------------------------------------------------------------------

MyAirClient::MyAirClient(std::string username, std::string password, std::string region_code)
    : username_(std::move(username)),
      password_(std::move(password)),
      region_(&myAirRegion(region_code)) {}

MyAirAuthState MyAirClient::connect(std::string& err) {
    err.clear();
    if (username_.empty() || password_.empty()) {
        err = "myAir username and password are both required";
        return MyAirAuthState::Failed;
    }

    json body{{"username", username_}, {"password", password_}};
    const std::vector<std::string> headers{
        "Content-Type: application/json", "Accept: application/json"};

    std::string cookies;
    if (!device_token_.empty()) cookies = "DT=" + device_token_;

    const auto res = httpRequest(region_->authnUrl(), headers, body.dump(), cookies, true);
    if (res.status != 200) {
        json j;
        if (parseJson(res.body, j) && j.contains("errorSummary")) {
            err = "myAir sign-in refused: " + stringOf(j, "errorSummary");
        } else {
            err = "myAir sign-in failed (HTTP " + std::to_string(res.status) + ")";
        }
        return MyAirAuthState::Failed;
    }

    // Carry the cookies forward: DT is the remembered device and is what keeps
    // ResMed from emailing a code on every single start.
    const auto dt = cookieValue(res.headers, "DT");
    if (!dt.empty()) device_token_ = dt;
    const auto sid = cookieValue(res.headers, "sid");
    if (!sid.empty()) session_cookie_ = sid;

    json j;
    if (!parseJson(res.body, j)) { err = "myAir sign-in returned no JSON"; return MyAirAuthState::Failed; }

    const std::string status = stringOf(j, "status");
    if (status == "SUCCESS") {
        session_token_ = stringOf(j, "sessionToken");
        if (session_token_.empty()) { err = "myAir sign-in gave no sessionToken"; return MyAirAuthState::Failed; }
        return exchangeSessionForTokens(err) ? MyAirAuthState::Ok : MyAirAuthState::Failed;
    }

    if (status == "MFA_REQUIRED" || status == "MFA_CHALLENGE") {
        state_token_ = stringOf(j, "stateToken");
        if (state_token_.empty()) { err = "myAir asked for MFA without a stateToken"; return MyAirAuthState::Failed; }

        // Find the email factor. The region default is a fallback for the shape
        // where Okta does not enumerate factors.
        mfa_factor_id_ = region_->email_factor_id;
        if (j.contains("_embedded") && j["_embedded"].contains("factors")) {
            for (const auto& f : j["_embedded"]["factors"]) {
                if (stringOf(f, "factorType") == "email") { mfa_factor_id_ = stringOf(f, "id"); break; }
            }
        }
        if (mfa_factor_id_.empty()) { err = "myAir asked for MFA but named no email factor"; return MyAirAuthState::Failed; }

        // An empty passCode is the documented way to ask Okta to SEND the code.
        json trigger{{"passCode", ""}, {"stateToken", state_token_}};
        const auto t = httpRequest(region_->mfaUrl(mfa_factor_id_), headers, trigger.dump(),
                                   cookies, true);
        if (t.status != 200 && t.status != 403) {
            err = "myAir would not send the MFA code (HTTP " + std::to_string(t.status) + ")";
            return MyAirAuthState::Failed;
        }
        return MyAirAuthState::MfaRequired;
    }

    err = "myAir sign-in returned an unhandled status: " + status;
    return MyAirAuthState::Failed;
}

MyAirAuthState MyAirClient::verifyMfa(const std::string& code, std::string& err) {
    err.clear();
    if (state_token_.empty()) { err = "no myAir sign-in is waiting for a code"; return MyAirAuthState::Failed; }
    if (code.empty()) { err = "the emailed myAir code is required"; return MyAirAuthState::Failed; }

    const std::vector<std::string> headers{
        "Content-Type: application/json", "Accept: application/json"};
    std::string cookies;
    if (!device_token_.empty()) cookies = "DT=" + device_token_;

    json body{{"passCode", code}, {"stateToken", state_token_}};
    const auto res = httpRequest(region_->mfaUrl(mfa_factor_id_), headers, body.dump(), cookies, true);

    const auto dt = cookieValue(res.headers, "DT");
    if (!dt.empty()) device_token_ = dt;

    json j;
    if (res.status != 200 || !parseJson(res.body, j)) {
        err = "myAir rejected the code (HTTP " + std::to_string(res.status) + ")";
        return MyAirAuthState::Failed;
    }
    if (stringOf(j, "status") != "SUCCESS") {
        err = "myAir rejected the code";
        return MyAirAuthState::Failed;
    }
    session_token_ = stringOf(j, "sessionToken");
    state_token_.clear();
    if (session_token_.empty()) { err = "myAir verified the code but gave no sessionToken"; return MyAirAuthState::Failed; }

    return exchangeSessionForTokens(err) ? MyAirAuthState::Ok : MyAirAuthState::Failed;
}

bool MyAirClient::exchangeSessionForTokens(std::string& err) {
    const std::string verifier = makeCodeVerifier();
    if (verifier.empty()) { err = "could not generate a PKCE verifier"; return false; }
    const std::string challenge = pkceChallenge(verifier);

    std::ostringstream url;
    url << region_->authorizeUrl()
        << "?client_id=" << urlEncode(region_->client_id)
        << "&code_challenge=" << urlEncode(challenge)
        << "&code_challenge_method=S256"
        << "&prompt=none"
        << "&redirect_uri=" << urlEncode(region_->redirect_url)
        << "&response_mode=fragment"
        << "&response_type=code"
        << "&sessionToken=" << urlEncode(session_token_)
        << "&scope=" << urlEncode("openid profile email")
        << "&state=hmscpap";

    std::string cookies;
    if (!device_token_.empty()) cookies = "DT=" + device_token_;
    if (!session_cookie_.empty()) {
        if (!cookies.empty()) cookies += "; ";
        cookies += "sid=" + session_cookie_;
    }

    // Redirects MUST NOT be followed: the answer IS the Location header.
    const auto res = httpRequest(url.str(), {"Accept: application/json"}, "", cookies, false);
    const std::string location = headerValue(res.headers, "location");
    if (location.empty()) {
        err = "myAir did not redirect after sign-in (HTTP " + std::to_string(res.status) + ")";
        return false;
    }
    const std::string code = codeFromRedirect(location);
    if (code.empty()) {
        err = "myAir redirect carried no authorization code";
        return false;
    }

    const auto dt = cookieValue(res.headers, "DT");
    if (!dt.empty()) device_token_ = dt;

    std::ostringstream form;
    form << "client_id=" << urlEncode(region_->client_id)
         << "&redirect_uri=" << urlEncode(region_->redirect_url)
         << "&grant_type=authorization_code"
         << "&code_verifier=" << urlEncode(verifier)
         << "&code=" << urlEncode(code);

    const auto tok = httpRequest(region_->tokenUrl(),
                                 {"Accept: application/json",
                                  "Content-Type: application/x-www-form-urlencoded"},
                                 form.str(), cookies, false);
    json j;
    if (tok.status != 200 || !parseJson(tok.body, j)) {
        err = "myAir token exchange failed (HTTP " + std::to_string(tok.status) + ")";
        return false;
    }
    access_token_ = stringOf(j, "access_token");
    id_token_ = stringOf(j, "id_token");
    if (access_token_.empty() || id_token_.empty()) {
        err = "myAir token exchange returned no tokens";
        return false;
    }
    session_token_.clear();
    country_code_.clear();
    return true;
}

std::string MyAirClient::countryCode() {
    if (country_code_.empty() && !id_token_.empty()) {
        country_code_ = unverifiedJwtClaim(id_token_, "myAirCountryId");
    }
    return country_code_;
}

bool MyAirClient::graphql(const std::string& operation_name, const std::string& query,
                          std::string& body, std::string& err) {
    if (!isAuthenticated()) { err = "not signed in to myAir"; return false; }
    const std::string country = countryCode();
    if (country.empty()) {
        err = "myAir did not tell us which country this account belongs to";
        return false;
    }

    const std::vector<std::string> headers{
        "Content-Type: application/json",
        "x-api-key: " + region_->api_key,
        "Authorization: Bearer " + access_token_,
        // ResMed's app sends these on every call. They are not authentication,
        // but the endpoint is fussy about their presence.
        "rmdhandsetid: 02c1c662-c289-41fd-a9ae-196ff15b5166",
        "rmdlanguage: en",
        "rmdhandsetmodel: Chrome",
        "rmdhandsetosversion: 127.0.6533.119",
        "rmdproduct: " + region_->product,
        "rmdappversion: 1.0.0",
        "rmdhandsetplatform: Web",
        "rmdcountry: " + country,
        "accept-language: en-US,en;q=0.9",
    };

    json payload{{"operationName", operation_name}, {"variables", json::object()}, {"query", query}};
    const auto res = httpRequest(region_->graphql_url, headers, payload.dump(), "", true);
    if (res.status != 200) {
        err = "myAir query failed (HTTP " + std::to_string(res.status) + ")";
        return false;
    }
    body = res.body;
    return true;
}

bool MyAirClient::fetchSleepRecords(std::vector<MyAirSleepRecord>& out, std::string& err) {
    // The window is fixed at 30 days by the API; there is no history endpoint.
    // Dates are formatted by the caller's clock, which is the same clock the
    // nights are filed under locally.
    const std::time_t now = std::time(nullptr);
    std::tm today{};
#ifdef _WIN32
    localtime_s(&today, &now);
#else
    localtime_r(&now, &today);
#endif
    char today_buf[16], month_ago_buf[16];
    std::strftime(today_buf, sizeof(today_buf), "%Y-%m-%d", &today);

    const std::time_t then = now - 30 * 24 * 60 * 60;
    std::tm past{};
#ifdef _WIN32
    localtime_s(&past, &then);
#else
    localtime_r(&then, &past);
#endif
    std::strftime(month_ago_buf, sizeof(month_ago_buf), "%Y-%m-%d", &past);

    const std::string query =
        std::string("query GetPatientSleepRecords { getPatientWrapper { patient { firstName } "
                    "sleepRecords(startMonth: \"") + month_ago_buf +
        "\", endMonth: \"" + today_buf + "\") { items { startDate totalUsage sleepScore "
        "usageScore ahiScore maskScore leakScore ahi maskPairCount leakPercentile "
        "sleepRecordPatientId __typename } __typename } __typename } }";

    std::string body;
    if (!graphql("GetPatientSleepRecords", query, body, err)) return false;
    return parseSleepRecords(body, out, err);
}

bool MyAirClient::fetchDevice(MyAirDevice& out, std::string& err) {
    const std::string query =
        "query getPatientWrapper { getPatientWrapper { masks { maskCode } fgDevices { "
        "serialNumber localizedName deviceSeries deviceFamily lastSleepDataReportTime "
        "fgDeviceManufacturerName fgDevicePatientId } } }";

    std::string body;
    if (!graphql("getPatientWrapper", query, body, err)) return false;
    return parseDevice(body, out, err);
}

}  // namespace hms_cpap
