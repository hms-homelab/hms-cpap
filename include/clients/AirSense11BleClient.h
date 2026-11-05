#pragma once

#include "clients/As11Session.h"
#include "clients/IAirSense11Client.h"
#include "clients/IAirSense11Pairing.h"
#include "clients/ble/BleTransport.h"
#include "services/As11NightWriter.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hms_cpap {

/**
 * AirSense11BleClient: the AirSense 11's own Bluetooth, read directly through
 * the host's adapter, SDD-057. Answers IAirSense11Client::fetchSummary as the
 * bridge's HTTP client does, so AirSense11Pull works unchanged.
 *
 * Only the link's decisions live here: which device, when to rediscover,
 * writes split to the MTU, notifications in. The link itself is a
 * ble::ITransport (BlueZ, CoreBluetooth or WinRT, by OS), and everything said
 * over it is as11::Session, so the test binary covers this class with a fake
 * transport and no Bluetooth.
 *
 * The pairing (clientId and masterPairKey) is kept in <data_dir>/as11.json,
 * mode 0600, and never written to a log.
 */
class AirSense11BleClient : public IAirSense11Client, public IAirSense11Pairing {
public:
    /// `address`: the machine's Bluetooth address (on macOS, the system's
    /// identifier for it), or empty for the first AirSense 11 the scan finds.
    /// `transport`: the host's when null; a test passes a fake.
    explicit AirSense11BleClient(std::string data_dir, std::string address = "",
                                 std::shared_ptr<ble::ITransport> transport = nullptr,
                                 as11::Timeouts timeouts = {});
    ~AirSense11BleClient() override;
    AirSense11BleClient(const AirSense11BleClient&) = delete;
    AirSense11BleClient& operator=(const AirSense11BleClient&) = delete;

    std::vector<uint8_t> fetchSummary(const std::string& from) override;

    /// Pairing, step 1: connects and starts the key exchange. WaitCode means
    /// the machine's screen shows the code now; the link stays up for
    /// pairConfirm, which must follow within the machine's window (under a
    /// minute). Any other result leaves the link down.
    as11::Result pairStart() override;
    /// Pairing, step 2: the code from the machine's screen. Ok saves the
    /// pairing. The link is down on return, whatever happened.
    as11::Result pairConfirm(const std::string& code) override;
    void pairCancel() override;
    bool pairPending() const override;
    bool paired() const override;
    std::string lastError() const override;

    static constexpr int kScanMs = 15000;
    static constexpr int kConnectMs = 15000;

    /// The live night (SDD-057 step 4): a thread that stays connected while
    /// the machine is awake, streams while it is in therapy into `writer`,
    /// and calls `night_ended` when a night's stream ends (the cue for the
    /// Summary pull). It yields the radio to a summary pull or a pairing
    /// while it only waits; a night in progress is never interrupted.
    void startNightLoop(std::shared_ptr<As11NightWriter> writer, std::function<void()> night_ended);
    void stopNightLoop();

    static constexpr int kStallMs = 30000;       // no report this long: the stream is dead
    static constexpr int kRetryMs = 30000;       // after the machine was not found

private:
    void nightLoop();
    bool streamOnce();                            // false when the machine was not reached
    void afterPull(const std::string& serial, const std::string& from, const std::vector<uint8_t>& bytes);

    std::shared_ptr<As11NightWriter> writer_;
    std::function<void()> night_ended_;
    std::thread night_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> want_radio_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<bool> therapy_{false};
    std::atomic<uint64_t> reports_{0};
    std::mutex wake_mtx_;
    std::condition_variable wake_;

    std::string data_dir_;
    std::string address_;
    std::shared_ptr<ble::ITransport> transport_;

    ble::Device device_;
    bool have_device_ = false;
    // The machine's own name, as seen when it was paired in this process. A
    // device the OS has cached under another name is not reused: the first
    // pull after a pairing once connected to a stale one and hung.
    std::string machine_name_;
    // Set when a pairing ends: the next scan ignores what the OS has cached
    // and finds the machine by its advertisement.
    bool rediscover_ = false;
    size_t write_chunk_ = 20;
    std::unique_ptr<ble::ILink> link_;

    as11::Session session_;
    mutable std::mutex op_mtx_;   // one operation on the machine at a time
    mutable std::mutex err_mtx_;
    std::string last_error_;
    // Why connectLink last failed: Connect (the machine), or RadioOff (this
    // computer's Bluetooth is off, held off by rf-kill, or not allowed to
    // this app). Under op_mtx_.
    as11::Result link_failure_ = as11::Result::Connect;

    bool scan(int timeout_ms);
    bool connectLink(int timeout_ms);
    void dropLink();
    bool writeFrame(const std::vector<uint8_t>& frame);
    void note(const std::string& what);
};

} // namespace hms_cpap
