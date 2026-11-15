#include "ConfigModel.h"

#include <algorithm>
#include <sstream>

namespace cpapdash::supervisor {

namespace {

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '.')) if (!part.empty()) parts.push_back(part);
    return parts;
}

const nlohmann::json* find(const nlohmann::json& doc, const std::string& path) {
    const nlohmann::json* node = &doc;
    for (const auto& key : splitPath(path)) {
        if (!node->is_object() || !node->contains(key)) return nullptr;
        node = &(*node)[key];
    }
    return node;
}

void assign(nlohmann::json& doc, const std::string& path, const nlohmann::json& value) {
    const auto parts = splitPath(path);
    if (parts.empty()) return;
    nlohmann::json* node = &doc;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!(*node)[parts[i]].is_object()) (*node)[parts[i]] = nlohmann::json::object();
        node = &(*node)[parts[i]];
    }
    (*node)[parts.back()] = value;
}

}  // namespace

const std::vector<std::string>& secretPaths() {
    // Exactly the fields CpapController redacts in toJson(). Kept in one place
    // so the model, the form and the tests cannot disagree about which values
    // arrive masked.
    static const std::vector<std::string> paths = {
        "database.password",
        "mqtt.password",
        "llm.api_key",
        "sleephq.client_secret",
        "cpapdash.token",
    };
    return paths;
}

void ConfigModel::reset(const nlohmann::json& doc, bool from_api) {
    current_  = doc;
    pristine_ = doc;
    loaded_from_api_ = from_api;
    secrets_.clear();

    for (const auto& path : secretPaths()) {
        const auto* v = find(doc, path);
        const std::string value = (v && v->is_string()) ? v->get<std::string>() : "";

        if (from_api && value == kRedacted) {
            // A secret exists and we were shown a mask of it.
            secrets_[path] = SecretMode::Kept;
        } else if (value.empty()) {
            secrets_[path] = SecretMode::Unset;
        } else {
            // A real value. From disk that is the actual secret; from the API
            // it cannot happen, because the API always masks a non-empty one.
            secrets_[path] = from_api ? SecretMode::Kept : SecretMode::Changed;
        }
    }
}

void ConfigModel::loadFromApi(const nlohmann::json& redacted)  { reset(redacted, true);  }
void ConfigModel::loadFromDisk(const nlohmann::json& raw)      { reset(raw, false); }

nlohmann::json ConfigModel::get(const std::string& path) const {
    const auto* v = find(current_, path);
    return v ? *v : nlohmann::json();
}

std::string ConfigModel::getString(const std::string& path) const {
    const auto v = get(path);
    if (v.is_string())          return v.get<std::string>();
    if (v.is_number_integer())  return std::to_string(v.get<long long>());
    return "";
}

int ConfigModel::getInt(const std::string& path, int fallback) const {
    const auto v = get(path);
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_string()) {
        try { return std::stoi(v.get<std::string>()); } catch (...) { return fallback; }
    }
    return fallback;
}

bool ConfigModel::getBool(const std::string& path, bool fallback) const {
    const auto v = get(path);
    return v.is_boolean() ? v.get<bool>() : fallback;
}

bool ConfigModel::isSecret(const std::string& path) const {
    const auto& p = secretPaths();
    return std::find(p.begin(), p.end(), path) != p.end();
}

SecretMode ConfigModel::secretMode(const std::string& path) const {
    const auto it = secrets_.find(path);
    return it == secrets_.end() ? SecretMode::Unset : it->second;
}

void ConfigModel::set(const std::string& path, const nlohmann::json& value) {
    assign(current_, path, value);
    // Any deliberate write to a secret means the user supplied it -- including
    // an empty string, which is how "clear this" is expressed. That is why
    // erasure has to be an explicit action in the UI and never a side effect of
    // an empty box.
    if (isSecret(path)) secrets_[path] = SecretMode::Changed;
}

void ConfigModel::keepSecret(const std::string& path) {
    if (!isSecret(path)) return;
    const auto* was = find(pristine_, path);
    assign(current_, path, was ? *was : nlohmann::json(""));
    secrets_[path] = loaded_from_api_ ? SecretMode::Kept : SecretMode::Unset;
}

bool ConfigModel::touched(const std::string& path) const {
    const auto* now = find(current_, path);
    const auto* was = find(pristine_, path);
    if (!now && !was) return false;
    if (!now || !was) return true;
    return *now != *was;
}

std::vector<std::string> ConfigModel::changedPaths() const {
    std::vector<std::string> out;

    // Walk the current document rather than a fixed list, so a key added to
    // the config later still shows up as changed without anyone remembering to
    // register it here.
    std::function<void(const nlohmann::json&, const std::string&)> walk =
        [&](const nlohmann::json& node, const std::string& prefix) {
            if (!node.is_object()) {
                if (!prefix.empty() && touched(prefix)) out.push_back(prefix);
                return;
            }
            for (auto it = node.begin(); it != node.end(); ++it) {
                const std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
                walk(it.value(), path);
            }
        };
    walk(current_, "");
    return out;
}

nlohmann::json ConfigModel::toPatch() const {
    nlohmann::json patch = nlohmann::json::object();

    for (const auto& path : changedPaths()) {
        // A secret we were only ever shown a mask of is not ours to send.
        if (isSecret(path) && secretMode(path) != SecretMode::Changed) continue;
        assign(patch, path, get(path));
    }
    return patch;
}

nlohmann::json ConfigModel::toDiskJson(bool* ok) const {
    if (ok) *ok = true;

    nlohmann::json out = current_;

    for (const auto& path : secretPaths()) {
        if (secretMode(path) != SecretMode::Kept) continue;

        // We hold a mask, not a secret, and AppConfig::save() writes verbatim.
        // Writing this would put the literal "********" into config.json AS the
        // password -- no error, no crash, and the user finds out when the
        // service stops connecting. Refuse instead.
        if (ok) *ok = false;
        assign(out, path, nlohmann::json(""));
    }
    return out;
}

}  // namespace cpapdash::supervisor
