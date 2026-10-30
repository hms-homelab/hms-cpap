#pragma once

#ifdef WITH_BLE

#include "clients/As11Session.h"
#include "clients/IAirSense11Client.h"
#include "clients/IAirSense11Pairing.h"

#include <sdbus-c++/sdbus-c++.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * AirSense11BleClient: the AirSense 11's own Bluetooth, read directly through
 * the host's adapter (BlueZ over D-Bus), SDD-057. Answers
 * IAirSense11Client::fetchSummary as the bridge's HTTP client does, so
 * AirSense11Pull works unchanged.
 *
 * Only the link lives here: scan, connect, the two characteristics, writes
 * split to the MTU, notifications in. Everything said over the link is
 * as11::Session, which the test binary covers without Bluetooth.
 *
 * The pairing (clientId and masterPairKey) is kept in <data_dir>/as11.json,
 * mode 0600, and never written to a log.
 */
class AirSense11BleClient : public IAirSense11Client, public IAirSense11Pairing {
public:
    /// `address`: the machine's Bluetooth address, or empty for the first
    /// AirSense 11 the scan finds.
    explicit AirSense11BleClient(std::string data_dir, std::string address = "");
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

private:
    std::string data_dir_;
    std::string address_;
    std::string adapter_path_;
    std::unique_ptr<sdbus::IConnection> conn_;

    std::string device_path_;
    std::string device_name_;
    std::string write_char_path_;
    std::string notify_char_path_;
    size_t write_chunk_ = 20;
    std::unique_ptr<sdbus::IProxy> device_proxy_;
    std::unique_ptr<sdbus::IProxy> write_proxy_;
    std::unique_ptr<sdbus::IProxy> notify_proxy_;

    as11::Session session_;
    mutable std::mutex op_mtx_;   // one operation on the machine at a time
    mutable std::mutex err_mtx_;
    std::string last_error_;

    bool findAdapter();
    bool scan(int timeout_ms);
    bool connectLink(int timeout_ms);
    bool discoverGatt();
    bool enableNotifications();
    void dropLink();
    bool writeFrame(const std::vector<uint8_t>& frame);
    void note(const std::string& what);
};

} // namespace hms_cpap

#endif // WITH_BLE
