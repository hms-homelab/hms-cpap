#include "PreflightReport.h"

#include <algorithm>
#include <sstream>

namespace cpapdash::supervisor {

namespace {

constexpr const char* kHeader        = "Preflight checks:";
constexpr const char* kRemedyMarker  = "-> ";
constexpr std::size_t kStatusWidth   = 8;   ///< "  ok    ", "  FAIL  ", "  warn  "

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// A remedy continuation: eight spaces, then "-> ".
///
/// Matched on the marker rather than on the exact indent, because a status line
/// can never contain "-> " in that position and an indent-only rule would break
/// the moment someone reformats the column.
bool isRemedyLine(const std::string& line) {
    const auto pos = line.find(kRemedyMarker);
    if (pos == std::string::npos) return false;
    return trim(line.substr(0, pos)).empty();
}

/// Parse "  ok    name: detail" into its three parts.
///
/// Returns false for anything that is not a check line, which includes the
/// header, the verdict line and any stray output the service printed.
bool parseCheckLine(const std::string& line, PreflightCheck& out) {
    if (line.size() <= kStatusWidth) return false;

    const std::string status = trim(line.substr(0, kStatusWidth));
    if      (status == "ok")   out.status = CheckStatus::Ok;
    else if (status == "FAIL") out.status = CheckStatus::Fail;
    else if (status == "warn") out.status = CheckStatus::Warn;
    else return false;

    const std::string rest = line.substr(kStatusWidth);

    // Only the FIRST colon separates the name from the detail. archive_dir's
    // detail contains one of its own ("...written to disk: OSCAR has nothing
    // to import..."), and splitting on the last would swallow the sentence.
    const auto colon = rest.find(':');
    if (colon == std::string::npos) return false;

    out.name   = trim(rest.substr(0, colon));
    out.detail = trim(rest.substr(colon + 1));
    return !out.name.empty();
}

}  // namespace

std::vector<PreflightCheck> PreflightReport::failures() const {
    std::vector<PreflightCheck> v;
    for (const auto& c : checks)
        if (c.status == CheckStatus::Fail) v.push_back(c);
    return v;
}

std::vector<PreflightCheck> PreflightReport::warnings() const {
    std::vector<PreflightCheck> v;
    for (const auto& c : checks)
        if (c.status == CheckStatus::Warn) v.push_back(c);
    return v;
}

const PreflightCheck* PreflightReport::find(const std::string& name) const {
    const auto it = std::find_if(checks.begin(), checks.end(),
                                 [&](const PreflightCheck& c) { return c.name == name; });
    return it == checks.end() ? nullptr : &*it;
}

PreflightReport parsePreflight(const std::string& out,
                               const std::string& err,
                               int exit_code) {
    PreflightReport r;

    // Keep both streams. Supervisor.cs picks stdout and falls back to stderr,
    // which is right for display but loses whichever one it did not choose --
    // and the interesting failures are exactly the ones that write to both.
    r.raw = out;
    if (!trim(err).empty()) {
        if (!r.raw.empty() && r.raw.back() != '\n') r.raw += '\n';
        r.raw += err;
    }

    const bool has_header = out.find(kHeader) != std::string::npos;

    // The config file is not valid JSON. main.cpp bails before preflight ever
    // runs, so there are no checks to parse and never will be. This is its own
    // verdict because it is the one case where offering to rewrite the file is
    // the wrong thing to do: the user hand-edited it and still wants what is
    // in there.
    if (!has_header) {
        const std::string text = trim(err).empty() ? trim(out) : trim(err);
        if (text.find("not valid JSON") != std::string::npos) {
            r.verdict = Verdict::ConfigUnparseable;
            r.message = text;
            return r;
        }
        r.verdict = Verdict::Unreadable;
        r.message = text.empty()
            ? "The configuration check produced no output."
            : text;
        return r;
    }

    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();   // CRLF from Windows

        // A remedy belongs to the check above it. Preflight only emits one
        // after a failing check, so an orphan means the format moved and is
        // dropped rather than guessed at.
        if (isRemedyLine(line)) {
            if (!r.checks.empty()) {
                const auto pos = line.find(kRemedyMarker);
                r.checks.back().remedy = trim(line.substr(pos + std::string(kRemedyMarker).size()));
            }
            continue;
        }

        PreflightCheck c;
        if (parseCheckLine(line, c)) r.checks.push_back(c);
    }

    // The exit code is the verdict, not our count of FAILs. They agree today,
    // but the service owns the decision and a parser that recomputed it would
    // be free to disagree with the process that actually refuses to start.
    r.verdict = (exit_code == 0) ? Verdict::Ok : Verdict::Failed;

    if (r.checks.empty()) {
        r.verdict = Verdict::Unreadable;
        r.message = "The configuration check printed a header but no checks.";
    }
    return r;
}

}  // namespace cpapdash::supervisor
