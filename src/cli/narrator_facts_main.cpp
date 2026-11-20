// SDD-013: the runtime's fact block and grounding check, for the offline
// corpus pipeline (training/narrator/).
//
// The corpus must be built from the inputs the model sees at runtime and held
// to the check the runtime holds it to, so neither is reimplemented in Python:
// this tool runs the same code the nightly job runs. No database is opened.
//
//   hms_cpap_narrator_facts render   JSONL nights (model/NightJson.h) on stdin,
//                                    {"id", "facts"} per line on stdout, where
//                                    facts is model::factBlock(), the model's
//                                    whole prompt at runtime.
//   hms_cpap_narrator_facts check    {"id", "facts", "text"} per line on stdin,
//                                    {"id", "ok", "violations"} on stdout, from
//                                    model::violations() (SDD-072's number check
//                                    plus the ISO date check).
//
// A line that cannot be read is answered with {"id", "error"} and the run goes
// on, so one bad night does not stop a batch.

#include "model/NarrationService.h"
#include "model/NightJson.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

namespace {

using nlohmann::json;
using namespace hms_cpap;

json render(const json& in) {
    const auto night = model::nightFromJson(in);
    return {{"id", in.value("id", "")}, {"facts", model::factBlock(night.facts())}};
}

json check(const json& in) {
    const auto facts = in.at("facts").get<std::string>();
    const auto text = in.at("text").get<std::string>();
    const auto bad = model::violations(text, facts);
    return {{"id", in.value("id", "")}, {"ok", bad.empty()}, {"violations", bad}};
}

int usage() {
    std::cerr << "usage: hms_cpap_narrator_facts render|check < in.jsonl > out.jsonl\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return usage();
    const std::string mode = argv[1];
    if (mode != "render" && mode != "check") return usage();

    std::ios::sync_with_stdio(false);
    std::string line;
    long lines = 0, errors = 0;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        ++lines;
        json in;
        json out;
        try {
            in = json::parse(line);
            out = mode == "render" ? render(in) : check(in);
        } catch (const std::exception& e) {
            ++errors;
            out = {{"id", in.is_object() ? in.value("id", "") : ""}, {"error", e.what()}};
        }
        std::cout << out.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
    }
    std::cout.flush();
    std::cerr << "hms_cpap_narrator_facts " << mode << ": " << lines << " lines, " << errors
              << " errors\n";
    return 0;
}
