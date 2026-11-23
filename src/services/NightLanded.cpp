#include "services/NightLanded.h"

#include "database/SqlDialect.h"
#include "services/NightFacts.h"
#include "services/SyncFolderState.h"
#include "utils/MachineZone.h"

#include <cmath>
#include <ctime>
#include <iostream>
#include <map>
#include <mutex>

namespace hms_cpap::night_landed {

namespace {

std::mutex& mu() {
    static std::mutex m;
    return m;
}

std::vector<Listener>& listeners() {
    static std::vector<Listener> v;
    return v;
}

// YYYY-MM-DD or YYYYMMDD -> YYYYMMDD; "" when it is neither.
std::string folderOf(const std::string& day) {
    std::string out;
    for (char c : day) {
        if (c == '-') continue;
        if (c < '0' || c > '9') return "";
        out += c;
    }
    return out.size() == 8 ? out : "";
}

}  // namespace

void listen(Listener f) {
    if (!f) return;
    std::lock_guard<std::mutex> lk(mu());
    listeners().push_back(std::move(f));
}

void clear() {
    std::lock_guard<std::mutex> lk(mu());
    listeners().clear();
}

void announce(const std::string& date_folder, const std::optional<STRDailyRecord>& str) {
    const auto folder = folderOf(date_folder);
    if (folder.empty()) return;
    std::vector<Listener> to;
    {
        std::lock_guard<std::mutex> lk(mu());
        to = listeners();
    }
    for (const auto& f : to) {
        try {
            f(folder, str);
        } catch (const std::exception& e) {
            std::cerr << "Night " << folder << ": a listener failed: " << e.what() << std::endl;
        }
    }
}

void announceDays(const std::set<std::string>& days) {
    std::set<std::string> folders;
    for (const auto& d : days) {
        const auto f = folderOf(d);
        if (!f.empty()) folders.insert(f);
    }
    for (const auto& f : folders) announce(f);
}

void announceStarts(const std::vector<std::chrono::system_clock::time_point>& starts) {
    std::set<std::string> folders;
    for (const auto& s : starts) folders.insert(strDayForSessionStart(s));
    for (const auto& f : folders) announce(f);
}

bool saveStrDaysAndAnnounce(IDatabase& db, const std::vector<STRDailyRecord>& records) {
    if (records.empty()) return db.saveSTRDailyRecords(records);
    const std::string device = records.front().device_id;
    const auto dayOf = [](const STRDailyRecord& r) {
        const std::tm tm = wall::fieldsOf(wall::seconds(r.record_date));   // SDD-074 D4: a label
        char buf[11];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        return std::string(buf);
    };
    std::string first, last;
    for (const auto& r : records) {
        const auto d = dayOf(r);
        if (first.empty() || d < first) first = d;
        if (last.empty() || d > last) last = d;
    }
    const auto t = db.dbType();
    const auto snapshot = [&]() {
        std::map<std::string, long long> out;   // day -> STR minutes x 100
        for (const auto& row : db.executeQuery(
                 "SELECT record_date, duration_minutes_str FROM cpap_daily_summary WHERE device_id = " +
                     sql::param(1, t) + " AND record_date >= " + sql::castDate(2, t) +
                     " AND record_date <= " + sql::castDate(3, t),
                 {device, first, last})) {
            const auto v = row.get("duration_minutes_str", Json::nullValue);
            double d = -1;
            if (v.isNumeric()) d = v.asDouble();
            else if (v.isString() && !v.asString().empty()) {
                try { d = std::stod(v.asString()); } catch (...) {}
            }
            out[row.get("record_date", "").asString().substr(0, 10)] = std::llround(d * 100.0);
        }
        return out;
    };
    const auto before = snapshot();
    if (!db.saveSTRDailyRecords(records)) return false;
    const auto after = snapshot();

    std::map<std::string, const STRDailyRecord*> byDay;
    for (const auto& r : records) byDay[dayOf(r)] = &r;
    for (const auto& [day, minutes] : after) {
        if (minutes <= 0) continue;
        const auto was = before.find(day);
        if (was != before.end() && was->second == minutes) continue;
        if (night_facts::hasSessions(db, device, day)) continue;   // an ordinary night
        const auto it = byDay.find(day);
        announce(day, it == byDay.end() ? std::nullopt : std::optional<STRDailyRecord>(*it->second));
    }
    return true;
}

}  // namespace hms_cpap::night_landed
