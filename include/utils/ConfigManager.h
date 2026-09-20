#pragma once

#include <string>
#include <cstdlib>

namespace hms_cpap {

/**
 * ConfigManager - Environment variable configuration reader
 *
 * Reads configuration from environment variables with defaults.
 */
class ConfigManager {
public:
    /**
     * Get string config value with default
     */
    static std::string get(const std::string& key, const std::string& default_value = "") {
        const char* value = std::getenv(key.c_str());
        return value ? std::string(value) : default_value;
    }

    /**
     * Get integer config value with default
     */
    static int getInt(const std::string& key, int default_value = 0) {
        const char* value = std::getenv(key.c_str());
        return value ? std::stoi(value) : default_value;
    }

};

} // namespace hms_cpap
