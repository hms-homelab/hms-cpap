#pragma once

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
    void clearFullSyncFlag() { device_state_.needs_full_sync = false; }

    bool readSectors(const std::vector<fysetc::SectorRange>& ranges,
                     std::vector<uint8_t>& out_data,
                     std::vector<std::pair<uint32_t, uint16_t>>& out_delivered);

    bool ping(uint32_t nonce);

    void disconnect();

private:
    void acceptLoop();
    void handleConnection(int fd);
    bool sendMessage(const std::vector<uint8_t>& msg);
    bool recvMessage(fysetc::MsgHeader& hdr, std::vector<uint8_t>& payload, int timeout_ms);
    bool processHello(const fysetc::MsgHeader& hdr, const std::vector<uint8_t>& payload);
    void processLog(const std::vector<uint8_t>& payload);
    void processStatus(const std::vector<uint8_t>& payload);
    void processPong(const std::vector<uint8_t>& payload);

    int port_;
    std::string bind_addr_;
    int server_fd_ = -1;
    int client_fd_ = -1;
    std::mutex fd_mutex_;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex send_mutex_;
    std::mutex recv_mutex_;
    uint16_t next_req_id_ = 1;
    FysetcDeviceState device_state_;
    LogCallback log_callback_;
};

}  // namespace hms_cpap
