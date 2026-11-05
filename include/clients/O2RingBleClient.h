#pragma once

#include "clients/IO2RingClient.h"
#include "clients/ble/BleTransport.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hms_cpap {

/**
 * O2RingBleClient: the ring read directly over the host's Bluetooth.
 *
 * Same interface as O2RingClient (HTTP) but connects directly to the O2 Ring
 * through a ble::ITransport (BlueZ, CoreBluetooth or WinRT, by OS). The test
 * binary drives it with a fake transport.
 *
 * Speaks two protocols, chosen when the scan matches the ring: the original
 * O2Ring family's 0xAA protocol, and the O2Ring-S's "OxyII" (OxyIIProtocol.h),
 * whose files are named "<YYYYMMDDhhmmss>.o2s", in plaintext or in the
 * encrypted session newer O2Ring-S firmware opens (SDD-057).
 */
class O2RingBleClient : public IO2RingClient {
public:
    /// `transport`: the host's when null; a test passes a fake.
    explicit O2RingBleClient(std::shared_ptr<ble::ITransport> transport = nullptr);
    ~O2RingBleClient() override;

    bool isConnected() override;
    std::vector<std::string> listFiles() override;
    std::vector<uint8_t> downloadFile(const std::string& filename) override;
    LiveReading getLive() override;
    int getBattery() const override { return cached_battery_; }

    // Which protocol the matched ring speaks.
    enum class Proto { Legacy, OxyII };

    /// Which protocol a scanned device speaks, if it is a ring at all;
    /// *by_t8520 says an O2Ring-S matched only by its worn-mode name.
    static std::optional<Proto> classifyDevice(const std::string& name, const std::vector<std::string>& uuids,
                                               bool* by_t8520 = nullptr);
    /// True if a scanned device looks like a Viatom/Wellue oximeter.
    static bool deviceMatches(const std::string& name, const std::vector<std::string>& uuids);

    static constexpr const char* SVC_UUID = "14839ac4-7d7e-415c-9a42-167340cf2339";
    static constexpr const char* WRITE_UUID = "8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3";
    static constexpr const char* NOTIFY_UUID = "0734594a-a8e7-4b1a-a6b1-cd5243059a57";

private:
    std::shared_ptr<ble::ITransport> transport_;
    std::unique_ptr<ble::ILink> link_;
    ble::Device device_;
    bool have_device_ = false;

    // State
    std::mutex mtx_;
    std::condition_variable cv_;
    bool connected_ = false;
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

    Proto proto_ = Proto::Legacy;
    // The O2Ring-S matched only by its worn-mode name ("T8520_..."), which may
    // not expose the OxyII service. Such a device, once it proves that, is
    // passed over so the scan finds its idle identity ("S8-AW ...").
    bool oxy_by_t8520_ = false;
    std::set<std::string> skip_ids_;

    // Largest write the link takes (ATT MTU - 3), from the link.
    size_t write_chunk_ = 20;

    // OxyII request/reply: the reply carries the request's opcode.
    uint8_t oxy_seq_ = 0;
    int oxy_wait_op_ = -1;
    // Per connection: the handshake has run, and the session key when the
    // ring answered auth with one (every payload after it is AES, SDD-057).
    bool oxy_session_ = false;
    std::optional<std::array<uint8_t, 16>> oxy_key_;

    // Viatom protocol (the original O2Ring family)
    static uint8_t crc8(const uint8_t* data, size_t len);
    static std::vector<uint8_t> buildCmd(uint8_t cmd, uint16_t block = 0,
                                          const std::vector<uint8_t>& payload = {});

    // BLE operations
    bool scan(int timeout_ms = 15000);
    bool connect(int timeout_ms = 15000);
    void disconnect();
    bool writeCharacteristic(const std::vector<uint8_t>& data);
    bool sendAndWait(uint8_t cmd, uint16_t block = 0,
                      const std::vector<uint8_t>& payload = {},
                      int timeout_ms = 10000);

    // Notification handling
    void onNotification(const std::vector<uint8_t>& value);
    void processReassembly();
    void dispatchFrame(const uint8_t* frame, size_t len);

    // OxyII (O2Ring-S)
    bool oxyRequest(uint8_t op, const std::vector<uint8_t>& payload, int timeout_ms);
    bool oxySession();
    void oxyCloseFile();
    std::vector<std::string> oxyListFiles();
    LiveReading oxyLive();
    std::vector<uint8_t> oxyDownload(const std::string& filename);

    // Human-readable label for logs; a UUID-matched device may carry no Name.
    static std::string describeDevice(const ble::Device& d);
};

} // namespace hms_cpap
