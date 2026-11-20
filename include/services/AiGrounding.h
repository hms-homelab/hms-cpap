#pragma once
//
// SDD-072 D3: a grounding check after generation.
//
// Every number in the model's text must appear in what it was given (the
// prompt with its facts), allowing the rounding a figure takes when it is
// said with fewer places: 6.25 may come back as 6.3 or 6, never as 7. A
// sentence that fails is regenerated once; one that fails again is KEPT and
// the summary is flagged as unverified (SDD-072 D3), not dropped.
//
// Provider-independent: it reads text, so it works the same for Ollama,
// Gemini, OpenAI and the small local models self-hosters run.
//
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace hms_cpap {
namespace grounding {

/// A number as written: its value and how many decimal places it shows.
struct Number {
    double value = 0;
    int decimals = 0;
    std::string text;
};

/// The numbers in [text]. A list marker at the start of a line ("1. ",
/// "2) ") is not a number; a date or a range ("2026-11-10", "1-2") is its
/// parts. Signs are ignored.
std::vector<Number> numbersIn(const std::string& text);

/// True when [n] is one of [given], or what one of them reads as at [n]'s
/// places.
bool isGrounded(const Number& n, const std::vector<Number>& given);

/// The numbers of [sentence] that are not in [given], as written.
std::vector<std::string> ungrounded(const std::string& sentence, const std::string& given);

/// What the check did to a summary.
struct Result {
    std::string text;      ///< the summary, with any rewritten sentences
    int checked = 0;       ///< sentences carrying at least one number
    int regenerated = 0;   ///< failing sentences rewritten into ones that pass
    int unverified = 0;    ///< failing sentences kept after the second try
};

/// One model call: the prompt in, the reply (or none) out.
using Generate = std::function<std::optional<std::string>(const std::string& prompt)>;

/// The prompt that asks for [sentence] again, from [given] only.
std::string rewritePrompt(const std::string& given, const std::string& sentence);

/// The line appended to a summary that kept [unverified] failing sentences.
std::string unverifiedNote(int unverified);

/// Check [summary] against [given]; each failing sentence is regenerated once
/// through [regenerate]. With unverified > 0 the text ends in unverifiedNote().
Result check(const std::string& summary, const std::string& given, const Generate& regenerate);

}  // namespace grounding
}  // namespace hms_cpap
