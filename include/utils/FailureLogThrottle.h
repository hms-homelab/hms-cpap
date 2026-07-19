#pragma once
//
// FailureLogThrottle — collapses a storm of identical failures into one log
// line plus periodic "still failing" summaries, and announces recovery.
//
// Why: when the Mule/ez Share device is unreachable, the poll loop retries
// roughly once a minute and every attempt logged a full error. A single
// offline device produced thousands of identical lines a day, which buried
// real signal in the journal and turned the hms-portal health summary red
// with no added information.
//
// The FIRST failure is always logged in full (an outage must be visible
// immediately). Identical repeats are counted and suppressed until the
// summary interval elapses, then one line reports the streak. When the call
// finally succeeds, recovery is logged with the streak length.
//
// The clock is injectable so tests are deterministic and need no sleeping.
//
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace hms_cpap {

class FailureLogThrottle {
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using NowFn     = std::function<TimePoint()>;

    struct Decision {
        bool        log = false;   // should the caller emit a line?
        std::string message;       // what to emit when log == true
    };

    explicit FailureLogThrottle(
        std::chrono::seconds summary_interval = std::chrono::minutes(15),
        NowFn now = [] { return Clock::now(); })
        : summary_interval_(summary_interval), now_(std::move(now)) {}

    // Report a failed attempt. A new or changed error is logged immediately;
    // identical repeats are suppressed until summary_interval_ has passed.
    Decision onFailure(const std::string& error) {
        const TimePoint t = now_();

        if (!active_ || error != last_error_) {
            active_       = true;
            last_error_   = error;
            consecutive_  = 1;
            suppressed_   = 0;
            first_failure_ = t;
            last_logged_   = t;
            return {true, error};
        }

        ++consecutive_;
        ++suppressed_;
        if (t - last_logged_ >= summary_interval_) {
            last_logged_ = t;
            const auto mins =
                std::chrono::duration_cast<std::chrono::minutes>(t - first_failure_).count();
            const uint64_t hidden = suppressed_;
            suppressed_ = 0;
            return {true, error + " [still failing: " + std::to_string(consecutive_) +
                          " consecutive failures over " + std::to_string(mins) +
                          " min; " + std::to_string(hidden) +
                          " identical errors suppressed]"};
        }
        return {false, {}};
    }

    // Report a successful attempt. Announces recovery if we were failing.
    Decision onSuccess() {
        if (!active_) return {false, {}};
        const uint64_t streak = consecutive_;
        active_      = false;
        consecutive_ = 0;
        suppressed_  = 0;
        last_error_.clear();
        return {true, "recovered after " + std::to_string(streak) +
                      (streak == 1 ? " failure" : " consecutive failures")};
    }

    bool     failing() const { return active_; }
    uint64_t consecutive() const { return consecutive_; }

private:
    std::chrono::seconds summary_interval_;
    NowFn                now_;
    std::string          last_error_;
    uint64_t             consecutive_{0};
    uint64_t             suppressed_{0};
    TimePoint            first_failure_{};
    TimePoint            last_logged_{};
    bool                 active_{false};
};

} // namespace hms_cpap
