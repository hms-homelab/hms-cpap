#pragma once

#ifdef WITH_BLE

#include "clients/IO2RingClient.h"
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <optional>
#include <set>

namespace hms_cpap {

/**
 * O2RingBleClient — direct BLE via BlueZ D-Bus (sdbus-c++)
 *
 * Same interface as O2RingClient (HTTP) but connects directly
 * to the O2 Ring via the host's Bluetooth adapter.
 *
 * Speaks two protocols, chosen when the scan matches the ring: the original
 * O2Ring family's 0xAA protocol, and the O2Ring-S's "OxyII" (OxyIIProtocol.h),
 * whose files are named "<YYYYMMDDhhmmss>.o2s". EXPERIMENTAL for the O2Ring-S.
 */
class O2RingBleClient : public IO2RingClient {
public:
    O2RingBleClient();
    ~O2RingBleClient() override;

    bool isConnected() override;
    std::vector<std::string> listFiles() override;
    std::vector<uint8_t> downloadFile(const std::string& filename) override;
    LiveReading getLive() override;
    int getBattery() const override { return cached_battery_; }

private:
    // D-Bus connection (runs event loop in background thread)
    std::unique_ptr<sdbus::IConnection> conn_;
    std::thread dbus_thread_;
    bool dbus_running_ = false;

    // BlueZ object paths (discovered at runtime)
    std::string adapter_path_;
    std::string device_path_;
    std::string write_char_path_;
    std::string notify_char_path_;

    // State
    std::mutex mtx_;
    std::condition_variable cv_;
    bool connected_ = false;
    bool services_resolved_ = false;
    int cached_battery_ = -1;

    // Response buffer (filled by notification handler)
    std::vector<uint8_t> resp_buf_;
    bool resp_ready_ = false;

    // File download state
    std::vector<uint8_t> dl_buf_;
    uint32_t dl_file_size_ = 0;
    uint32_t dl_offset_ = 0;
    uint16_t dl_block_ = 0;
    bool dl_active_ = false;

    // Frame reassembly
    std::vector<uint8_t> reasm_buf_;

    // Keep notify proxy alive so signal handler stays registered
    std::unique_ptr<sdbus::IProxy> notify_proxy_;

    // Which protocol the matched ring speaks.
    enum class Proto { Legacy, OxyII };
    Proto proto_ = Proto::Legacy;
    // The O2Ring-S matched only by its worn-mode name ("T8520_..."), which may
    // not expose the OxyII service. Such a device, once it proves that, is
    // passed over so the scan finds its idle identity ("S8-AW ...").
    bool oxy_by_t8520_ = false;
    std::set<std::string> skip_paths_;

    // Largest write the link takes (ATT MTU - 3), from the characteristic.
    size_t write_chunk_ = 20;

    // OxyII request/reply: the reply carries the request's opcode.
    uint8_t oxy_seq_ = 0;
    int oxy_wait_op_ = -1;

    // Viatom protocol (the original O2Ring family)
    static uint8_t crc8(const uint8_t* data, size_t len);
    static std::vector<uint8_t> buildCmd(uint8_t cmd, uint16_t block = 0,
                                          const std::vector<uint8_t>& payload = {});

    // BLE operations
    bool scan(int timeout_ms = 15000);
    bool connect(int timeout_ms = 15000);
    void disconnect();
    bool discoverGatt();
    bool enableNotifications();
    void writeCharacteristic(const std::vector<uint8_t>& data);
    bool sendAndWait(uint8_t cmd, uint16_t block = 0,
                      const std::vector<uint8_t>& payload = {},
                      int timeout_ms = 10000);

    // Notification handling
    void onNotification(const std::vector<uint8_t>& value);
    void processReassembly();
    void dispatchFrame(const uint8_t* frame, size_t len);

    // Helpers
    std::string findAdapter();
    void startDBusLoop();
    void stopDBusLoop();

    // OxyII (O2Ring-S)
    bool oxyRequest(uint8_t op, const std::vector<uint8_t>& payload, int timeout_ms);
    bool oxySession();
    void oxyCloseFile();
    std::vector<std::string> oxyListFiles();
    LiveReading oxyLive();
    std::vector<uint8_t> oxyDownload(const std::string& filename);

    // Which protocol a scanned org.bluez.Device1 speaks, if it is a ring at
    // all; *by_t8520 says an O2Ring-S matched only by its worn-mode name.
    static std::optional<Proto> classifyDevice(const std::map<std::string, sdbus::Variant>& props,
                                               bool* by_t8520 = nullptr);
    // True if a scanned org.bluez.Device1 looks like a Viatom/Wellue oximeter.
    static bool deviceMatches(const std::map<std::string, sdbus::Variant>& props);
    // Human-readable label for logs; a UUID-matched device may carry no Name.
    static std::string describeDevice(const std::map<std::string, sdbus::Variant>& props);

    static constexpr const char* SVC_UUID = "14839ac4-7d7e-415c-9a42-167340cf2339";
    static constexpr const char* WRITE_UUID = "8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3";
    static constexpr const char* NOTIFY_UUID = "0734594a-a8e7-4b1a-a6b1-cd5243059a57";
};

} // namespace hms_cpap

#endif // WITH_BLE
