#include "clients/AirSense11Client.h"

#include <iostream>
#include <stdexcept>

namespace hms_cpap {

AirSense11Client::AirSense11Client(const std::string& base_url)
    : curl_(curl_easy_init()),
      base_url_(base_url) {
    if (!curl_) {
        throw std::runtime_error("Failed to initialize CURL");
    }
}

AirSense11Client::~AirSense11Client() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
}

size_t AirSense11Client::WriteBinaryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* vec = static_cast<std::vector<uint8_t>*>(userp);
    auto* bytes = static_cast<uint8_t*>(contents);
    vec->insert(vec->end(), bytes, bytes + total_size);
    return total_size;
}

std::vector<uint8_t> AirSense11Client::fetchSummary(const std::string& from) {
    std::vector<uint8_t> data;

    char* encoded = curl_easy_escape(curl_, from.c_str(), static_cast<int>(from.size()));
    const std::string url = base_url_ + "/airsense11/summary?from=" + std::string(encoded ? encoded : "");
    curl_free(encoded);

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteBinaryCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, DOWNLOAD_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        // A bridge that is off fails every pull; say so once, then summarise.
        auto decision = http_fail_log_.onFailure(
            std::string("AirSense11: summary request failed (") + url + "): " + curl_easy_strerror(res));
        if (decision.log) std::cerr << decision.message << std::endl;
        return {};
    }
    if (auto recovered = http_fail_log_.onSuccess(); recovered.log)
        std::cerr << "AirSense11 " << recovered.message << std::endl;

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        // The bridge answered, the machine did not (408), or the bridge has no
        // pairing yet (409): nothing to read this time, nothing to retry at once.
        std::cerr << "AirSense11: bridge answered HTTP " << http_code
                  << (http_code == 409 ? " (not paired with the machine yet)"
                      : http_code == 408 ? " (the machine did not answer)" : "")
                  << std::endl;
        return {};
    }
    return data;
}

} // namespace hms_cpap
