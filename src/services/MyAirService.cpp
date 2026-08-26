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
        client.setRefreshToken(config_.myair.refresh_token);

        // The stored token first, so the ordinary path never touches a password
        // and works whether or not one was ever kept.
        MyAirAuthState state = MyAirAuthState::Failed;
        if (!config_.myair.refresh_token.empty()) {
            state = client.connectWithRefreshToken(err);
            if (state != MyAirAuthState::Ok && config_.myair.password.empty()) {
                // Revoked, expired, or signed out everywhere, and nothing to
                // fall back on. The only cure is an interactive sign-in, so the
                // dead token is DISCARDED and the state is recorded rather than
                // retried hourly forever against a credential that will never
                // work again. Keeping it would also make the service look
                // connected when it is not.
                config_.myair.refresh_token.clear();
                needs_reauth_ = true;
                if (!config_path_.empty()) config_.save(config_path_);
                err = "myAir disconnected: the stored token is no longer valid. "
                      "Enter the password once to reconnect.";
                return false;
            }
        }

        if (state != MyAirAuthState::Ok) {
            if (config_.myair.password.empty()) {
                err = "myAir is not connected. Enter the password once to sign in.";
                return false;
            }
            state = client.connect(err);
            if (state == MyAirAuthState::MfaRequired) {
                err = "myAir emailed a verification code, which this service cannot "
                      "answer. Sign in once from the settings page to remember this device.";
                return false;
            }
            if (state != MyAirAuthState::Ok) return false;
        }

        // Persist what replaces the password, and then get rid of the password.
        //
        // This is the whole point of asking for offline_access: after one
        // successful sign-in the account password is no longer needed, so it is
        // erased from memory and from the file rather than sitting there being a
        // liability. The refresh token that replaces it is revocable from the
        // myAir account and is useless for signing in as the user anywhere.
        bool dirty = false;
        if (!client.refreshToken().empty() &&
            client.refreshToken() != config_.myair.refresh_token) {
            config_.myair.refresh_token = client.refreshToken();
            dirty = true;
        }
        if (!config_.myair.refresh_token.empty() && !config_.myair.password.empty()) {
            config_.myair.password.clear();
            dirty = true;
            std::cout << "myAir: connected. The password has been replaced by a "
                         "revocable token and erased." << std::endl;
        }
        // The remembered-device cookie, or a region with an email factor asks
        // for a code on every single start.
        if (!client.deviceToken().empty() &&
            client.deviceToken() != config_.myair.device_token) {
            config_.myair.device_token = client.deviceToken();
            dirty = true;
        }
        if (dirty && !config_path_.empty()) config_.save(config_path_);

        return client.fetchSleepRecords(out, err);
    };
}

bool MyAirService::enabled() const {
    // A password OR a refresh token is enough, and after the first sign-in it is
    // always the token: requiring the password here would switch myAir off the
    // moment we succeeded in no longer needing it.
    return config_.myair.enabled && !config_.myair.username.empty() &&
           (!config_.myair.password.empty() || !config_.myair.refresh_token.empty());
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

void MyAirService::adoptTokens(const MyAirClient& client) {
    if (!client.refreshToken().empty()) config_.myair.refresh_token = client.refreshToken();
    if (!client.deviceToken().empty()) config_.myair.device_token = client.deviceToken();
    // The password did its one job. Holding it after this point buys nothing and
    // is the only credential here that is reusable on ResMed's own website.
    config_.myair.password.clear();
    config_.myair.enabled = true;
    needs_reauth_ = false;
    if (!config_path_.empty()) config_.save(config_path_);
}

MyAirAuthState MyAirService::signIn(const std::string& username, const std::string& password,
                                    const std::string& region, std::string& err) {
    err.clear();
    pending_.reset();

    if (username.empty() || password.empty()) {
        err = "A myAir email address and password are both required";
        return MyAirAuthState::Failed;
    }

    auto client = std::make_unique<MyAirClient>(username, password, region);
    // Offer the remembered device, so a region with an email factor stops asking
    // for a code after the first time.
    client->setDeviceToken(config_.myair.device_token);

    const auto state = client->connect(err);
    if (state == MyAirAuthState::MfaRequired) {
        // Keep the half-finished client: the state token that ties the emailed
        // code to this attempt lives inside it.
        config_.myair.username = username;
        config_.myair.region = region;
        pending_ = std::move(client);
        return state;
    }
    if (state != MyAirAuthState::Ok) return state;

    config_.myair.username = username;
    config_.myair.region = region;
    adoptTokens(*client);
    return MyAirAuthState::Ok;
}

MyAirAuthState MyAirService::verifyMfa(const std::string& code, std::string& err) {
    err.clear();
    if (!pending_) {
        err = "No myAir sign-in is waiting for a code";
        return MyAirAuthState::Failed;
    }
    const auto state = pending_->verifyMfa(code, err);
    if (state != MyAirAuthState::Ok) return state;

    adoptTokens(*pending_);
    pending_.reset();
    return MyAirAuthState::Ok;
}

void MyAirService::disconnect() {
    pending_.reset();
    config_.myair.enabled = false;
    config_.myair.password.clear();
    config_.myair.refresh_token.clear();
    // The remembered device goes too: "disconnect" should not leave ResMed
    // holding a trust decision this box made.
    config_.myair.device_token.clear();
    needs_reauth_ = false;
    last_error_.clear();
    if (!config_path_.empty()) config_.save(config_path_);
    // Stored nights are deliberately left alone. The user asked to disconnect,
    // not to delete the history they already have.
}

Json::Value MyAirService::status() const {
    Json::Value j;
    j["enabled"] = config_.myair.enabled;
    j["connected"] = !config_.myair.refresh_token.empty();
    j["username"] = config_.myair.username;
    j["region"] = config_.myair.region;
    j["needs_reauth"] = needs_reauth_;
    j["awaiting_code"] = pending_ != nullptr;
    j["poll_minutes"] = config_.myair.poll_minutes;
    j["last_sync_at"] = static_cast<Json::Int64>(last_sync_at_);
    j["last_error"] = last_error_;
    // Deliberately no credential of any kind, not even masked: this is a plain
    // GET that anything on the LAN can call.
    return j;
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
