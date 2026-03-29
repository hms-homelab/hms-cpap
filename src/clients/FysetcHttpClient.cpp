#include "clients/FysetcHttpClient.h"

#include <json/json.h>
#include <iostream>
#include <sstream>

namespace hms_cpap {

FysetcHttpClient::FysetcHttpClient() : curl_(curl_easy_init()) {
    if (!curl_) {
        throw std::runtime_error("Failed to initialize CURL for FysetcHttpClient");
    }
}

FysetcHttpClient::~FysetcHttpClient() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
}

void FysetcHttpClient::setBaseURL(const std::string& url) {
    base_url_ = url;
    // Strip trailing slash
    if (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
}

// ── Callbacks ───────────────────────────────────────────────────────────────

size_t FysetcHttpClient::WriteStringCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

size_t FysetcHttpClient::WriteBinaryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* vec = static_cast<std::vector<uint8_t>*>(userp);
    auto* bytes = static_cast<uint8_t*>(contents);
    vec->insert(vec->end(), bytes, bytes + total);
    return total;
}

// ── HTTP helpers ────────────────────────────────────────────────────────────

std::string FysetcHttpClient::httpGet(const std::string& url, long timeout) {
    std::string response;
    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteStringCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        std::cerr << "FysetcHttp: GET " << url << " failed: "
                  << curl_easy_strerror(res) << std::endl;
        return "";
    }
    return response;
}

std::vector<uint8_t> FysetcHttpClient::httpGetBinary(const std::string& url, int& out_status) {
    std::vector<uint8_t> data;
    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteBinaryCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, DOWNLOAD_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        std::cerr << "FysetcHttp: GET " << url << " failed: "
                  << curl_easy_strerror(res) << std::endl;
        out_status = -1;
        return {};
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    out_status = static_cast<int>(http_code);

    if (http_code != 200) {
        data.clear();
    }
    return data;
}

std::string FysetcHttpClient::httpPostJson(const std::string& url, const std::string& json_body) {
    std::string response;
    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteStringCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        std::cerr << "FysetcHttp: POST " << url << " failed: "
                  << curl_easy_strerror(res) << std::endl;
        return "";
    }
    return response;
}

// ── Protocol methods ────────────────────────────────────────────────────────

bool FysetcHttpClient::init(int poll_interval_sec,
                            const std::vector<std::pair<std::string, int64_t>>& known_files,
                            int& out_poll_interval) {
    Json::Value body;
    body["poll_interval_sec"] = poll_interval_sec;

    Json::Value files_arr(Json::arrayValue);
    for (const auto& [path, offset] : known_files) {
        Json::Value entry;
        entry["path"] = path;
        entry["size"] = static_cast<Json::Int64>(offset);
        files_arr.append(entry);
    }
    body["known_files"] = files_arr;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string json_str = Json::writeString(builder, body);

    std::string resp = httpPostJson(base_url_ + "/init", json_str);
    if (resp.empty()) return false;

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::istringstream ss(resp);
    std::string errs;
    if (!Json::parseFromStream(reader, ss, &root, &errs)) {
        std::cerr << "FysetcHttp: /init parse error: " << errs << std::endl;
        return false;
    }

    if (root["status"].asString() != "ok") {
        std::cerr << "FysetcHttp: /init returned status=" << root["status"].asString() << std::endl;
        return false;
    }

    out_poll_interval = root.get("poll_interval_sec", poll_interval_sec).asInt();
    std::cout << "FysetcHttp: /init ok (poll_interval=" << out_poll_interval
              << "s, tracked=" << root.get("n_tracked", 0).asInt() << ")" << std::endl;
    return true;
}

std::optional<FysetcPollResponse> FysetcHttpClient::poll() {
    std::string resp = httpGet(base_url_ + "/poll");
    if (resp.empty()) return std::nullopt;

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::istringstream ss(resp);
    std::string errs;
    if (!Json::parseFromStream(reader, ss, &root, &errs)) {
        std::cerr << "FysetcHttp: /poll parse error: " << errs << std::endl;
        return std::nullopt;
    }

    FysetcPollResponse result;
    result.status = root["status"].asString();
    result.cycle = root.get("cycle", 0).asInt();
    result.polled_at = root.get("polled_at", 0).asInt64();
    result.next_poll_sec = root.get("next_poll_sec", 65).asInt();

    if (result.status == "ok") {
        for (const auto& d : root["new_dirs"]) {
            result.new_dirs.push_back({d["path"].asString(), d["name"].asString()});
        }
        for (const auto& f : root["new_files"]) {
            result.new_files.push_back({
                f["path"].asString(),
                f["name"].asString(),
                f.get("size", 0).asInt64()
            });
        }
        for (const auto& d : root["deltas"]) {
            result.deltas.push_back({
                d["path"].asString(),
                d["name"].asString(),
                d.get("from", 0).asInt64(),
                d.get("to", 0).asInt64()
            });
        }
    }

    return result;
}

std::vector<uint8_t> FysetcHttpClient::fetchFile(const std::string& path,
                                                  int64_t offset,
                                                  int& out_status) {
    // URL-encode the path parameter
    char* encoded_path = curl_easy_escape(curl_, path.c_str(), static_cast<int>(path.size()));
    std::string url = base_url_ + "/file?path=" + std::string(encoded_path);
    curl_free(encoded_path);

    if (offset > 0) {
        url += "&offset=" + std::to_string(offset);
    }

    return httpGetBinary(url, out_status);
}

std::optional<FysetcAckResponse> FysetcHttpClient::ack(
    const std::vector<std::pair<std::string, int64_t>>& confirmed_offsets) {

    Json::Value body;
    Json::Value files_arr(Json::arrayValue);
    for (const auto& [path, offset] : confirmed_offsets) {
        Json::Value entry;
        entry["path"] = path;
        entry["offset"] = static_cast<Json::Int64>(offset);
        files_arr.append(entry);
    }
    body["files"] = files_arr;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string json_str = Json::writeString(builder, body);

    std::string resp = httpPostJson(base_url_ + "/ack", json_str);
    if (resp.empty()) return std::nullopt;

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::istringstream ss(resp);
    std::string errs;
    if (!Json::parseFromStream(reader, ss, &root, &errs)) {
        std::cerr << "FysetcHttp: /ack parse error: " << errs << std::endl;
        return std::nullopt;
    }

    FysetcAckResponse result;
    result.status = root["status"].asString();
    result.n_acked = root.get("n_acked", 0).asInt();
    result.next_poll_sec = root.get("next_poll_sec", 65).asInt();
    return result;
}

bool FysetcHttpClient::healthCheck() {
    std::string resp = httpGet(base_url_ + "/api/status");
    if (resp.empty()) return false;

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::istringstream ss(resp);
    std::string errs;
    return Json::parseFromStream(reader, ss, &root, &errs);
}

} // namespace hms_cpap
