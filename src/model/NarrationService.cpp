#include "model/NarrationService.h"

#include "services/AiGrounding.h"

#include <functional>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>

namespace hms_cpap::model {

const char* const kLocalSummarySource = "local";

std::string factBlock(const NightFacts& night) {
    return "Night: " + night.sleep_day + "\n" + night_facts::renderNight(night);
}

std::vector<std::string> violations(const std::string& text, const std::string& block) {
    auto out = grounding::ungrounded(text, block);
    // Dates first-class: a summary that names the wrong night is the failure
    // users notice first, and the number check alone sees only its parts.
    static const std::regex kIsoDay(R"(\b\d{4}-\d{2}-\d{2}\b)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), kIsoDay); it != std::sregex_iterator();
         ++it) {
        if (block.find(it->str()) == std::string::npos) out.push_back(it->str());
    }
    return out;
}

std::string templateSummary(const NightFacts& n) {
    std::vector<std::string> sentences;
    auto say = [&sentences](const std::function<void(std::ostringstream&)>& write) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(2);
        write(o);
        sentences.push_back(o.str());
    };
    say([&](auto& o) { o << "Night of " << n.sleep_day << "."; });
    if (n.usage_hours)
        say([&](auto& o) { o << "Usage was " << *n.usage_hours << " hours."; });
    else
        say([&](auto& o) { o << "Usage was not recorded."; });
    if (n.graded) {
        say([&](auto& o) {
            o << "AHI was " << n.index << " events per hour, from " << n.inIndex()
              << " apneas and hypopneas (obstructive apnea " << n.obstructive << ", central apnea "
              << n.central << ", clear airway apnea " << n.clear_airway << ", unclassified apnea "
              << n.unclassified << ", hypopnea " << n.hypopnea << ").";
        });
    } else {
        say([&](auto& o) {
            o << "This machine does not detect hypopneas, so no AHI is available; it recorded " << n.total
              << " events.";
        });
    }
    if (n.leak_p95)
        say([&](auto& o) { o << "Leak at the 95th percentile was " << *n.leak_p95 << " L/min."; });
    else if (n.leak_avg)
        say([&](auto& o) { o << "Average leak was " << *n.leak_avg << " L/min."; });
    if (n.pressure_p95)
        say([&](auto& o) { o << "Pressure at the 95th percentile was " << *n.pressure_p95 << " cmH2O."; });
    else if (n.therapy_pressure)
        say([&](auto& o) { o << "Average therapy pressure was " << *n.therapy_pressure << " cmH2O."; });
    if (n.oximetry)
        say([&](auto& o) {
            o << "Average SpO2 was " << n.oximetry->avg_spo2 << "%, the lowest " << n.oximetry->min_spo2 << "%.";
        });

    // The template is held to the same contract as the model: a sentence whose
    // figure the fact block does not carry is left out, never said.
    const auto block = factBlock(n);
    std::string out;
    for (const auto& s : sentences) {
        if (!violations(s, block).empty()) continue;
        if (!out.empty()) out += " ";
        out += s;
    }
    return out;
}

Narration narrate(const NightFacts& night, ILocalModel& model) {
    Narration r;
    r.night = night;
    if (!model.ready()) {
        r.why = model.whyNotReady();
        return r;
    }
    const auto block = factBlock(night);

    std::string why;
    const double temps[] = {kFirstTemperature, kRetryTemperature};
    for (int attempt = 0; attempt < 2; ++attempt) {
        GenerateParams p;
        p.temperature = temps[attempt];
        const auto text = model.generate(block, p);
        if (!text || text->empty()) {
            why = "the model gave no text";
            continue;
        }
        const auto bad = violations(*text, block);
        if (bad.empty()) {
            r.written = true;
            r.text = *text;
            r.how = attempt == 0 ? "model" : "model_retry";
            return r;
        }
        why = "the model stated figures not in the facts:";
        for (const auto& b : bad) why += " " + b;
    }
    r.written = true;
    r.text = templateSummary(night);
    r.how = "template";
    r.why = why;
    std::cout << "Local model: " << night.sleep_day << " written from the template, twice " << why
              << std::endl;
    return r;
}

Narration narrateAndStore(IDatabase& db, const std::string& device_id, const std::string& sleep_day,
                          ILocalModel& model) {
    Narration r;
    if (!model.ready()) {
        r.why = model.whyNotReady();
        return r;
    }
    const auto night = night_facts::loadNight(db, device_id, sleep_day);
    if (!night) {
        r.why = "no closed night " + sleep_day + " in the database";
        return r;
    }
    r = narrate(*night, model);
    if (!r.written) return r;

    const double hours = night->usage_hours.value_or(0.0);
    if (!db.saveSummary(device_id, "daily", sleep_day, sleep_day, 1, night->index, hours,
                        hours >= 4.0 ? 100.0 : 0.0, r.text, kLocalSummarySource)) {
        r.written = false;
        r.why = "the summary could not be stored";
    }
    return r;
}

}  // namespace hms_cpap::model
