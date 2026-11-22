#pragma once
//
// SDD-082: what POST /api/reports/generate may ask for beyond its dates, and
// the rules it is held to. Header-only and free of the PDF stack, so the
// route's answers are unit tested on every platform.
//
#include "services/reports/figures/ComplianceFigures.h"

#include <json/json.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace hms_cpap {

struct ReportRequest {
    std::string kind = "standard";   // standard | clinician | compliance
    double rule_hours = 4.0;         // compliance only
    int rule_share = compliance::kDefaultShare;
    std::string language = "en";     // the PDF's language (B6)
};

inline bool validReportKind(const std::string& kind) {
    return kind == "standard" || kind == "clinician" || kind == "compliance";
}

namespace report_names {
inline constexpr const char* kClinician  = "cpap_clinician_report_";
inline constexpr const char* kCompliance = "cpap_compliance_report_";
inline constexpr const char* kStandard   = "cpap_report_";
}  // namespace report_names

/// The kind a stored filename was made for (its prefix, as CpapDash reads it).
inline std::string reportKindOfFilename(const std::string& filename) {
    if (filename.rfind(report_names::kClinician, 0) == 0) return "clinician";
    if (filename.rfind(report_names::kCompliance, 0) == 0) return "compliance";
    return "standard";
}

/// The download name a report of `kind` over [start, end] gets. CpapDash's
/// names for the two new kinds; the standard report keeps the one it had.
inline std::string reportFilename(const std::string& kind, const std::string& start,
                                  const std::string& end) {
    const auto prefixed = [&](const char* prefix) {
        if (start == end) return std::string(prefix) + start + ".pdf";
        return std::string(prefix) + start + "_to_" + end + ".pdf";
    };
    if (kind == "clinician") return prefixed(report_names::kClinician);
    if (kind == "compliance") return prefixed(report_names::kCompliance);
    return std::string(report_names::kStandard) + start + "_to_" + end + ".pdf";
}

/// The rule as the report row stores it: {"hours":4,"share":70}.
inline std::string ruleJsonOf(double hours, int share) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{\"hours\":%g,\"share\":%d}", hours, share);
    return buf;
}

/// Read a request body into `out`; nullopt when it may run, else the 400's text.
/// `default_hours` / `default_share` are the rule a request without one is
/// judged against: the remembered rule (D-082-2), else the usage target from
/// Settings at 70 % (B5). Dates are checked by the caller, as they always were.
inline std::optional<std::string> parseReportRequest(const Json::Value& body, double default_hours,
                                                     ReportRequest& out,
                                                     int default_share = compliance::kDefaultShare) {
    out = ReportRequest();
    out.rule_hours = default_hours;
    out.rule_share = default_share;
    if (body.isMember("kind") && !body["kind"].isNull()) {
        if (!body["kind"].isString() || !validReportKind(body["kind"].asString()))
            return std::string("'kind' must be standard, clinician or compliance");
        out.kind = body["kind"].asString();
    }
    if (body.isMember("language") && body["language"].isString()) {
        const std::string l = body["language"].asString();
        if (l == "en" || l == "es" || l == "pt" || l == "hu" || l == "fr") out.language = l;
    }
    if (out.kind != "compliance") return std::nullopt;
    const Json::Value rule = body.get("rule", Json::Value());
    if (!rule.isNull() && !rule.isObject())
        return std::string("'rule' must be an object {hours, share}");
    if (rule.isObject()) {
        if (rule.isMember("hours") && !rule["hours"].isNull()) {
            if (!rule["hours"].isNumeric()) return std::string("'rule.hours' must be a number");
            out.rule_hours = rule["hours"].asDouble();
        }
        if (rule.isMember("share") && !rule["share"].isNull()) {
            if (!rule["share"].isNumeric()) return std::string("'rule.share' must be a number");
            const double s = rule["share"].asDouble();
            if (s != std::floor(s)) return std::string("'rule.share' must be a whole percent");
            out.rule_share = static_cast<int>(s);
        }
    }
    if (!compliance::validRule(out.rule_hours, out.rule_share))
        return std::string("Hours must be 0.5 to 24 and the percent 1 to 100.");
    return std::nullopt;
}

}  // namespace hms_cpap
