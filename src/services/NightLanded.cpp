#include "services/NightLanded.h"

#include "services/SyncFolderState.h"

#include <iostream>
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

}  // namespace hms_cpap::night_landed
