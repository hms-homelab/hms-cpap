#include "clients/O2RingClient.h"
#include <iostream>
#include <nlohmann/json.hpp>

namespace hms_cpap {

O2RingClient::O2RingClient(const std::string& base_url)
    : curl_(curl_easy_init()),
      base_url_(base_url) {
    if (!curl_) {
        throw std::runtime_error("Failed to initialize CURL");
    }
}

O2RingClient::~O2RingClient() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
}

size_t O2RingClient::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total_size);
    return total_size;
}

size_t O2RingClient::WriteBinaryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* vec = static_cast<std::vector<uint8_t>*>(userp);
    auto* bytes = static_cast<uint8_t*>(contents);
    vec->insert(vec->end(), bytes, bytes + total_size);
    return total_size;
}

std::string O2RingClient::httpGet(const std::string& url) {
    std::string response;

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        std::string error = std::string("HTTP GET failed (") + url + "): " + curl_easy_strerror(res);
        std::cerr << error << std::endl;
        throw std::runtime_error(error);
    }

    return response;
}

bool O2RingClient::isConnected() {
    try {
        std::string url = base_url_ + "/o2ring/status";
        std::string response = httpGet(url);

        auto json = nlohmann::json::parse(response);
        if (json.contains("battery") && json["battery"].is_number()) {
            cached_battery_ = json["battery"].get<int>();
        }
        return json.value("connected", false);

    } catch (const std::exception& e) {
        std::cerr << "O2Ring: Status check failed: " << e.what() << std::endl;
        return false;
    }
}

std::vector<std::string> O2RingClient::listFiles() {
    std::vector<std::string> files;

    try {
        std::string url = base_url_ + "/o2ring/files";
        std::string response = httpGet(url);

        auto json = nlohmann::json::parse(response);

        if (json.contains("battery") && json["battery"].is_number()) {
            cached_battery_ = json["battery"].get<int>();
        }

        if (json.contains("files") && json["files"].is_array()) {
            for (const auto& f : json["files"]) {
                if (f.is_string()) {
                    files.push_back(f.get<std::string>());
                }
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "O2Ring: List files failed: " << e.what() << std::endl;
    }

    return files;
}

std::vector<uint8_t> O2RingClient::downloadFile(const std::string& filename) {
    std::vector<uint8_t> data;

    try {
        // URL-encode the filename parameter
        char* encoded = curl_easy_escape(curl_, filename.c_str(),
                                          static_cast<int>(filename.size()));
        std::string url = base_url_ + "/o2ring/files?name=" + std::string(encoded);
        curl_free(encoded);

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteBinaryCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, DOWNLOAD_TIMEOUT);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

        CURLcode res = curl_easy_perform(curl_);
        if (res != CURLE_OK) {
            std::cerr << "O2Ring: Download failed (" << filename << "): "
                      << curl_easy_strerror(res) << std::endl;
            data.clear();
            return data;
        }

        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code != 200) {
            std::cerr << "O2Ring: HTTP " << http_code << " for " << filename << std::endl;
            data.clear();
            return data;
        }

        if (data.empty()) {
            std::cerr << "O2Ring: Empty file " << filename << std::endl;
            return data;
        }

        std::cout << "O2Ring: " << filename << " -> " << data.size() << " bytes" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "O2Ring: Download error (" << filename << "): " << e.what() << std::endl;
        data.clear();
    }

    return data;
}

} // namespace hms_cpap
