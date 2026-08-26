#pragma once
//
// MyAirService (SDD-020) — keeps a local copy of what ResMed's own servers say
// about the same nights, so their score can sit next to ours.
//
// OPT IN, AND NOTHING DEPENDS ON IT. With myair.enabled = false this does
// nothing at all, and with it on, every failure degrades to "myAir unavailable"
// rather than blocking a sweep, a parse or a report. The API is undocumented and
// ResMed can change it without notice; the card is the product, this is a second
// opinion on it.
//
// No thread and no scheduler, following CpapDashSyncService: the burst loop
// already runs on a timer, so sweep() is called from there and decides for
// itself whether enough time has passed. A detached thread here would be a
// second clock to reason about for no gain.
//
// The fetch is injected so that everything below the network is testable without
// one. That matters more than usual here, because the one thing we cannot test
// against is the real service.
//
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "database/IDatabase.h"
#include "services/MyAirClient.h"

namespace hms_cpap {

// Forward declared INSIDE the namespace on purpose. At global scope this
// declares a different type from hms_cpap::AppConfig, and the error it produces
// ("cannot bind to a value of unrelated type 'AppConfig'") names the same word
// twice and reads like a compiler bug.
struct AppConfig;

class MyAirService {
public:
    /// Fill `out` with recent nights. Return false and set `err` on any failure,
    /// having changed nothing.
    using Fetch = std::function<bool(std::vector<MyAirSleepRecord>& out, std::string& err)>;

    /// `config_path` is where the remembered-device token is written back to.
    /// Empty means "do not persist it", which costs a fresh emailed code on the
    /// next start in a region that has an email factor.
    MyAirService(std::shared_ptr<IDatabase> db, AppConfig& config,
                 std::string config_path = "");

    /// Replace the network fetch. Tests use this; production leaves it alone and
    /// gets a real MyAirClient.
    void setFetch(Fetch fetch) { fetch_ = std::move(fetch); }

    bool enabled() const;

    /// Called from the burst loop. Polls at most once per myair.poll_minutes and
    /// returns immediately the rest of the time. Never throws.
    void sweep();

    /// Fetch and store now, ignoring the interval. Returns the number of nights
    /// stored, or -1 on failure with `err` set.
    int syncNow(std::string& err);

    const std::string& lastError() const { return last_error_; }
    std::time_t lastSyncAt() const { return last_sync_at_; }

    /// True once the stored token has been rejected and discarded. The only cure
    /// is an interactive sign-in, so this is a state to show the user rather
    /// than something to keep retrying.
    bool needsReauth() const { return needs_reauth_; }

    /// Write `records` into cpap_myair_records, replacing whatever was there for
    /// the dates they cover.
    ///
    /// Delete-the-window then insert-the-window, rather than an upsert: the whole
    /// window is refetched every poll, so this is naturally idempotent, and it
    /// avoids writing three different upsert dialects, which is where this
    /// codebase has repeatedly diverged between backends.
    int store(const std::vector<MyAirSleepRecord>& records, std::string& err);

private:
    std::shared_ptr<IDatabase> db_;
    AppConfig& config_;
    std::string config_path_;
    Fetch fetch_;
    std::string last_error_;
    std::time_t last_sync_at_ = 0;
    bool needs_reauth_ = false;
};

}  // namespace hms_cpap
