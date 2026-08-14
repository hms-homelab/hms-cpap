#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cpapdash::supervisor {

/**
 * SDD-016: the settings document the supervisor edits.
 *
 * Pure: no Qt, no I/O. That is what lets the whole rule set be tested in
 * run_tests on every platform with no display and no Qt in the test build.
 *
 * ── The trap this class exists to prevent ──────────────────────────────────
 *
 * `GET /api/config` REDACTS five fields to the literal "********":
 * database.password, mqtt.password, llm.api_key, sleephq.client_secret and
 * cpapdash.token. The write path treats that literal as "keep what you have",
 * so:
 *
 *     PUT "********"  -> the stored secret is PRESERVED
 *     PUT ""          -> the stored secret is ERASED
 *
 * Which means a settings dialog that loads secrets into a line edit and sends
 * back whatever is in the box will wipe someone's database password the first
 * time they save without touching it. No error, no crash: they find out when
 * the service stops connecting.
 *
 * Worse, the DIRECT-write path has the opposite asymmetry. AppConfig::save()
 * writes secrets verbatim, so if a value loaded from the API ever reaches
 * config.json, the literal "********" is written as the real password.
 *
 * So secrets are a tri-state rather than a string, and the two loaders never
 * convert into each other:
 *
 *   Unset    the API sent ""            -> omit from the patch
 *   Kept     the API sent "********"    -> omit from the patch; MUST NOT be
 *                                          written to disk
 *   Changed  the user typed something   -> send it
 *
 * `Kept` reaching toDiskJson() is a bug that silently destroys credentials, so
 * it is checked rather than trusted.
 */

enum class SecretMode {
    Unset,     ///< no secret stored
    Kept,      ///< a secret is stored and we are not touching it
    Changed,   ///< the user supplied a new value (possibly empty = deliberate erase)
};

/// The sentinel the service uses for a redacted secret.
inline constexpr const char* kRedacted = "********";

/// Dotted paths of every field the API redacts. One list, referenced by the
/// model and by the tests, so the two cannot drift.
const std::vector<std::string>& secretPaths();

class ConfigModel {
public:
    ConfigModel() = default;

    /// Load from `GET /api/config`. Secrets arrive redacted, so this is the
    /// only loader that can produce `Kept`.
    void loadFromApi(const nlohmann::json& redacted);

    /// Load a config.json read straight off disk. Secrets are verbatim here, so
    /// this NEVER produces `Kept` -- everything is either Unset or a real value.
    void loadFromDisk(const nlohmann::json& raw);

    /// Value at a dotted path ("database.port"), or a null json if absent.
    nlohmann::json get(const std::string& path) const;
    std::string    getString(const std::string& path) const;
    int            getInt(const std::string& path, int fallback = 0) const;
    bool           getBool(const std::string& path, bool fallback = false) const;

    /// Set a value and mark the field touched. Setting a secret path moves it
    /// to `Changed`.
    void set(const std::string& path, const nlohmann::json& value);

    /// Restore a secret to "leave it alone".
    void keepSecret(const std::string& path);

    SecretMode secretMode(const std::string& path) const;
    bool       isSecret(const std::string& path) const;

    /// True when the field differs from what was loaded.
    bool touched(const std::string& path) const;

    /// The body for `PUT /api/config`.
    ///
    /// Untouched keys are OMITTED rather than echoed back. The endpoint is a
    /// merge patch guarded by isMember(), so an omitted key is a guaranteed
    /// no-op -- one fewer thing to get right than resending a value we were
    /// only ever shown a mask of.
    nlohmann::json toPatch() const;

    /// The full document for a direct write to config.json, used only when the
    /// service is down and cannot be started.
    ///
    /// @param ok  set false when a `Kept` secret would have been written, which
    ///            means the caller loaded from the API and must not write.
    nlohmann::json toDiskJson(bool* ok = nullptr) const;

    /// Paths whose value differs from the loaded document.
    std::vector<std::string> changedPaths() const;

    const nlohmann::json& current()  const { return current_; }
    const nlohmann::json& pristine() const { return pristine_; }

private:
    void reset(const nlohmann::json& doc, bool from_api);

    nlohmann::json current_;
    nlohmann::json pristine_;
    std::map<std::string, SecretMode> secrets_;
    bool loaded_from_api_ = false;
};

}  // namespace cpapdash::supervisor
