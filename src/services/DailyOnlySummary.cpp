#include "services/DailyOnlySummary.h"

#include "database/SqlDialect.h"

#include <iostream>

namespace hms_cpap::daily_only {

const char* const kLlmSource = "llm";

namespace {

std::string isoDay(const std::string& day) {
    if (day.size() == 8) return day.substr(0, 4) + "-" + day.substr(4, 2) + "-" + day.substr(6, 2);
    return day;
}

std::string substitute(std::string tmpl, const std::string& metrics) {
    const std::string key = "{metrics}";
    const auto at = tmpl.find(key);
    if (at == std::string::npos) return tmpl + "\n\n" + metrics;
    tmpl.replace(at, key.size(), metrics);
    return tmpl;
}

struct Row {
    std::string id, facts;
};

std::vector<Row> llmRows(IDatabase& db, const std::string& device_id, const std::string& sleep_day) {
    const auto t = db.dbType();
    const auto rows = db.executeQuery(
        "SELECT id, facts FROM cpap_summaries WHERE device_id = " + sql::param(1, t) +
            " AND period = 'daily' AND source = '" + kLlmSource + "' AND range_start = " +
            sql::castDate(2, t) + " ORDER BY id DESC",
        {device_id, sleep_day});
    std::vector<Row> out;
    for (const auto& r : rows) {
        Row row;
        if (r["id"].isString()) row.id = r["id"].asString();
        else if (r["id"].isNumeric()) row.id = std::to_string(r["id"].asLargestInt());
        if (row.id.empty()) continue;
        row.facts = r["facts"].isString() ? r["facts"].asString() : "";
        out.push_back(row);
    }
    return out;
}

}  // namespace

std::string storedFacts(IDatabase& db, const std::string& device_id, const std::string& sleep_day) {
    const auto rows = llmRows(db, device_id, isoDay(sleep_day));
    return rows.empty() ? "" : rows.front().facts;
}

Result summarize(IDatabase& db, const std::string& device_id, const std::string& sleep_day_in,
                 const std::string& prompt_template, const grounding::Generate& generate) {
    Result r;
    const std::string sleep_day = isoDay(sleep_day_in);
    const auto night = night_facts::loadDailyOnlyNight(db, device_id, sleep_day);
    if (!night) {
        r.why = "not a summary-only night (it has sessions, or no daily record)";
        return r;
    }
    r.facts = "Night: " + sleep_day + "\n" + night_facts::renderNight(*night);
    const auto earlier = llmRows(db, device_id, sleep_day);
    // Only a summary-only row knows its facts here: a row from an ordinary
    // night's close has none, and is replaced like any other.
    if (!earlier.empty() && earlier.front().facts == r.facts) {
        r.skipped = true;
        r.why = "already summarized from these facts";
        return r;
    }
    if (!generate) {
        r.why = "no LLM configured";
        return r;
    }
    const std::string prompt = substitute(prompt_template, r.facts);
    const auto reply = generate(prompt);
    if (!reply || reply->empty()) {
        r.why = "the LLM gave no text";
        return r;
    }
    // SDD-072 D3: every number the reply states is one the prompt gave.
    const auto checked = grounding::check(*reply, prompt, generate);
    if (checked.regenerated > 0 || checked.unverified > 0)
        std::cout << "LLM: summary-only night " << sleep_day << " grounding: " << checked.checked
                  << " sentence(s) with numbers, " << checked.regenerated << " regenerated, "
                  << checked.unverified << " kept unverified" << std::endl;
    r.text = checked.text;

    const double hours = night->usage_hours.value_or(0.0);
    if (!db.saveSummary(device_id, "daily", sleep_day, sleep_day, 1, night->index, hours,
                        hours >= 4.0 ? 100.0 : 0.0, r.text, kLlmSource)) {
        r.why = "the summary could not be stored";
        return r;
    }
    // The new row is the one not there before: it gets its facts, and the
    // night's earlier LLM summaries go, so a night has one.
    const auto t = db.dbType();
    std::string not_earlier;
    std::vector<std::string> params = {r.facts, device_id, sleep_day};
    for (const auto& e : earlier) {
        params.push_back(e.id);
        not_earlier += " AND id <> " + sql::param(static_cast<int>(params.size()), t);
    }
    db.executeQuery("UPDATE cpap_summaries SET facts = " + sql::param(1, t) + " WHERE device_id = " +
                        sql::param(2, t) + " AND period = 'daily' AND source = '" + kLlmSource +
                        "' AND range_start = " + sql::castDate(3, t) + not_earlier,
                    params);
    for (const auto& e : earlier)
        db.executeQuery("DELETE FROM cpap_summaries WHERE id = " + sql::param(1, t), {e.id});
    r.written = true;
    std::cout << "LLM: summary-only night " << sleep_day << " (" << night->daily_only
              << ") summarized" << std::endl;
    return r;
}

// ---- The queue -----------------------------------------------------------------

void Queue::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void Queue::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Queue::enqueue(const std::string& date_folder) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const std::string day = isoDay(date_folder);
        for (const auto& d : days_)
            if (d == day) return;   // already waiting
        days_.push_back(day);
    }
    cv_.notify_one();
}

void Queue::run() {
    for (;;) {
        std::string day;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !days_.empty(); });
            if (stopping_) return;
            day = days_.front();
            days_.pop_front();
        }
        try {
            work_(day);
        } catch (const std::exception& e) {
            std::cerr << "LLM: summary-only night " << day << ": " << e.what() << std::endl;
        }
    }
}

}  // namespace hms_cpap::daily_only
