#include "services/GarminService.h"

#include "database/SqlDialect.h"
#include "services/OximetryOutbox.h"
#include "services/RemovedNights.h"
#include "utils/AppConfig.h"
#include "utils/MachineClock.h"
#include "utils/OximetryDevice.h"
#include "utils/TimeCompat.h"

#include <ctime>
#include <iostream>
#include <vector>

namespace hms_cpap {

namespace {

/// "YYYY-MM-DD" of a second read in a UTC frame (a wall clock stored as UTC).
std::string isoDate(long long secs) {
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

/// "YYYY-MM-DD" [days] after [date] ("YYYY-MM-DD" or "YYYYMMDD"); "" if unreadable.
std::string addDays(const std::string& date, int days) {
    std::string d;
    for (char c : date) if (c != '-') d += c;
    if (d.size() != 8) return "";
    for (char c : d) if (c < '0' || c > '9') return "";
    std::tm tm{};
    tm.tm_year = std::stoi(d.substr(0, 4)) - 1900;
    tm.tm_mon  = std::stoi(d.substr(4, 2)) - 1;
    tm.tm_mday = std::stoi(d.substr(6, 2));
    tm.tm_hour = 12;
    return isoDate(static_cast<long long>(timegm_utc(&tm)) + days * 86400LL);
}

/// The watch, as the card and the night say it: "Garmin Fenix 7X".
std::string watchName(const std::string& garmin_name) {
    if (garmin_name.empty()) return "";
    if (garmin_name.rfind("Garmin", 0) == 0) return garmin_name;
    return "Garmin " + garmin_name;
}

}  // namespace

GarminService::GarminService(Deps deps)
    : deps_(std::move(deps)), client_(deps_.endpoints) {
    if (deps_.config) {
        client_.setTokens({deps_.config->garmin.access_token, deps_.config->garmin.refresh_token,
                           deps_.config->garmin.client_id});
        if (!client_.tokens().empty()) state_ = "signed_in";
    }
}

GarminService::~GarminService() { stop(); }

void GarminService::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (worker_.joinable()) return;
    stopping_ = false;
    worker_ = std::thread([this] { run(); });
}

void GarminService::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

long long GarminService::nowUtc() const {
    return deps_.now ? deps_.now() : static_cast<long long>(std::time(nullptr));
}

long long GarminService::wallOffset(long long utc) const {
    if (!deps_.db) return hostZoneOffset(utc);
    return machineOffset(loadMachineClock(*deps_.db, deps_.cpap_device_id), utc);
}

bool GarminService::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return deps_.config && deps_.config->garmin.enabled;
}

// ── events ────────────────────────────────────────────────────────────────

void GarminService::enqueue(Event e, const std::string& date_folder, const std::string& code) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.any = true;
        if (e == Event::NightClosed && !date_folder.empty()) pending_.nights.insert(date_folder);
        if (e == Event::SettingsSaved) pending_.settings = true;
        if (e == Event::MfaCode) {
            pending_.mfa = true;
            pending_.mfa_code = code;
        }
    }
    cv_.notify_all();
}

void GarminService::onNightClosed(const std::string& date_folder) {
    if (enabled()) enqueue(Event::NightClosed, date_folder, "");
}

void GarminService::onNightListOpened() {
    if (enabled()) enqueue(Event::NightListOpened, "", "");
}

bool GarminService::fetchNow() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!deps_.config || !deps_.config->garmin.enabled || deps_.config->garmin.email.empty() ||
            deps_.config->garmin.password.empty())
            return false;
    }
    enqueue(Event::FetchNow, "", "");
    return true;
}

void GarminService::onSettingsSaved() { enqueue(Event::SettingsSaved, "", ""); }

bool GarminService::submitMfaCode(const std::string& code) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state_ != "mfa_required" || code.empty()) return false;
        state_ = "signing_in";
    }
    enqueue(Event::MfaCode, "", code);
    return true;
}

