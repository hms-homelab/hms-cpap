#include "services/AirSense11Pull.h"

#include "services/RemovedNights.h"

#include <cpapdash/parser/AirSense11Summary.h>

#include <algorithm>
#include <ctime>
#include <iostream>
#include <map>

namespace hms_cpap {

AirSense11Pull::AirSense11Pull(std::shared_ptr<IAirSense11Client> client,
                               std::shared_ptr<IDatabase> db,
                               std::string device_id,
                               std::chrono::seconds interval)
    : client_(std::move(client)),
      db_(std::move(db)),
      device_id_(std::move(device_id)),
      interval_(interval) {}

std::string AirSense11Pull::fromAfter(const std::vector<STRDailyRecord>& records) {
    if (records.empty()) return "";
    auto newest = records.front().record_date;
    for (const auto& r : records)
        if (r.record_date > newest) newest = r.record_date;
    const std::time_t t = std::chrono::system_clock::to_time_t(newest - std::chrono::hours(48));
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

namespace {

// The calendar date a record is filed under, as the database keys it.
std::string dateOf(const std::chrono::system_clock::time_point& tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

} // namespace

int AirSense11Pull::withStrSettings(std::vector<STRDailyRecord>& days, const std::vector<STRDailyRecord>& str) {
    std::map<std::string, const STRDailyRecord*> by_date;
    for (const auto& s : str) by_date[dateOf(s.record_date)] = &s;
    int given = 0;
    for (auto& d : days) {
        auto it = by_date.find(dateOf(d.record_date));
        if (it == by_date.end() || it->second->mode < 0) continue;   // no settings on that day
        const STRDailyRecord& s = *it->second;
        d.mode = s.mode;
        d.epr_level = s.epr_level;
        d.pressure_setting = s.pressure_setting;
        d.max_pressure = s.max_pressure;
        d.min_pressure = s.min_pressure;
        d.family = s.family;
        d.bl_max_ipap = s.bl_max_ipap;
        d.bl_min_epap = s.bl_min_epap;
        d.bl_ps = s.bl_ps;
        d.bl_ipap = s.bl_ipap;
        d.bl_epap = s.bl_epap;
        d.asv_start_press = s.asv_start_press;
        d.asv_epap = s.asv_epap;
        d.asv_max_ps = s.asv_max_ps;
        d.asv_min_ps = s.asv_min_ps;
        d.asvauto_min_epap = s.asvauto_min_epap;
        d.asvauto_max_epap = s.asvauto_max_epap;
        given++;
    }
    return given;
}

AirSense11Pull::Result AirSense11Pull::run(Clock::time_point now) {
    if (pulled_once_ && now - last_pull_ < interval_) return {};
    last_pull_   = now;
    pulled_once_ = true;
    return pullNow();
}

AirSense11Pull::Result AirSense11Pull::pullNow() {
    using cpapdash::parser::AirSense11SummaryParser;
    Result r;
    r.asked = true;
    if (!client_ || !db_) { r.error = "no client or database"; return r; }

    const auto bytes = client_->fetchSummary(from_);
    if (bytes.empty()) { r.error = "no summary from the bridge"; return r; }
    if (!AirSense11SummaryParser::looksLike(bytes.data(), bytes.size())) {
        r.error = "the bridge answered something that is not a summary";
        return r;
    }
    const auto days = AirSense11SummaryParser::parse(bytes.data(), bytes.size());
    r.days = static_cast<int>(days.size());
    // All the days, so a window that ends on days off still moves forward.
    std::vector<STRDailyRecord> all;
    for (const auto& d : days) all.push_back(AirSense11SummaryParser::toStrRecord(d, device_id_));
    if (!all.empty()) {
        auto newest = all.front().record_date;
        for (const auto& rec : all) newest = std::max(newest, rec.record_date);
        const std::time_t t = std::chrono::system_clock::to_time_t(newest);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        r.newest_day = buf;
    }
    auto used = AirSense11SummaryParser::toStrRecords(days, device_id_);
    used = withoutRemovedNights(std::move(used), removedNightSet(*db_, device_id_));
    // The Summary carries no settings; the machine's STR.edf does from the day
    // they were read (written just now, from this Summary and those settings).
    if (str_path_ && !used.empty()) {
        const std::string path = str_path_();
        if (!path.empty()) {
            const auto str = EDFParser::parseSTRFile(path, device_id_);
            if (const int n = withStrSettings(used, str); n > 0)
                std::cout << "AirSense11: settings on " << n << " day(s), from the machine's STR" << std::endl;
        }
    }
    if (!used.empty()) {
        if (!db_->saveSTRDailyRecords(used)) {
            r.error = "saveSTRDailyRecords failed";
            return r;
        }
        r.written = static_cast<int>(used.size());
    }
    const auto next = fromAfter(all);
    if (!next.empty()) from_ = next;
    std::cout << "AirSense11: summary from " << from_ << ": " << r.days << " day(s), "
              << r.written << " with usage written" << std::endl;
    return r;
}

} // namespace hms_cpap
