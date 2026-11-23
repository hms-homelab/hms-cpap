#include "model/NarrationService.h"

#include "database/SqlDialect.h"
#include "services/AiGrounding.h"

#include <cstdlib>
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
    const auto usage = usageWordingProblem(text, block);
    if (!usage.empty()) out.push_back(usage);
    return out;
}

std::string usageWordingProblem(const std::string& text, const std::string& block) {
    static const std::regex kUsage(R"(Usage: ([0-9]+(?:\.[0-9]+)?) hours \([0-9.]+% of ([0-9]+)h target\))");
    std::smatch m;
    if (!std::regex_search(block, m, kUsage)) return "";
    const double hours = std::atof(m[1].str().c_str());
    const int target = std::atoi(m[2].str().c_str());
    if (target <= 0) return "";
    // Measured on narrator-v1 (2026-11-21): the corpus only ever said 8h, and
    // with a 4h target the model still wrote "well short of your target" for
    // 4 to 8 h nights. A text that contradicts met / not met is not stored.
    static const std::regex kShort(
        R"(\b(short of|fell short|falls short|below (the |your |its )?(\d+ ?h(ours?)? )?target|)"
        R"(under (the |your )?(\d+ ?h(ours?)? )?target|missed (the |your )?target|did not (meet|reach)|)"
        R"(didn't (meet|reach)|not (meet|reach)(ing)? (the |your )?target)\b)",
        std::regex::icase);
    static const std::regex kMet(
        R"(\b((target|goal) (was |is )?(met|reached|achieved|exceeded)|)"
        R"((met|reached|achieved|exceeded|hit) (the |your )?(\d+ ?h(ours?)? )?(usage )?(target|goal))\b)",
        std::regex::icase);
    const bool met = hours >= target;
    std::smatch w;
    if (met && std::regex_search(text, w, kShort))
        return "usage: \"" + w.str() + "\" but " + m[1].str() + " hours meets the " + m[2].str() + "h target";
    if (!met && std::regex_search(text, w, kMet))
        return "usage: \"" + w.str() + "\" but " + m[1].str() + " hours is under the " + m[2].str() + "h target";
    return "";
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
    if (!n.daily_only.empty()) {
        // Issue #74: a summary-only night says so, and says only its daily figures.
        say([&](auto& o) {
            o << "Summary only: no detailed session data was recorded for this night, so these are "
              << (n.daily_only == "myair" ? "the daily figures from ResMed myAir."
                                          : "the daily figures from the machine's own record.");
        });
        if (n.usage_hours)
            say([&](auto& o) { o << "Usage was " << *n.usage_hours << " hours."; });
        else
            say([&](auto& o) { o << "Usage was not recorded."; });
        if (n.graded)
            say([&](auto& o) { o << "AHI was " << n.index << " events per hour."; });
        else
            say([&](auto& o) {
                o << "This machine does not detect hypopneas, so no AHI is available; its apnea index was "
                  << n.index << " per hour.";
            });
        if (n.leak_p95)
            say([&](auto& o) { o << "Leak at the 95th percentile was " << *n.leak_p95 << " L/min."; });
        if (n.pressure_p95)
            say([&](auto& o) { o << "Pressure at the 95th percentile was " << *n.pressure_p95 << " cmH2O."; });
        if (n.myair && n.myair->score)
            say([&](auto& o) { o << "The myAir score was " << *n.myair->score << " of 100."; });
        if (n.myair && n.myair->mask_pairs)
            say([&](auto& o) {
                if (*n.myair->mask_pairs == 1) o << "The mask was put on and taken off once.";
                else o << "The mask was put on and taken off " << *n.myair->mask_pairs << " times.";
            });
        const auto block = factBlock(n);
        std::string out;
        for (const auto& s : sentences) {
            if (!violations(s, block).empty()) continue;
            if (!out.empty()) out += " ";
            out += s;
        }
        return out;
    }
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

    // Issue #74: narrator-v1 was trained on full nights' fact blocks; a
    // summary-only night's block is not one, so its template writes it.
    if (!night.daily_only.empty()) {
        r.written = true;
        r.text = templateSummary(night);
        r.how = "template";
        r.why = "a summary-only night (" + night.daily_only + "): the model was trained on full nights";
        return r;
    }

    std::string why;
    GenerateParams p;
    p.temperature = kTemperature;
    const auto text = model.generate(block, p);
    if (!text || text->empty()) {
        why = "the model gave no text";
    } else {
        const auto bad = violations(*text, block);
        if (bad.empty()) {
            r.written = true;
            r.text = *text;
            r.how = "model";
            return r;
        }
        why = "the model's text failed the facts check:";
        for (const auto& b : bad) why += " " + b;
    }
    r.written = true;
    r.text = templateSummary(night);
    r.how = "template";
    r.why = why;
    std::cout << "Local model: " << night.sleep_day << " written from the template, " << why << std::endl;
    return r;
}

