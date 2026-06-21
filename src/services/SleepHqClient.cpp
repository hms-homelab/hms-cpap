#include "services/SleepHqClient.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <utility>

namespace hms_cpap {

using json = nlohmann::json;

static size_t writeCb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static bool parseJson(const std::string& s, json& out) {
    out = json::parse(s, nullptr, false);
    return !out.is_discarded();
}

SleepHqClient::SleepHqClient(std::string client_id, std::string client_secret)
    : client_id_(std::move(client_id)), client_secret_(std::move(client_secret)) {}

std::string SleepHqClient::httpGet(const std::string& url, long& status) const {
    CURL* c = curl_easy_init();
    if (!c) { status = 0; return ""; }
    std::string resp;
    struct curl_slist* h = nullptr;
    std::string auth = "Authorization: Bearer " + access_token_;
    h = curl_slist_append(h, auth.c_str());
    h = curl_slist_append(h, "accept: application/vnd.api+json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return resp;
}

std::string SleepHqClient::httpPostForm(const std::string& url, const std::string& form,
                                        long& status) const {
    CURL* c = curl_easy_init();
    if (!c) { status = 0; return ""; }
    std::string resp;
    struct curl_slist* h = nullptr;
    if (!access_token_.empty()) {
        std::string auth = "Authorization: Bearer " + access_token_;
        h = curl_slist_append(h, auth.c_str());
    }
    h = curl_slist_append(h, "accept: application/vnd.api+json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return resp;
}

bool SleepHqClient::connect(std::string& err) {
    if (client_id_.empty() || client_secret_.empty()) { err = "missing SleepHQ credentials"; return false; }

    // grant_type=password (client creds, no username) — client_credentials yields
    // an anonymous token where /me fails (confirmed against the live API).
    std::string form = "grant_type=password&client_id=" + client_id_ +
                       "&client_secret=" + client_secret_ + "&scope=read+write";
    long st = 0;
    std::string body = httpPostForm(token_url_, form, st);
    json j;
    if (st != 200 || !parseJson(body, j) || !j.contains("access_token")) {
        err = "SleepHQ token request failed (" + std::to_string(st) + ")";
        return false;
    }
    access_token_ = j["access_token"].get<std::string>();

    std::string me = httpGet(base_url_ + "/me", st);
    json mj;
    if (st != 200 || !parseJson(me, mj)) { err = "SleepHQ /me failed (" + std::to_string(st) + ")"; return false; }
    const auto& t = mj["data"]["current_team_id"];
    if (t.is_string())          team_id_ = t.get<std::string>();
    else if (t.is_number_integer()) team_id_ = std::to_string(t.get<long long>());
    if (team_id_.empty()) { err = "SleepHQ: no current team id"; return false; }
    return true;
}

std::string SleepHqClient::createImport(std::string& err) {
    long st = 0;
    std::string url = base_url_ + "/teams/" + team_id_ + "/imports";
    std::string body = httpPostForm(url, "programmatic=true", st);
    json j;
    if ((st != 200 && st != 201) || !parseJson(body, j)) {
        err = "SleepHQ createImport failed (" + std::to_string(st) + ")";
        return "";
    }
    const auto& id = j["data"]["id"];
    if (id.is_string())          return id.get<std::string>();
    if (id.is_number_integer())  return std::to_string(id.get<long long>());
    err = "SleepHQ createImport: no import id";
    return "";
}

std::string SleepHqClient::contentHash(const std::string& filename, const std::string& abs_file_path) {
    std::ifstream f(abs_file_path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf();
    std::string data = ss.str();
    std::string buf = filename + data;  // filename + contents (verify vs live API)

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, buf.data(), buf.size());
    EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);

    std::ostringstream o;
    for (unsigned i = 0; i < dlen; ++i)
        o << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return o.str();
}

bool SleepHqClient::uploadFile(const std::string& import_id, const std::string& name,
                               const std::string& rel_path, const std::string& abs_file_path,
                               std::string& err) {
    std::string hash = contentHash(name, abs_file_path);
    if (hash.empty()) { err = "cannot read " + abs_file_path; return false; }

    CURL* c = curl_easy_init();
    if (!c) { err = "curl init"; return false; }
    std::string resp; long st = 0;
    struct curl_slist* h = nullptr;
    std::string auth = "Authorization: Bearer " + access_token_;
    h = curl_slist_append(h, auth.c_str());
    h = curl_slist_append(h, "accept: application/vnd.api+json");

    curl_mime* mime = curl_mime_init(c);
    curl_mimepart* p;
    p = curl_mime_addpart(mime); curl_mime_name(p, "name");         curl_mime_data(p, name.c_str(), CURL_ZERO_TERMINATED);
    p = curl_mime_addpart(mime); curl_mime_name(p, "path");         curl_mime_data(p, rel_path.c_str(), CURL_ZERO_TERMINATED);
    p = curl_mime_addpart(mime); curl_mime_name(p, "content_hash"); curl_mime_data(p, hash.c_str(), CURL_ZERO_TERMINATED);
    p = curl_mime_addpart(mime); curl_mime_name(p, "file");         curl_mime_filedata(p, abs_file_path.c_str());

    std::string url = base_url_ + "/imports/" + import_id + "/files";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);

    curl_mime_free(mime);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);

    if (st != 200 && st != 201) { err = "SleepHQ upload " + name + " failed (" + std::to_string(st) + ")"; return false; }
    return true;
}

bool SleepHqClient::processFiles(const std::string& import_id, std::string& err) {
    long st = 0;
    std::string url = base_url_ + "/imports/" + import_id + "/process_files";
    httpPostForm(url, "", st);
    if (st != 200 && st != 201) { err = "SleepHQ process_files failed (" + std::to_string(st) + ")"; return false; }
    return true;
}

} // namespace hms_cpap
