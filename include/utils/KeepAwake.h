#pragma once

#include <functional>
#include <memory>
#include <string>

namespace hms_cpap {

/**
 * Keeps the computer from idle-sleeping while one is alive, and only then: a
 * computer that sleeps drops the Bluetooth link and with it the live night.
 * Taken when a night starts streaming and released when its stream ends or the
 * link drops. The display may still sleep; nothing else changes.
 *
 *   macOS    an IOKit power assertion, PreventUserIdleSystemSleep
 *   Windows  SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) on
 *            the calling thread: take and release on the same thread (the
 *            guard's owner does, it lives in one function)
 *   Linux    a logind "sleep" inhibitor over D-Bus in a BlueZ build; nothing
 *            elsewhere
 *
 * The OS part is behind Ops so the test binary checks the take/release
 * contract with a fake; the test binary itself never touches the OS.
 */
class KeepAwake {
public:
    struct Ops {
        const char* name = "none";
        /// Takes the hold. False and *error set when the OS refused.
        std::function<bool(const std::string& why, std::string* error)> take;
        std::function<void()> release;
    };

    /// The host's: the OS mechanism above, or one that does nothing.
    static Ops hostOps();

    explicit KeepAwake(std::string why, Ops ops = hostOps());
    ~KeepAwake();
    KeepAwake(const KeepAwake&) = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;

    bool held() const { return held_; }

private:
    std::string why_;
    Ops ops_;
    bool held_ = false;
};

} // namespace hms_cpap
