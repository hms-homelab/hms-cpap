#include "services/MyAirService.h"

#include "database/SqlDialect.h"
#include "utils/AppConfig.h"

#include <iostream>
#include <sstream>
#include <utility>

namespace hms_cpap {

namespace {

/// A night ResMed returned entirely empty means THEY HAVE NO DATA for that date,
/// which is not the same as a night with no therapy. Anything that averages
/// these has to tell the two apart, so the distinction is stored rather than
/// left to be re-derived by every reader.
bool nightHasData(const MyAirSleepRecord& r) {
    return r.total_usage_min > 0 || r.sleep_score > 0 || r.ahi > 0 ||
           r.mask_pair_count > 0 || r.leak_percentile > 0;
}

std::string num(double v) {
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

}  // namespace

MyAirService::MyAirService(std::shared_ptr<IDatabase> db, AppConfig& config,
                           std::string config_path)
    : db_(std::move(db)), config_(config), config_path_(std::move(config_path)) {
    // The default fetch is the real thing. A test replaces it before use.
    fetch_ = [this](std::vector<MyAirSleepRecord>& out, std::string& err) {
        MyAirClient client(config_.myair.username, config_.myair.password,
                           config_.myair.region);
        client.setDeviceToken(config_.myair.device_token);

        const auto state = client.connect(err);
        if (state == MyAirAuthState::MfaRequired) {
            err = "myAir emailed a verification code, which this service cannot answer. "
                  "Sign in once from the settings page to remember this device.";
            return false;
        }
        if (state != MyAirAuthState::Ok) return false;

        // Keep the remembered-device cookie, or a region with an email factor
        // asks for a code on every single start.
        if (!client.deviceToken().empty() &&
            client.deviceToken() != config_.myair.device_token) {
            config_.myair.device_token = client.deviceToken();
            if (!config_path_.empty()) config_.save(config_path_);
        }
        return client.fetchSleepRecords(out, err);
    };
}

bool MyAirService::enabled() const {
    return config_.myair.enabled && !config_.myair.username.empty() &&
           !config_.myair.password.empty();
}

void MyAirService::sweep() {
    if (!enabled()) return;

    const int minutes = config_.myair.poll_minutes > 0 ? config_.myair.poll_minutes : 60;
    const std::time_t now = std::time(nullptr);
    if (last_sync_at_ != 0 && now - last_sync_at_ < static_cast<std::time_t>(minutes) * 60) {
        return;  // not yet
    }

    // The interval is advanced even on failure. myAir is down, or ResMed changed
    // something, and retrying every burst would hammer an undocumented API to no
    // purpose; an hour later is soon enough for data that changes once a day.
    last_sync_at_ = now;

    std::string err;
    const int stored = syncNow(err);
    if (stored < 0) {
        std::cerr << "myAir: " << err << std::endl;
    } else if (stored > 0) {
        std::cout << "myAir: stored " << stored << " night(s)" << std::endl;
    }
}

int MyAirService::syncNow(std::string& err) {
    err.clear();
    if (!enabled()) { err = "myAir is not configured"; return -1; }
    if (!fetch_) { err = "myAir has no fetch configured"; return -1; }

    std::vector<MyAirSleepRecord> records;
    if (!fetch_(records, err)) {
        last_error_ = err;
        return -1;
    }
    const int stored = store(records, err);
    last_error_ = stored < 0 ? err : "";
    if (stored >= 0) last_sync_at_ = std::time(nullptr);
    return stored;
}

int MyAirService::store(const std::vector<MyAirSleepRecord>& records, std::string& err) {
    if (!db_) { err = "no database"; return -1; }
    if (records.empty()) return 0;

    const DbType dt = db_->dbType();

    // The window actually covered, taken from the records rather than assumed.
    // ResMed serves whole months, so the span is wider than the 30 days asked
    // for and hardcoding a range here would leave stale rows behind.
    std::string first = records.front().start_date;
    std::string last = records.front().start_date;
    for (const auto& r : records) {
        if (r.start_date < first) first = r.start_date;
        if (r.start_date > last) last = r.start_date;
    }

    db_->executeQuery("DELETE FROM cpap_myair_records WHERE record_date >= " +
                          sql::castDate(1, dt) + " AND record_date <= " + sql::castDate(2, dt),
                      {first, last});

    int stored = 0;
    for (const auto& r : records) {
        const std::string sql_text =
            "INSERT INTO cpap_myair_records (record_date, total_usage_min, sleep_score,"
            " usage_score, ahi_score, mask_score, leak_score, ahi, mask_pair_count,"
            " leak_percentile, has_data) VALUES (" +
            sql::castDate(1, dt) + ", " + sql::param(2, dt) + ", " + sql::param(3, dt) + ", " +
            sql::param(4, dt) + ", " + sql::param(5, dt) + ", " + sql::param(6, dt) + ", " +
            sql::param(7, dt) + ", " + sql::param(8, dt) + ", " + sql::param(9, dt) + ", " +
            sql::param(10, dt) + ", " + sql::param(11, dt) + ")";

        db_->executeQuery(sql_text, {
            r.start_date,
            num(r.total_usage_min),
            std::to_string(r.sleep_score),
            std::to_string(r.usage_score),
            std::to_string(r.ahi_score),
            std::to_string(r.mask_score),
            std::to_string(r.leak_score),
            num(r.ahi),
            std::to_string(r.mask_pair_count),
            num(r.leak_percentile),
            nightHasData(r) ? "1" : "0",
        });
        ++stored;
    }
    return stored;
}

}  // namespace hms_cpap
