#include "services/ListingPlan.h"

#include "utils/TimeCompat.h"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace hms_cpap {

namespace {

constexpr long long kDay = 24 * 3600;

bool isDateFolder(const std::string& f) {
    return f.size() == 8 &&
           std::all_of(f.begin(), f.end(), [](unsigned char c) { return std::isdigit(c); });
}

long long minutes(std::chrono::minutes m) { return m.count() * 60; }

}  // namespace

std::string machineDay(long long host_now, long long offset, int days) {
    const std::time_t t = static_cast<std::time_t>(host_now + offset + days * kDay);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[9];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return buf;
}

std::string tonightFolder(long long host_now, long long offset) {
    return machineDay(host_now - 12 * 3600, offset);
}

bool isFutureFolder(const std::string& folder, long long host_now, long long offset) {
    return isDateFolder(folder) && folder > machineDay(host_now, offset, 1);
}

bool isRecentFolder(const std::string& folder, long long host_now, long long offset) {
    return isDateFolder(folder) && !isFutureFolder(folder, host_now, offset) &&
           folder >= machineDay(host_now, offset, -kRecentDays);
}

long long lastMachineNoon(long long host_now, long long offset) {
    const long long machine_now = host_now + offset;
    long long day_start = machine_now - (((machine_now % kDay) + kDay) % kDay);
    long long noon = day_start + 12 * 3600;
    if (noon > machine_now) noon -= kDay;
    return noon - offset;
}

const char* listReasonString(ListReason r) {
    switch (r) {
    case ListReason::Tonight:    return "tonight";
    case ListReason::FirstLook:  return "first look";
    case ListReason::Relist:     return "re-list";
    case ListReason::Live:       return "live";
    case ListReason::Checkpoint: return "checkpoint";
    case ListReason::Ceiling:    return "ceiling";
    }
    return "?";
}

std::vector<PlannedListing> planFolderListings(
    long long now, long long offset,
    const std::vector<std::string>& card_folders,
    const std::set<std::string>& candidates,
    const std::map<std::string, FolderWatch>& watch,
    const std::set<std::string>& done) {

    std::vector<PlannedListing> out;
    std::set<std::string> taken;
    auto take = [&](const std::string& f, ListReason r) {
        if (taken.insert(f).second) out.push_back({f, r});
    };

    const std::string tonight = tonightFolder(now, offset);
    take(tonight, ListReason::Tonight);

    std::set<std::string> folders(card_folders.begin(), card_folders.end());
    for (const auto& [f, w] : watch) folders.insert(f);

    for (const auto& f : folders) {
        if (!isDateFolder(f) || f == tonight) continue;
        if (done.count(f)) continue;   // D9: done is never listed again
        const auto it = watch.find(f);
        const FolderWatch none;
        const FolderWatch& w = it != watch.end() ? it->second : none;
        const bool future = isFutureFolder(f, now, offset);
        if (w.parked_until > now) continue;   // keeps refusing: retried after an hour

        if (w.last_listed == 0) {
            // D7: a future folder is listed once, when the index finds it.
            if (future || candidates.count(f) || isRecentFolder(f, now, offset))
                take(f, ListReason::FirstLook);
            continue;
        }
        if (future) continue;
        if (w.relist) { take(f, ListReason::Relist); continue; }
        if (w.growing) { take(f, ListReason::Live); continue; }
        if (w.stopped_at > 0) {
            bool due = false;
            for (const auto cp : kGrowthCheckpoints) {
                const long long at = w.stopped_at + minutes(cp);
                if (now >= at && w.last_listed < at) due = true;
            }
            if (due) { take(f, ListReason::Checkpoint); continue; }
        }
        if (isRecentFolder(f, now, offset) &&
            now - w.last_listed >= std::chrono::duration_cast<std::chrono::seconds>(kRelistCeiling).count())
            take(f, ListReason::Ceiling);
    }
    return out;
}

FolderWatch observeFolder(const FolderWatch& prev, const std::string& sig,
                          bool stored_something, long long now) {
    FolderWatch next = prev;
    const bool grew = (prev.last_listed > 0 && sig != prev.sig) || stored_something;
    if (grew) {
        next.growing = true;
        next.last_growth = now;
        next.stopped_at = 0;
    } else if (prev.growing) {
        next.growing = false;
        next.stopped_at = now;
    }
    next.sig = sig;
    next.last_listed = now;
    next.relist = false;
    return next;
}

bool indexDue(const IndexState& s, long long now, long long offset, bool night_growing) {
    if (!s.ok_this_run || s.reason) return true;
    if (night_growing) return false;
    return s.last_index < lastMachineNoon(now, offset);
}

bool settingsDue(const SettingsState& s, long long now, bool night_in_progress,
                 bool therapy_started, bool checkpoint_listed) {
    if (s.reason || s.last_listed == 0) return true;
    // An AirSense 11 card: its SETTINGS answer changes only with a settings
    // change, a swap or a fault, so only D6's reasons list it.
    if (!s.logs_on_card) return false;
    if (therapy_started || checkpoint_listed) return true;
    const long long floor = minutes(night_in_progress ? kSettingsNightFloor : kSettingsIdleFloor);
    return now - s.last_listed >= floor;
}

bool settingsListingHasLogs(const std::vector<std::string>& names) {
    for (const auto& n : names) {
        std::string l = n;
        std::transform(l.begin(), l.end(), l.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (l.rfind("currentsettings.", 0) == 0) continue;
        return true;
    }
    return false;
}

}  // namespace hms_cpap