void GarminService::run() {
    for (;;) {
        Pending p;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stopping_ || pending_.any; });
            if (stopping_) return;
            p = std::move(pending_);
            pending_ = Pending{};
        }
        if (p.settings) handle(Event::SettingsSaved);
        if (p.mfa) handle(Event::MfaCode, "", p.mfa_code);
        if (p.nights.empty() && !p.settings && !p.mfa) handle(Event::NightListOpened);
        for (const auto& n : p.nights) handle(Event::NightClosed, n);
    }
}

// ── the work ──────────────────────────────────────────────────────────────

void GarminService::handle(Event e, const std::string& date_folder, const std::string& code) {
    std::lock_guard<std::mutex> work(work_mu_);
    struct Busy {
        GarminService* s;
        explicit Busy(GarminService* x) : s(x) { std::lock_guard<std::mutex> lk(s->mu_); s->busy_ = true; }
        ~Busy() { std::lock_guard<std::mutex> lk(s->mu_); s->busy_ = false; }
    } busy(this);

    if (!enabled()) return;

    if (e == Event::SettingsSaved) {
        // A changed account has had its tokens dropped (PUT /api/config):
        // start again from what the settings hold now.
        client_.resetSignIn();
        std::lock_guard<std::mutex> lk(mu_);
        client_.setTokens({deps_.config->garmin.access_token, deps_.config->garmin.refresh_token,
                           deps_.config->garmin.client_id});
        state_ = client_.tokens().empty() ? "signed_out" : "signed_in";
        error_.clear();
        error_kind_.clear();
    }

    if (e == Event::MfaCode) {
        if (!client_.mfaPending()) return;
        const auto a = client_.verifyMfa(code);
        if (a.state == GarminAuth::BadMfaCode) {
            std::lock_guard<std::mutex> lk(mu_);
            state_ = "mfa_required";
            error_ = a.message;
            error_kind_ = garminAuthName(a.state);
            return;
        }
        if (!a.ok()) { noteError(a); return; }
        persistTokens();
        std::cout << "Garmin: signed in (MFA code accepted)" << std::endl;
    } else if (client_.mfaPending()) {
        // Waiting for the code: a new sign-in would only send another one.
        return;
    }

    if (!signedIn() || !ensureProfile()) return;
    const auto dates = datesToFetch(e == Event::NightClosed ? date_folder : "");
    if (dates.empty()) {
        if (e == Event::FetchNow)
            noteFetch(true, "Every night of the last 3 days is already here", 0);
        return;
    }
    fetchDates(dates);
}

bool GarminService::signedIn() {
    if (!client_.tokens().empty()) return true;
    std::string email, password;
    {
        std::lock_guard<std::mutex> lk(mu_);
        email = deps_.config->garmin.email;
        password = deps_.config->garmin.password;
        if (email.empty() || password.empty()) {
            state_ = "not_set_up";
            return false;
        }
        state_ = "signing_in";
    }
    const auto a = client_.signIn(email, password);
    if (a.state == GarminAuth::MfaRequired) {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = "mfa_required";
        error_.clear();
        error_kind_.clear();
        std::cout << "Garmin: sign-in needs the code Garmin sent; enter it on the Devices page" << std::endl;
        return false;
    }
    if (!a.ok()) { noteError(a); return false; }
    persistTokens();
    std::cout << "Garmin: signed in" << std::endl;
    return true;
}

bool GarminService::ensureProfile() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!deps_.config->garmin.display_name.empty()) return true;
    }
    auto p = client_.socialProfile();
    if (!p.ok && p.auth == GarminAuth::Expired) {
        // The saved session is gone: sign in again with the saved password (D1).
        client_.setTokens({});
        if (!signedIn()) return false;
        p = client_.socialProfile();
    }
    if (!p.ok) {
        noteError({p.auth, p.message});
        return false;
    }
    std::string name;
    if (p.body.is_object() && p.body.contains("displayName") && p.body["displayName"].is_string())
        name = p.body["displayName"].get<std::string>();
    if (name.empty() && p.body.is_object() && p.body.contains("userName") && p.body["userName"].is_string())
        name = p.body["userName"].get<std::string>();
    if (name.empty()) {
        noteError({GarminAuth::Changed, kGarminChangedMessage});
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        deps_.config->garmin.display_name = name;
    }
    persistTokens();
    return true;
}

