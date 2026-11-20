#include "services/AiGrounding.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace hms_cpap {
namespace grounding {

namespace {

bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

/// Length of a list marker at [pos] ("1. ", "12) "), or 0.
size_t listMarkerLength(const std::string& text, size_t pos) {
    size_t i = pos;
    while (i < text.size() && isDigit(text[i])) ++i;
    if (i == pos || i + 1 >= text.size()) return 0;
    if ((text[i] == '.' || text[i] == ')') && (text[i + 1] == ' ' || text[i + 1] == '\t'))
        return i + 2 - pos;
    return 0;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/// The leading part of a line that is layout, not prose: indentation and a
/// bullet ("* ", "- ", "• ") or a list marker ("1. ").
size_t linePrefixLength(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i + 1 < line.size() && (line[i] == '*' || line[i] == '-') && line[i + 1] == ' ')
        return i + 2;
    if (line.compare(i, 4, "\xE2\x80\xA2 ") == 0) return i + 4;   // "• "
    if (const size_t m = listMarkerLength(line, i)) return i + m;
    return i;
}

/// [begin, end) of each sentence of [body]: a sentence ends at '.', '!' or
/// '?' followed by whitespace or the end. A decimal point is followed by a
/// digit, so "6.25" never ends one.
std::vector<std::pair<size_t, size_t>> sentenceSpans(const std::string& body) {
    std::vector<std::pair<size_t, size_t>> spans;
    size_t start = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if ((c == '.' || c == '!' || c == '?') &&
            (i + 1 == body.size() || std::isspace(static_cast<unsigned char>(body[i + 1])))) {
            spans.emplace_back(start, i + 1);
            start = i + 1;
        }
    }
    if (start < body.size()) spans.emplace_back(start, body.size());
    return spans;
}

std::string firstLine(const std::string& s) {
    const std::string t = trim(s);
    const auto nl = t.find('\n');
    return trim(nl == std::string::npos ? t : t.substr(0, nl));
}

}  // namespace

std::vector<Number> numbersIn(const std::string& text) {
    std::vector<Number> out;
    size_t i = 0;
    bool line_start = true;
    while (i < text.size()) {
        if (line_start) {
            size_t j = i;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
            if (const size_t m = listMarkerLength(text, j)) {
                i = j + m;
                line_start = false;
                continue;
            }
            line_start = false;
        }
        const char c = text[i];
        if (c == '\n') {
            line_start = true;
            ++i;
            continue;
        }
        // A digit run that is not the tail of a word ("SpO2", "O2", "H2O"):
        // those are names, not figures.
        if (isDigit(c) && (i == 0 || !std::isalpha(static_cast<unsigned char>(text[i - 1])))) {
            size_t j = i;
            while (j < text.size() && isDigit(text[j])) ++j;
            int decimals = 0;
            if (j + 1 < text.size() && text[j] == '.' && isDigit(text[j + 1])) {
                size_t k = j + 1;
                while (k < text.size() && isDigit(text[k])) ++k;
                decimals = static_cast<int>(k - j - 1);
                j = k;
            }
            Number n;
            n.text = text.substr(i, j - i);
            n.value = std::strtod(n.text.c_str(), nullptr);
            n.decimals = decimals;
            out.push_back(n);
            i = j;
            continue;
        }
        // Skip the rest of a word that carries digits (e.g. "H2O", "SpO2").
        if (std::isalpha(static_cast<unsigned char>(c))) {
            while (i < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_'))
                ++i;
            continue;
        }
        ++i;
    }
    return out;
}

bool isGrounded(const Number& n, const std::vector<Number>& given) {
    // What a given figure reads as at n's places: within half of n's last
    // place. A small epsilon keeps 6.25 -> 6.3 a pass in binary floating point.
    const double half = 0.5 * std::pow(10.0, -n.decimals) + 1e-9;
    for (const auto& g : given) {
        if (g.decimals < n.decimals) {
            // n says more places than the figure has: only the same value.
            if (std::fabs(g.value - n.value) < 1e-9) return true;
            continue;
        }
        if (std::fabs(g.value - n.value) <= half) return true;
    }
    return false;
}

std::vector<std::string> ungrounded(const std::string& sentence, const std::string& given) {
    const auto facts = numbersIn(given);
    std::vector<std::string> out;
    for (const auto& n : numbersIn(sentence))
        if (!isGrounded(n, facts)) out.push_back(n.text);
    return out;
}

std::string rewritePrompt(const std::string& given, const std::string& sentence) {
    return "The sentence below, from a summary of the data that follows, states a "
           "number that is not in that data. Rewrite the sentence so that every "
           "number in it is one of the figures given in the data, or leave the "
           "number out. Keep its meaning otherwise. Reply with the rewritten "
           "sentence only, on one line.\n\n"
           "Data:\n" + given + "\n\nSentence:\n" + sentence + "\n";
}

std::string unverifiedNote(int unverified) {
    return "Unverified: " + std::to_string(unverified) +
           (unverified == 1 ? " sentence in this summary states a number"
                            : " sentences in this summary state numbers") +
           " not found in the data it was given. Check against the figures before "
           "relying on it.";
}

Result check(const std::string& summary, const std::string& given, const Generate& regenerate) {
    Result r;
    const auto facts = numbersIn(given);
    auto fails = [&](const std::string& s) {
        for (const auto& n : numbersIn(s))
            if (!isGrounded(n, facts)) return true;
        return false;
    };

    std::string out;
    size_t pos = 0;
    while (pos <= summary.size()) {
        const size_t nl = summary.find('\n', pos);
        const std::string line =
            summary.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        const size_t prefix = linePrefixLength(line);
        out += line.substr(0, prefix);
        const std::string body = line.substr(prefix);
        for (const auto& [b, e] : sentenceSpans(body)) {
            const std::string piece = body.substr(b, e - b);
            if (numbersIn(piece).empty()) {
                out += piece;
                continue;
            }
            ++r.checked;
            if (!fails(piece)) {
                out += piece;
                continue;
            }
            // Regenerated once. The reply keeps the original's spacing and
            // layout; a bullet the model adds is its own, not the line's.
            std::optional<std::string> again;
            if (regenerate) again = regenerate(rewritePrompt(given, trim(piece)));
            std::string candidate = again ? firstLine(*again) : std::string();
            candidate = candidate.substr(linePrefixLength(candidate));
            if (!candidate.empty() && !fails(candidate)) {
                const size_t lead = piece.find_first_not_of(" \t");
                out += piece.substr(0, lead == std::string::npos ? 0 : lead) + candidate;
                ++r.regenerated;
            } else {
                out += piece;   // SDD-072 D3: kept, and the summary is flagged
                ++r.unverified;
            }
        }
        if (nl == std::string::npos) break;
        out += '\n';
        pos = nl + 1;
    }

    if (r.unverified > 0) {
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        out += "\n\n" + unverifiedNote(r.unverified);
    }
    r.text = out;
    return r;
}

}  // namespace grounding
}  // namespace hms_cpap
