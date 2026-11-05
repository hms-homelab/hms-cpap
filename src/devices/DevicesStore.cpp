#include "devices/DevicesStore.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

namespace hms_cpap::devices {

namespace fs = std::filesystem;

namespace {

std::string key(const std::string& kind, const std::string& id) { return kind + "/" + id; }

bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

}  // namespace

bool validName(const std::string& name, std::string* trimmed, std::string* why) {
    size_t a = 0, b = name.size();
    while (a < b && isSpace(static_cast<unsigned char>(name[a]))) ++a;
    while (b > a && isSpace(static_cast<unsigned char>(name[b - 1]))) --b;
    const std::string t = name.substr(a, b - a);
    // Count code points, checking the UTF-8 as it goes.
    size_t points = 0;
    for (size_t i = 0; i < t.size();) {
        const auto c = static_cast<unsigned char>(t[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (n == 0 || i + n > t.size()) {
            if (why) *why = "the name is not valid text";
            return false;
        }
        for (size_t k = 1; k < n; k++)
            if ((static_cast<unsigned char>(t[i + k]) >> 6) != 0x2) {
                if (why) *why = "the name is not valid text";
                return false;
            }
        if (n == 1 && (c < 0x20 || c == 0x7F)) {
            if (why) *why = "the name has a control character";
            return false;
        }
        i += n;
        ++points;
    }
    if (points > 40) {
        if (why) *why = "a name is at most 40 characters";
        return false;
    }
    if (trimmed) *trimmed = t;
    return true;
}

DevicesStore::DevicesStore(std::string path) : path_(std::move(path)) { load(); }

void DevicesStore::load() {
    if (path_.empty()) return;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return;   // none yet
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(text);
    if (!Json::parseFromStream(rb, ss, &v, nullptr) || !v.isObject()) {
        std::cerr << "[devices] " << path_ << " is unreadable: read as empty" << std::endl;
        return;
    }
    if (v["names"].isObject())
        for (const auto& k : v["names"].getMemberNames())
            if (v["names"][k].isString()) names_[k] = v["names"][k];
    if (v["forgotten"].isObject())
        for (const auto& k : v["forgotten"].getMemberNames())
            if (v["forgotten"][k].isIntegral()) forgotten_[k] = v["forgotten"][k];
}

void DevicesStore::save() const {
    if (path_.empty()) return;
    Json::Value v(Json::objectValue);
    v["names"]     = names_;
    v["forgotten"] = forgotten_;
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    const std::string text = Json::writeString(w, v);
    std::error_code ec;
    const fs::path dest(path_);
    if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = dest.string() + ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            std::cerr << "[devices] " << tmp << " could not be written" << std::endl;
            return;
        }
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        std::cerr << "[devices] " << path_ << " could not be replaced: " << ec.message() << std::endl;
        fs::remove(tmp, ec);
    }
}

std::string DevicesStore::name(const std::string& kind, const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    return names_.get(key(kind, id), "").asString();
}

bool DevicesStore::setName(const std::string& kind, const std::string& id, const std::string& name,
                           std::string* why) {
    std::string t;
    if (!validName(name, &t, why)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (t.empty()) names_.removeMember(key(kind, id));
    else names_[key(kind, id)] = t;
    save();
    return true;
}

void DevicesStore::forget(const std::string& kind, const std::string& id, std::time_t at) {
    std::lock_guard<std::mutex> lk(mu_);
    forgotten_[key(kind, id)] = static_cast<Json::Int64>(at);
    names_.removeMember(key(kind, id));
    save();
}

std::optional<std::time_t> DevicesStore::forgottenAt(const std::string& kind, const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    const Json::Value& v = forgotten_[key(kind, id)];
    if (!v.isIntegral()) return std::nullopt;
    return static_cast<std::time_t>(v.asInt64());
}

void DevicesStore::unforget(const std::string& kind, const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!forgotten_.isMember(key(kind, id))) return;
    forgotten_.removeMember(key(kind, id));
    save();
}

}  // namespace hms_cpap::devices