void GarminService::persistTokens() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!deps_.config) return;
    const auto& t = client_.tokens();
    deps_.config->garmin.access_token  = t.access_token;
    deps_.config->garmin.refresh_token = t.refresh_token;
    deps_.config->garmin.client_id     = t.client_id;
    if (!deps_.config_path.empty()) deps_.config->save(deps_.config_path);
    state_ = "signed_in";
    error_.clear();
    error_kind_.clear();
}

void GarminService::noteError(const GarminAuthResult& a) {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = "error";
    error_ = a.state == GarminAuth::Changed ? kGarminChangedMessage : a.message;
    error_kind_ = garminAuthName(a.state);
    std::cerr << "Garmin: " << error_ << std::endl;
}

void GarminService::noteFetch(bool ok, const std::string& message, int stored) {
    std::lock_guard<std::mutex> lk(mu_);
    last_fetch_at_ = nowUtc();
    last_fetch_ok_ = ok;
    last_fetch_message_ = message;
    last_fetch_stored_ = stored;
}

std::set<std::string> GarminService::datesToFetch(const std::string& date_folder) const {
    std::set<std::string> want;
    const long long now = nowUtc();
    const std::string today = isoDate(now + wallOffset(now));
    for (int d = 0; d <= 3; ++d) want.insert(addDays(today, -d));
    if (!date_folder.empty()) {
        const std::string morning = addDays(date_folder, 1);
        if (!morning.empty()) want.insert(morning);
    }
    std::set<std::string> out;
    for (const auto& w : want)
        if (!w.empty() && !haveNight(w)) out.insert(w);
    return out;
}

bool GarminService::haveNight(const std::string& date) const {
    if (!deps_.db) return false;
    const std::string night = addDays(date, -1);
    const DbType dt = deps_.db->dbType();
    const auto rows = deps_.db->executeQuery(
        "SELECT 1 AS one FROM oximetry_sessions WHERE device_id = " + sql::param(1, dt) +
            " AND source = " + sql::param(2, dt) + " AND " + sql::sleepDay("start_time", dt) + " = " +
            sql::castDate(3, dt) + " LIMIT 1",
        {kOximetryDeviceId, kOximetrySourceGarmin, night});
    return rows.isArray() && !rows.empty();
}

int GarminService::fetchDates(const std::set<std::string>& dates) {
    std::string display;
    {
        std::lock_guard<std::mutex> lk(mu_);
        display = deps_.config->garmin.display_name;
    }
    int stored = 0;
    bool signed_in_again = false;
    bool device_asked = false;
    std::string waiting;
    for (const auto& date : dates) {
        auto res = client_.sleepData(display, date);
        if (!res.ok && res.auth == GarminAuth::Expired && !signed_in_again) {
            // The session is gone: sign in again with the saved password (D1).
            signed_in_again = true;
            client_.setTokens({});
            if (!signedIn()) {
                noteFetch(false, "Garmin needs a new sign-in", stored);
                return stored;
            }
            res = client_.sleepData(display, date);
        }
        if (!res.ok) {
            noteError({res.auth, res.message});
            noteFetch(false, res.auth == GarminAuth::Changed ? kGarminChangedMessage : res.message, stored);
            return stored;
        }
        const long long now = nowUtc();
        const auto night = convertGarminSleep(res.body, now, [this](long long utc) { return wallOffset(utc); });
        if (night.verdict != GarminNight::Verdict::Ready) {
            std::cout << "Garmin: " << date << ": " << night.reason << "; asked again on the next event"
                      << std::endl;
            waiting = date + ": " + night.reason;
            continue;
        }
        if (!device_asked) {
            device_asked = true;
            const auto d = client_.lastUsedDevice();
            if (d.ok && d.body.is_object() && d.body.contains("lastUsedDeviceName") &&
                d.body["lastUsedDeviceName"].is_string()) {
                std::lock_guard<std::mutex> lk(mu_);
                deps_.config->garmin.device_name = watchName(d.body["lastUsedDeviceName"].get<std::string>());
            }
        }
        if (storeNight(night)) {
            ++stored;
            std::cout << "Garmin: stored the night of " << date << " (" << night.valid_spo2 << " SpO2 samples)"
                      << std::endl;
        }
    }
    persistTokens();   // a refresh may have rotated them; the device name too
    std::string msg;
    if (stored > 0) msg = "Stored " + std::to_string(stored) + (stored == 1 ? " night" : " nights");
    else if (!waiting.empty()) msg = waiting;
    else msg = "Nothing new from Garmin";
    noteFetch(true, msg, stored);
    return stored;
}

