#pragma once
//
// SDD-078: Garmin SpO2 as an oximetry source.
//
// D2, events and never a timer. Garmin cannot say a night is ready, and the
// watch syncs some time after waking, so a night is asked for when something
// happens, and only then:
//   - a CPAP night closes (SDD-046's quiet hour): that night;
//   - a client opens the night list or the dashboard;
//   - "Fetch from Garmin now" on the Devices page;
//   - the Garmin settings are saved, and an MFA code is entered.
// Each event also asks again for every night of the last 3 days that has no
// Garmin recording yet. An incomplete sleep period, or one with too few
// samples, is left for the next event. There is no clock in here: the worker
// thread sleeps on its queue until an event lands on it.
//
// D1: the account's email and password and Garmin's session tokens are
// settings (AppConfig::Garmin). Tokens are refreshed, and a refused session
// is signed in again with the saved password, with nobody at the keyboard;
// only an MFA code needs the user, asked inline on the Devices page.
//
// D3/D6: a night is stored under kOximetryDeviceId with source "garmin" and
// the watch's name, keyed by its sleep period (garmin-<start ms>), and it
// replaces a copy of the same night an outside importer uploaded.
//
#include "clients/GarminConnectClient.h"
#include "database/IDatabase.h"
#include "services/GarminSleep.h"

#include <json/json.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace hms_cpap {

struct AppConfig;

class GarminService {
public:
    enum class Event { NightClosed, NightListOpened, FetchNow, SettingsSaved, MfaCode };

    struct Deps {
        std::shared_ptr<IDatabase> db;     ///< its own connection on a server database
        AppConfig*  config = nullptr;      ///< the settings; tokens are written back here
        std::string config_path;           ///< where they are saved ("" saves nothing)
        std::string cpap_device_id;        ///< whose machine clock lines the nights up (D5)
        GarminEndpoints endpoints;
        /// The current UTC second (the system clock when unset).
        std::function<long long()> now;
    };

    explicit GarminService(Deps deps);
    ~GarminService();

    /// The worker thread, waiting on events.
    void start();
    void stop();

    // -- The events (D2). Each returns at once; the worker does the work. ----

    /// SDD-046: the night in DATALOG folder [date_folder] (YYYYMMDD) closed.
    void onNightClosed(const std::string& date_folder);
    /// A client fetched the night list or the dashboard.
    void onNightListOpened();
    /// "Fetch from Garmin now". False when Garmin is off or not set up.
    bool fetchNow();
    /// The Garmin settings were saved (a changed account drops the old sign-in).
    void onSettingsSaved();
    /// The MFA code Garmin asked for. False when no sign-in is waiting for one.
    bool submitMfaCode(const std::string& code);

    /// What the Devices page's Garmin card shows (D4): {"enabled",
    /// "configured", "state": off|not_set_up|signed_out|signing_in|
    /// mfa_required|signed_in|error, "email", "device_name", "error",
    /// "error_kind", "busy", "last_fetch": {"at", "ok", "message", "stored"}}.
    Json::Value status() const;

    /// Runs one event now, on the calling thread, as the worker would. For
    /// the suite (and the worker itself).
    void handle(Event e, const std::string& date_folder = "", const std::string& code = "");

    /// Garmin calendar dates (YYYY-MM-DD) this event asks for: the closed
    /// night's (the morning after its folder's date), and today and the 3
    /// days before on the machine's clock, less those already stored.
    std::set<std::string> datesToFetch(const std::string& date_folder) const;

    /// Whether a Garmin recording is stored for the night that ended on
    /// Garmin calendar date [date] (sleep day the day before).
    bool haveNight(const std::string& date) const;

private:
    struct Pending {
        bool any = false;
        bool settings = false;
        std::set<std::string> nights;
        std::string mfa_code;
        bool mfa = false;
    };

    void run();
    void enqueue(Event e, const std::string& date_folder, const std::string& code);
    bool enabled() const;
    bool signedIn();
    bool ensureProfile();
    void persistTokens();
    void noteError(const GarminAuthResult& a);
    void noteFetch(bool ok, const std::string& message, int stored);
    int  fetchDates(const std::set<std::string>& dates);
    bool storeNight(const GarminNight& night);
    long long nowUtc() const;
    long long wallOffset(long long utc) const;

    Deps deps_;
    GarminConnectClient client_;

    mutable std::mutex mu_;      // the state below and the config's garmin block
    std::condition_variable cv_;
    Pending pending_;
    bool stopping_ = false;
    std::thread worker_;

    std::mutex work_mu_;         // one event at a time on the client
    std::string state_ = "signed_out";
    std::string error_;
    std::string error_kind_;
    bool busy_ = false;
    long long last_fetch_at_ = 0;
    bool last_fetch_ok_ = false;
    std::string last_fetch_message_;
    int last_fetch_stored_ = 0;
};

}  // namespace hms_cpap
