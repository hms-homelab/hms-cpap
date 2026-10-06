#include "services/AirSense11Pull.h"

#include "services/RemovedNights.h"

#include <cpapdash/parser/AirSense11Summary.h>

#include <ctime>
#include <iostream>

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
    auto used = AirSense11SummaryParser::toStrRecords(days, device_id_);
    used = withoutRemovedNights(std::move(used), removedNightSet(*db_, device_id_));
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
