#pragma once

#ifndef _WIN32

#include "clients/FysetcProtocol.h"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>

namespace hms_cpap {

struct FysetcDeviceState {
    std::string device_serial;
    uint16_t fw_version       = 0;
    uint32_t boot_count       = 0;
    uint32_t session_id       = 0;
    uint32_t last_boot_count  = 0;
    bool     needs_full_sync  = false;
    fysetc::StatusPayload last_status = {};
    std::chrono::steady_clock::time_point connected_at;
};

class FysetcTcpServer {
public:
    using LogCallback = std::function<void(fysetc::LogLevel, const std::string& tag,
                                           const std::string& msg)>;

    explicit FysetcTcpServer(int port = 9000, const std::string& bind_addr = "0.0.0.0");
    ~FysetcTcpServer();

    void start();
    void stop();
    bool isRunning() const { return running_; }
    bool isConnected() const { return client_fd_ >= 0; }

    void setLogCallback(LogCallback cb) { log_callback_ = std::move(cb); }

    const FysetcDeviceState& deviceState() const { return device_state_; }

    /**
     * True once if the last HELLO reported a different boot_count than the
     * previous connection, meaning the device rebooted or power-cycled and
     * anything cached about its card is suspect. Clears the flag.
     *
     * Read-and-clear rather than a plain getter: the flag is recomputed on every
     * HELLO and stays set for the life of that connection, so a consumer that
     * only peeked would throw away a perfectly good FAT on every burst cycle
     * instead of once on reconnect.
     *
     * The protocol has specified this since v1.0.0 ("If boot_count differs from
     * the server's last-known value, a full FAT re-sync follows"), and the
     * detection has always been implemented, but nothing consumed the flag,
     * which is why a power cycle used to leave a stale FAT in place.
     */
    bool takeFullSyncFlag();

    bool readSectors(const std::vector<fysetc::SectorRange>& ranges,
                     std::vector<uint8_t>& out_data,
                     std::vector<std::pair<uint32_t, uint16_t>>& out_delivered);

    bool ping(uint32_t nonce);

    /**
     * Push a single setting to the device and wait for its CONFIG_ACK.
     *
     * The device stores the value and reboots to apply it, so the connection
     * drops immediately after a successful call and re-establishes a few
     * seconds later. Returns true only when the device acked with OK.
     *
     * Used to sweep the SD clock (ConfigKey::SD_FREQ_KHZ) without pulling the
     * card out of the machine to reflash it.
     */
    bool setConfig(fysetc::ConfigKey key, uint32_t value);

    void disconnect();

private:
    void acceptLoop();
    void handleConnection(int fd);
    bool sendMessage(const std::vector<uint8_t>& msg);
    bool recvMessage(fysetc::MsgHeader& hdr, std::vector<uint8_t>& payload, int timeout_ms);
    bool recvMessageLocked(fysetc::MsgHeader& hdr, std::vector<uint8_t>& payload, int timeout_ms);
    bool processHello(const fysetc::MsgHeader& hdr, const std::vector<uint8_t>& payload);
    void processLog(const std::vector<uint8_t>& payload);
    void processStatus(const std::vector<uint8_t>& payload);
    void processPong(const std::vector<uint8_t>& payload);
    void startDrainLoop();
    void stopDrainLoop();
    void drainLoop();

    int port_;
    std::string bind_addr_;
    int server_fd_ = -1;
    int client_fd_ = -1;
    std::mutex fd_mutex_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::thread drain_thread_;
    std::atomic<bool> pause_drain_{false};
    std::mutex send_mutex_;
    std::mutex recv_mutex_;
    uint16_t next_req_id_ = 1;
    FysetcDeviceState device_state_;
    /// Guards the read-and-clear of needs_full_sync, which is written on the
    /// receive thread and consumed by the burst collector.
    mutable std::mutex full_sync_mutex_;
    LogCallback log_callback_;
};

}  // namespace hms_cpap

#endif // _WIN32