bool GarminService::storeNight(const GarminNight& night) {
    if (!deps_.db) return false;
    // SDD-029: a removed night stays removed.
    if (!deps_.cpap_device_id.empty()) {
        const auto removed = removedNightSet(*deps_.db, deps_.cpap_device_id);
        if (isRemovedOximetryNight(removed, night.session.start_time)) return false;
    }
    IDatabase::OximetryOrigin origin;
    origin.source = kOximetrySourceGarmin;
    {
        std::lock_guard<std::mutex> lk(mu_);
        origin.device_name = deps_.config ? deps_.config->garmin.device_name : "";
    }
    if (!deps_.db->saveOximetrySessionFrom(kOximetryDeviceId, night.session, origin)) return false;
    // SDD-079 D4: hms-json with Garmin's own instants (clock "utc"). A night
    // fetched again rewrites it under the same name and it goes whole again.
    oximetry_outbox::stored(night.session, origin, "", &night.sample_utc_s);

    // D6: the same night uploaded by an outside importer (the reference's
    // garmin-*.csv as "upload", or one it labelled "garmin") is this one now.
    const DbType dt = deps_.db->dbType();
    const std::string night_day = oximetrySleepDay(night.session.start_time);
    const std::string same =
        "device_id = " + sql::param(1, dt) + " AND " + sql::sleepDay("start_time", dt) + " = " +
        sql::castDate(2, dt) + " AND filename <> " + sql::param(3, dt) +
        " AND ((source = 'upload' AND LOWER(filename) LIKE 'garmin%') OR source = 'garmin')";
    const std::vector<std::string> args{kOximetryDeviceId, night_day, night.session.filename};
    deps_.db->executeQuery("DELETE FROM oximetry_samples WHERE oximetry_session_id IN "
                           "(SELECT id FROM oximetry_sessions WHERE " + same + ")",
                           args);
    deps_.db->executeQuery("DELETE FROM oximetry_sessions WHERE " + same, args);
    return true;
}

Json::Value GarminService::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    Json::Value v(Json::objectValue);
    const bool on = deps_.config && deps_.config->garmin.enabled;
    const bool configured = deps_.config && !deps_.config->garmin.email.empty() &&
                            !deps_.config->garmin.password.empty();
    v["enabled"]     = on;
    v["configured"]  = configured;
    v["email"]       = deps_.config ? deps_.config->garmin.email : "";
    v["device_name"] = deps_.config ? deps_.config->garmin.device_name : "";
    std::string state = state_;
    if (!on) state = "off";
    else if (!configured && state_ != "mfa_required") state = "not_set_up";
    v["state"]      = state;
    v["error"]      = error_;
    v["error_kind"] = error_kind_;
    v["busy"]       = busy_;
    if (last_fetch_at_) {
        Json::Value f(Json::objectValue);
        f["at"]      = static_cast<Json::Int64>(last_fetch_at_);
        f["ok"]      = last_fetch_ok_;
        f["message"] = last_fetch_message_;
        f["stored"]  = last_fetch_stored_;
        v["last_fetch"] = f;
    } else {
        v["last_fetch"] = Json::Value(Json::nullValue);
    }
    return v;
}

}  // namespace hms_cpap
