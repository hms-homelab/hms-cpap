#include "clients/FysetcTcpServer.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <chrono>

namespace hms_cpap {

FysetcTcpServer::FysetcTcpServer(int port, const std::string& bind_addr)
    : port_(port), bind_addr_(bind_addr) {}

FysetcTcpServer::~FysetcTcpServer() {
    stop();
}

void FysetcTcpServer::start() {
    if (running_) return;

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "FysetcTcp: socket() failed: " << strerror(errno) << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "FysetcTcp: bind() failed: " << strerror(errno) << std::endl;
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    if (listen(server_fd_, 1) < 0) {
        std::cerr << "FysetcTcp: listen() failed: " << strerror(errno) << std::endl;
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    running_ = true;
    accept_thread_ = std::thread(&FysetcTcpServer::acceptLoop, this);

    std::cout << "FysetcTcp: Listening on " << bind_addr_ << ":" << port_ << std::endl;
}

void FysetcTcpServer::stop() {
    running_ = false;
    disconnect();
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void FysetcTcpServer::disconnect() {
    std::lock_guard<std::mutex> lock(fd_mutex_);
    if (client_fd_ >= 0) {
        close(client_fd_);
        client_fd_ = -1;
    }
}

void FysetcTcpServer::acceptLoop() {
    while (running_) {
        struct pollfd pfd = {server_fd_, POLLIN, 0};
        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0) continue;

        struct sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        int fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (fd < 0) continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        std::cout << "FysetcTcp: Connection from " << ip << std::endl;

        // Close any existing connection (single-tenant)
        {
            std::lock_guard<std::mutex> lock(fd_mutex_);
            if (client_fd_ >= 0) {
                std::cout << "FysetcTcp: Dropping previous connection" << std::endl;
                close(client_fd_);
            }

            int nodelay = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

            // Set send timeout to avoid blocking forever (M7)
            struct timeval snd_tv = {.tv_sec = 10, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

            // Enable TCP keepalive for stale connection detection (H2)
            int keepalive = 1;
            int keepidle = 30;
            int keepintvl = 10;
            int keepcnt = 3;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

            client_fd_ = fd;
        }
        handleConnection(fd);
    }
}

void FysetcTcpServer::handleConnection(int fd) {
    // Wait for HELLO
    fysetc::MsgHeader hdr;
    std::vector<uint8_t> payload;

    if (!recvMessage(hdr, payload, 10000)) {
        std::cerr << "FysetcTcp: No HELLO received, closing" << std::endl;
        disconnect();
        return;
    }

    if (hdr.type != fysetc::MsgType::HELLO) {
        std::cerr << "FysetcTcp: Expected HELLO, got 0x"
                  << std::hex << (int)static_cast<uint8_t>(hdr.type) << std::dec << std::endl;
        disconnect();
        return;
    }

    if (!processHello(hdr, payload)) {
        disconnect();
        return;
    }

    // Send HELLO_ACK
    auto now = std::chrono::system_clock::now();
    uint64_t unix_time = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    device_state_.session_id++;

    auto ack = fysetc::encodeHelloAck(hdr.req_id, unix_time, device_state_.session_id);
    if (!sendMessage(ack)) {
        disconnect();
        return;
    }

    device_state_.connected_at = std::chrono::steady_clock::now();
    std::cout << "FysetcTcp: Device " << device_state_.device_serial
              << " connected (boot #" << device_state_.boot_count
              << ", fw " << (device_state_.fw_version >> 8) << "."
              << (device_state_.fw_version & 0xFF)
              << ", session " << device_state_.session_id << ")" << std::endl;

    // Connection is now established. We stay connected and service
    // readSectors() calls from the acquisition service. Incoming LOG/STATUS
    // messages are drained opportunistically during recvMessage().
}

bool FysetcTcpServer::processHello(const fysetc::MsgHeader& hdr,
                                    const std::vector<uint8_t>& payload) {
    (void)hdr;
    fysetc::HelloPayload hello;
    if (!fysetc::decodeHello(payload.data(), payload.size(), hello)) {
        std::cerr << "FysetcTcp: Invalid HELLO payload" << std::endl;
        return false;
    }

    device_state_.device_serial = std::string(hello.device_serial,
        strnlen(hello.device_serial, 16));
    device_state_.fw_version = hello.fw_version;

    if (hello.boot_count != device_state_.last_boot_count) {
        device_state_.needs_full_sync = true;
        device_state_.last_boot_count = hello.boot_count;
    } else {
        device_state_.needs_full_sync = false;
    }
    device_state_.boot_count = hello.boot_count;

    return true;
}

void FysetcTcpServer::processLog(const std::vector<uint8_t>& payload) {
    if (payload.size() < 13) return;

    auto level = static_cast<fysetc::LogLevel>(payload[0]);
    // uint32_t timestamp_ms at [1..4]
    std::string tag(reinterpret_cast<const char*>(&payload[5]), 8);
    auto null_pos = tag.find('\0');
    if (null_pos != std::string::npos) tag.resize(null_pos);

    std::string msg;
    if (payload.size() > 13) {
        msg = std::string(reinterpret_cast<const char*>(&payload[13]),
                          payload.size() - 13);
        auto msg_null = msg.find('\0');
        if (msg_null != std::string::npos) msg.resize(msg_null);
    }

    if (log_callback_) {
        log_callback_(level, tag, msg);
    }
}

void FysetcTcpServer::processStatus(const std::vector<uint8_t>& payload) {
    fysetc::decodeStatus(payload.data(), payload.size(), device_state_.last_status);
}

void FysetcTcpServer::processPong(const std::vector<uint8_t>& payload) {
    if (payload.size() >= 4 + fysetc::StatusPayload::WIRE_SIZE) {
        fysetc::decodeStatus(payload.data() + 4, payload.size() - 4,
                             device_state_.last_status);
    }
}

bool FysetcTcpServer::sendMessage(const std::vector<uint8_t>& msg) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (client_fd_ < 0) return false;

    size_t total_sent = 0;
    while (total_sent < msg.size()) {
        ssize_t n = ::send(client_fd_, msg.data() + total_sent,
                           msg.size() - total_sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total_sent += n;
    }
    return true;
}

bool FysetcTcpServer::recvMessage(fysetc::MsgHeader& hdr,
                                   std::vector<uint8_t>& payload,
                                   int timeout_ms) {
    std::lock_guard<std::mutex> lock(recv_mutex_);
    if (client_fd_ < 0) return false;

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    // Helper: recv exactly `len` bytes into `buf` respecting deadline
    auto recvExact = [&](uint8_t* buf, size_t len) -> bool {
        size_t got = 0;
        while (got < len) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;
            struct pollfd pfd = {client_fd_, POLLIN, 0};
            int ret = poll(&pfd, 1, static_cast<int>(std::min(remaining, (decltype(remaining))1000)));
            if (ret < 0) return false;
            if (ret == 0) continue;
            ssize_t n = ::recv(client_fd_, buf + got, len - got, 0);
            if (n <= 0) return false;
            got += n;
        }
        return true;
    };

    uint8_t hdr_buf[fysetc::MsgHeader::WIRE_SIZE];

    if (!recvExact(hdr_buf, fysetc::MsgHeader::WIRE_SIZE)) return false;

    while (true) {
        if (!fysetc::decodeHeader(hdr_buf, fysetc::MsgHeader::WIRE_SIZE, hdr)) return false;
        if (hdr.length < 4 || hdr.length > 65536) return false;

        uint32_t payload_size = hdr.length - 4;
        payload.resize(payload_size);

        if (payload_size > 0 && !recvExact(payload.data(), payload_size)) return false;

        // Consume async messages in-place — loop back for next header
        if (hdr.type == fysetc::MsgType::LOG) {
            processLog(payload);
            if (!recvExact(hdr_buf, fysetc::MsgHeader::WIRE_SIZE)) return false;
            continue;
        }
        if (hdr.type == fysetc::MsgType::STATUS) {
            processStatus(payload);
            if (!recvExact(hdr_buf, fysetc::MsgHeader::WIRE_SIZE)) return false;
            continue;
        }
        if (hdr.type == fysetc::MsgType::PONG) {
            processPong(payload);
        }
        return true;
    }
}

bool FysetcTcpServer::readSectors(const std::vector<fysetc::SectorRange>& ranges,
                                   std::vector<uint8_t>& out_data,
                                   std::vector<std::pair<uint32_t, uint16_t>>& out_delivered) {
    if (client_fd_ < 0 || ranges.empty()) return false;

    uint16_t req_id = next_req_id_++;
    auto req = fysetc::encodeSectorReadReq(req_id, ranges);
    if (!sendMessage(req)) return false;

    out_data.clear();
    out_delivered.clear();

    for (size_t i = 0; i < ranges.size(); ++i) {
        fysetc::MsgHeader hdr;
        std::vector<uint8_t> payload;

        if (!recvMessage(hdr, payload, 30000)) return false;

        if (hdr.type == fysetc::MsgType::SECTOR_READ_ERR) {
            std::cerr << "FysetcTcp: Sector read error for range " << i << std::endl;
            return false;  // fail entire request — caller retries next cycle
        }

        if (hdr.type != fysetc::MsgType::SECTOR_READ_RESP) return false;

        uint32_t sector_lba;
        uint16_t count;
        fysetc::SectorReadStatus status;
        const uint8_t* data_ptr;
        size_t data_len;

        if (!fysetc::decodeSectorReadResp(payload.data(), payload.size(),
                                           sector_lba, count, status,
                                           data_ptr, data_len)) {
            return false;
        }

        out_data.insert(out_data.end(), data_ptr, data_ptr + data_len);
        out_delivered.push_back({sector_lba, count});
    }

    return true;
}

bool FysetcTcpServer::ping(uint32_t nonce) {
    if (client_fd_ < 0) return false;

    uint16_t req_id = next_req_id_++;
    auto msg = fysetc::encodePing(req_id, nonce);
    if (!sendMessage(msg)) return false;

    fysetc::MsgHeader hdr;
    std::vector<uint8_t> payload;
    if (!recvMessage(hdr, payload, 5000)) return false;

    return hdr.type == fysetc::MsgType::PONG;
}

}  // namespace hms_cpap
