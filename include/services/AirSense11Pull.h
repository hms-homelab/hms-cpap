#pragma once

#include "clients/IAirSense11Client.h"
#include "database/IDatabase.h"
#include "parsers/CpapdashBridge.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * AirSense11Pull - the machine's own daily summary, through the bridge, into
 * cpap_daily_summary.
 *
 * For an AirSense 11 whose SD slot cannot power a WiFi card there are no
 * session files to collect; what there is, over the machine's Bluetooth, is
 * its Summary: one record per therapy day with the figures STR.edf carries.
 * Every pull asks from two days before the newest day the last pull brought
 * (the machine revises the current day until it is over), the first one from
 * 2000-01-01, which the machine answers with everything it keeps. The days
 * with usage are written exactly as STR days are (saveSTRDailyRecords, a
 * full-row upsert), minus the nights an operator removed.
 *
 * Pulls are paced: one per `interval`, and a pull that brought nothing does
 * not move the window. The bridge does the Bluetooth; this only asks.
 */
class AirSense11Pull {
public:
    using Clock = std::chrono::steady_clock;

    AirSense11Pull(std::shared_ptr<IAirSense11Client> client,
                   std::shared_ptr<IDatabase> db,
                   std::string device_id,
                   std::chrono::seconds interval = std::chrono::hours(6));

    struct Result {
        bool asked = false;     ///< a pull went out this call (it was due)
        int  days = 0;          ///< day records the spool carried
        int  written = 0;       ///< days with usage written (0 on a day off, or nothing)
        std::string error;      ///< why nothing was written, when asked
    };

    /** One pull if it is due at `now`; the next is due `interval` later. */
    Result run(Clock::time_point now);

    /** Pull now regardless of the pace (a config change, an operator). */
    Result pullNow();

    /** What the next pull asks from. */
    std::string nextFrom() const { return from_; }

    /** Two days before the newest day in `records`, "YYYY-MM-DD"; "" when none. */
    static std::string fromAfter(const std::vector<STRDailyRecord>& records);

    /** The first pull's `from`: everything the machine keeps. */
    static constexpr const char* kFromTheBeginning = "2000-01-01";

private:
    std::shared_ptr<IAirSense11Client> client_;
    std::shared_ptr<IDatabase>         db_;
    std::string                        device_id_;
    std::chrono::seconds               interval_;
    std::string                        from_ = kFromTheBeginning;
    bool                               pulled_once_ = false;
    Clock::time_point                  last_pull_{};
};

} // namespace hms_cpap
