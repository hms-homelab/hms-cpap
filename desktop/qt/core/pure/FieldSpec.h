#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ConfigModel.h"

namespace cpapdash::supervisor {

/**
 * SDD-016: every setting, declared once.
 *
 * The web settings page renders about 70 bound fields across 17 groups. Hand
 * writing that in Qt would be roughly 3,000 lines in which each field is an
 * independent opportunity to forget the error label, the disabled state or the
 * restart flag -- and it would drift from the web page the first time either
 * side gained a key.
 *
 * So the fields are DATA. One table drives the widgets, the validation, the
 * error labels, the restart-required notice and the JSON marshalling, and the
 * whole rule set is a pure function over a json document, which means it is
 * testable with no display, no offscreen plugin and no Qt in the test build.
 *
 * The validation here is deliberately STRONGER than the web page's, which has
 * min/max attributes that nothing enforces and a Save button gated only on
 * "already saving". Out-of-range values reach the server today, which silently
 * clamps four of them and accepts the rest. Here, an invalid form does not
 * submit.
 */

enum class FieldKind {
    Text,
    Secret,      ///< tri-state; see ConfigModel
    Int,
    Double,
    Bool,
    Choice,
    Directory,   ///< picked with a button, never typed
    File,
};

enum class Severity {
    Error,     ///< blocks Save
    Warning,   ///< shown, does not block
};

struct Issue {
    std::string path;
    std::string message;
    std::string hint;      ///< the remedy, shown under the message
    Severity    severity = Severity::Error;
};

struct Choice {
    std::string value;
    std::string label;
};

/// A rule over the WHOLE document, not one value.
///
/// The interesting rules are cross-field -- the agent needs PostgreSQL, an
/// ezShare source needs an archive directory -- so a per-value signature would
/// not express them and half the rules would end up somewhere else.
using Rule = std::function<std::optional<Issue>(const ConfigModel&)>;

struct FieldSpec {
    std::string path;        ///< dotted, matches the config JSON exactly
    std::string group;       ///< the section it appears under
    std::string label;
    std::string help;        ///< shown under the widget, may be empty
    FieldKind   kind = FieldKind::Text;

    std::string placeholder;
    std::vector<Choice> choices;     ///< Choice only

    std::optional<double> min;       ///< Int/Double only
    std::optional<double> max;

    /// Restart-required, mirroring RESTART_KEYS in settings.component.ts. The
    /// server does not signal this -- PUT /api/config returns nothing about
    /// restarts -- so a client that does not carry the list will claim settings
    /// took effect when they did not.
    bool restart_required = false;

    /// Shown only when this returns true. Used for the source-specific fields
    /// and for everything behind an "enabled" toggle.
    std::function<bool(const ConfigModel&)> visible;
};

struct Group {
    std::string key;
    std::string title;
    std::string description;
    /// The whole group is gated on this path being true, e.g. "mqtt.enabled".
    std::string enabled_by;
};

/// The groups, in the order the dialog shows them.
const std::vector<Group>& settingsGroups();

/// Every field.
const std::vector<FieldSpec>& settingsFields();

/// The cross-field rules.
const std::vector<Rule>& settingsRules();

/// Run the constraints from the table plus every cross-field rule.
std::vector<Issue> validate(const ConfigModel& model);

/// Errors only -- what blocks a Save.
std::vector<Issue> blockingIssues(const ConfigModel& model);

/// Human labels for the restart-required fields that changed, for the
/// "saved, but not yet in effect" notice.
std::vector<std::string> restartReasons(const ConfigModel& model);

}  // namespace cpapdash::supervisor