NightFacts modelNight(SessionMetrics m, const STRDailyRecord* str,
                      const std::optional<NightOximetryFacts>& oximetry) {
    if (!str && m.therapy_mode && *m.therapy_mode == 0) m.therapy_mode.reset();
    return NightFacts::from(m, str, oximetry);
}

Narration narrateAndStore(IDatabase& db, const std::string& device_id, const std::string& sleep_day,
                          ILocalModel& model, const STRDailyRecord* str) {
    Narration r;
    if (!model.ready()) {
        r.why = model.whyNotReady();
        return r;
    }
    const auto metrics = night_facts::nightMetrics(db, device_id, sleep_day);
    std::optional<NightFacts> loaded;
    if (metrics) {
        loaded = modelNight(*metrics, str, night_facts::oximetryForNight(db, sleep_day));
    } else {
        // Issue #74: no session night, but a daily record (the card's STR, or myAir).
        loaded = night_facts::loadDailyOnlyNight(db, device_id, sleep_day);
    }
    if (!loaded) {
        r.why = "no closed night " + sleep_day + " in the database";
        return r;
    }
    const NightFacts night = *loaded;
    const auto block = factBlock(night);

    // D-701-3: a night announced again (a re-read, a second upload of the
    // same card, every stored session read once per process) is summarized
    // again only when what the model would be given changed.
    const auto earlier = localSummaryRows(db, device_id, sleep_day);
    if (!earlier.empty() && earlier.front().facts == block) {
        r.night = night;
        r.skipped = true;
        r.why = "already summarized from these facts";
        return r;
    }

    r = narrate(night, model);
    if (!r.written) return r;

    const double hours = night.usage_hours.value_or(0.0);
    if (!db.saveSummary(device_id, "daily", sleep_day, sleep_day, 1, night.index, hours,
                        hours >= 4.0 ? 100.0 : 0.0, r.text, kLocalSummarySource)) {
        r.written = false;
        r.why = "the summary could not be stored";
        return r;
    }
    // The new row is the one not there before: it gets its fact block, and
    // the night's earlier local summaries go, so a night has one.
    const auto t = db.dbType();
    std::string not_earlier;
    std::vector<std::string> params = {block, device_id, sleep_day};
    for (const auto& e : earlier) {
        params.push_back(e.id);
        not_earlier += " AND id <> " + sql::param(static_cast<int>(params.size()), t);
    }
    db.executeQuery("UPDATE cpap_summaries SET facts = " + sql::param(1, t) + " WHERE device_id = " +
                        sql::param(2, t) + " AND period = 'daily' AND source = '" + kLocalSummarySource +
                        "' AND range_start = " + sql::castDate(3, t) + not_earlier,
                    params);
    for (const auto& e : earlier)
        db.executeQuery("DELETE FROM cpap_summaries WHERE id = " + sql::param(1, t), {e.id});
    return r;
}

std::vector<LocalSummaryRow> localSummaryRows(IDatabase& db, const std::string& device_id,
                                              const std::string& sleep_day) {
    const auto t = db.dbType();
    const auto rows = db.executeQuery(
        "SELECT id, facts FROM cpap_summaries WHERE device_id = " + sql::param(1, t) +
            " AND period = 'daily' AND source = '" + kLocalSummarySource + "' AND range_start = " +
            sql::castDate(2, t) + " ORDER BY id DESC",
        {device_id, sleep_day});
    std::vector<LocalSummaryRow> out;
    for (const auto& row : rows) {
        LocalSummaryRow s;
        const auto& id = row["id"];
        if (id.isString()) s.id = id.asString();
        else if (id.isNumeric()) s.id = std::to_string(id.asLargestInt());
        if (s.id.empty()) continue;
        s.facts = row["facts"].isString() ? row["facts"].asString() : "";
        out.push_back(std::move(s));
    }
    return out;
}

std::string storedFactBlock(IDatabase& db, const std::string& device_id, const std::string& sleep_day) {
    const auto rows = localSummaryRows(db, device_id, sleep_day);
    return rows.empty() ? "" : rows.front().facts;
}

}  // namespace hms_cpap::model
