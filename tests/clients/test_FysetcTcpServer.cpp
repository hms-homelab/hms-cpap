#include <gtest/gtest.h>
#include "clients/FysetcTcpServer.h"
#include "clients/FysetcProtocol.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>

using namespace hms_cpap;
using namespace hms_cpap::fysetc;

// Helper: connect a raw TCP client to localhost:port
static int connectTo(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Helper: send raw bytes over fd
static bool sendAll(int fd, const std::vector<uint8_t>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// Helper: recv exact bytes with timeout
static bool recvAll(int fd, std::vector<uint8_t>& out, size_t count, int timeout_ms = 5000) {
    out.resize(count);
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (got < count) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        struct pollfd pfd = {fd, POLLIN, 0};
        int ret = poll(&pfd, 1, static_cast<int>(std::min(remaining, (decltype(remaining))1000)));
        if (ret < 0) return false;
        if (ret == 0) continue;
        ssize_t n = ::recv(fd, out.data() + got, count - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

// Helper: build a HELLO message
static std::vector<uint8_t> makeHello(const char* serial, uint16_t fw_ver,
                                       uint32_t boot_count, uint16_t req_id = 1) {
    auto buf = encodeHeader(MsgType::HELLO, 0, req_id, HelloPayload::WIRE_SIZE);
    buf.resize(MsgHeader::WIRE_SIZE + HelloPayload::WIRE_SIZE, 0);
    size_t off = MsgHeader::WIRE_SIZE;
    std::memset(&buf[off], 0, 16);
    std::strncpy(reinterpret_cast<char*>(&buf[off]), serial, 16);
    std::memcpy(&buf[off + 16], &fw_ver, 2);
    std::memcpy(&buf[off + 18], &boot_count, 4);
    uint32_t uptime = 5000;
    std::memcpy(&buf[off + 22], &uptime, 4);
    uint32_t heap = 100000;
    std::memcpy(&buf[off + 26], &heap, 4);
    return buf;
}

// Helper: build a SECTOR_READ_RESP message
static std::vector<uint8_t> makeSectorResp(uint16_t req_id, uint32_t lba,
                                            uint16_t count, SectorReadStatus status,
                                            const std::vector<uint8_t>& data) {
    uint32_t payload_size = 7 + data.size();
    auto buf = encodeHeader(MsgType::SECTOR_READ_RESP, 0, req_id, payload_size);
    buf.resize(MsgHeader::WIRE_SIZE + payload_size);
    size_t off = MsgHeader::WIRE_SIZE;
    std::memcpy(&buf[off], &lba, 4);
    std::memcpy(&buf[off + 4], &count, 2);
    buf[off + 6] = static_cast<uint8_t>(status);
    if (!data.empty()) {
        std::memcpy(&buf[off + 7], data.data(), data.size());
    }
    return buf;
}

// Helper: build a PONG message
static std::vector<uint8_t> makePong(uint16_t req_id, uint32_t nonce) {
    uint32_t payload_size = 4 + StatusPayload::WIRE_SIZE;
    auto buf = encodeHeader(MsgType::PONG, 0, req_id, payload_size);
    buf.resize(MsgHeader::WIRE_SIZE + payload_size, 0);
    std::memcpy(&buf[MsgHeader::WIRE_SIZE], &nonce, 4);
    return buf;
}

static std::atomic<int> s_next_port{29876};

class FysetcTcpServerTest : public ::testing::Test {
protected:
    int test_port_ = 0;

    void SetUp() override {
        test_port_ = s_next_port.fetch_add(1);
        server = std::make_unique<FysetcTcpServer>(test_port_, "127.0.0.1");
        server->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        server->stop();
        server.reset();
    }

    int connectAndHandshake(uint32_t boot_count = 1) {
        int fd = connectTo(test_port_);
        if (fd < 0) return -1;

        auto hello = makeHello("TEST_DEVICE", 0x0100, boot_count);
        if (!sendAll(fd, hello)) { close(fd); return -1; }

        // Receive HELLO_ACK
        std::vector<uint8_t> ack_buf;
        if (!recvAll(fd, ack_buf, MsgHeader::WIRE_SIZE + HelloAckPayload::WIRE_SIZE)) {
            close(fd);
            return -1;
        }

        return fd;
    }

    std::unique_ptr<FysetcTcpServer> server;
};

TEST_F(FysetcTcpServerTest, AcceptsConnection) {
    int fd = connectAndHandshake();
    ASSERT_GE(fd, 0);

    EXPECT_TRUE(server->isConnected());
    EXPECT_EQ(server->deviceState().device_serial, "TEST_DEVICE");
    EXPECT_EQ(server->deviceState().fw_version, 0x0100);
    EXPECT_EQ(server->deviceState().boot_count, 1u);
    EXPECT_EQ(server->deviceState().session_id, 1u);

    close(fd);
}

TEST_F(FysetcTcpServerTest, DetectsReboot) {
    // First connection: needs_full_sync = true (no prior boot_count known)
    int fd1 = connectAndHandshake(1);
    ASSERT_GE(fd1, 0);
    EXPECT_TRUE(server->deviceState().needs_full_sync);
    close(fd1);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Reconnect with SAME boot_count: no reboot detected
    int fd2 = connectAndHandshake(1);
    ASSERT_GE(fd2, 0);
    EXPECT_FALSE(server->deviceState().needs_full_sync);
    close(fd2);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Reconnect with DIFFERENT boot_count: reboot detected
    int fd3 = connectAndHandshake(2);
    ASSERT_GE(fd3, 0);
    EXPECT_TRUE(server->deviceState().needs_full_sync);
    EXPECT_EQ(server->deviceState().boot_count, 2u);
    close(fd3);
}

TEST_F(FysetcTcpServerTest, RejectsNonHello) {
    int fd = connectTo(test_port_);
    ASSERT_GE(fd, 0);

    auto ping = encodePing(1, 42);
    sendAll(fd, ping);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(server->isConnected());
    close(fd);
}

TEST_F(FysetcTcpServerTest, PingPong) {
    int fd = connectAndHandshake();
    ASSERT_GE(fd, 0);

    // Server sends PING, we need to respond with PONG
    // But ping() is synchronous — we need a background thread to respond
    std::thread responder([&]() {
        std::vector<uint8_t> ping_buf;
        if (!recvAll(fd, ping_buf, MsgHeader::WIRE_SIZE + 4, 5000)) return;

        MsgHeader hdr;
        decodeHeader(ping_buf.data(), MsgHeader::WIRE_SIZE, hdr);
        uint32_t nonce;
        std::memcpy(&nonce, &ping_buf[MsgHeader::WIRE_SIZE], 4);

        auto pong = makePong(hdr.req_id, nonce);
        sendAll(fd, pong);
    });

    bool result = server->ping(42);
    responder.join();
    EXPECT_TRUE(result);

    close(fd);
}

TEST_F(FysetcTcpServerTest, ReadSectors) {
    int fd = connectAndHandshake();
    ASSERT_GE(fd, 0);

    // Server will call readSectors() which sends SECTOR_READ_REQ
    // We respond with SECTOR_READ_RESP in a background thread
    std::vector<uint8_t> fake_sector(512, 0xAB);

    std::thread responder([&]() {
        // Receive SECTOR_READ_REQ
        std::vector<uint8_t> req_buf;
        if (!recvAll(fd, req_buf, MsgHeader::WIRE_SIZE, 5000)) return;

        MsgHeader hdr;
        decodeHeader(req_buf.data(), MsgHeader::WIRE_SIZE, hdr);
        uint32_t payload_size = hdr.length - 4;

        std::vector<uint8_t> payload;
        if (!recvAll(fd, payload, payload_size, 5000)) return;

        std::vector<SectorRange> ranges;
        decodeSectorReadReq(payload.data(), payload.size(), ranges);

        // Respond with one sector of data per range
        for (auto& r : ranges) {
            std::vector<uint8_t> data(r.count * 512, 0xAB);
            auto resp = makeSectorResp(hdr.req_id, r.sector_lba, r.count,
                                       SectorReadStatus::OK, data);
            sendAll(fd, resp);
        }
    });

    std::vector<SectorRange> request_ranges = {{100, 1}, {200, 2}};
    std::vector<uint8_t> out_data;
    std::vector<std::pair<uint32_t, uint16_t>> out_delivered;

    bool result = server->readSectors(request_ranges, out_data, out_delivered);
    responder.join();

    EXPECT_TRUE(result);
    ASSERT_EQ(out_delivered.size(), 2u);
    EXPECT_EQ(out_delivered[0].first, 100u);
    EXPECT_EQ(out_delivered[0].second, 1u);
    EXPECT_EQ(out_delivered[1].first, 200u);
    EXPECT_EQ(out_delivered[1].second, 2u);

    // 1 + 2 = 3 sectors = 1536 bytes, all 0xAB
    EXPECT_EQ(out_data.size(), 3u * 512u);
    EXPECT_EQ(out_data[0], 0xAB);
    EXPECT_EQ(out_data[1535], 0xAB);

    close(fd);
}

// --- Protocol codec unit tests (no TCP needed) ---

TEST(FysetcProtocolTest, HeaderRoundTrip) {
    auto encoded = encodeHeader(MsgType::SECTOR_READ_REQ, 0, 42, 100);
    ASSERT_EQ(encoded.size(), MsgHeader::WIRE_SIZE);

    MsgHeader hdr;
    ASSERT_TRUE(decodeHeader(encoded.data(), encoded.size(), hdr));
    EXPECT_EQ(hdr.type, MsgType::SECTOR_READ_REQ);
    EXPECT_EQ(hdr.req_id, 42u);
    EXPECT_EQ(hdr.length, 104u);  // 4 + 100
}

TEST(FysetcProtocolTest, SectorReadReqRoundTrip) {
    std::vector<SectorRange> ranges = {{100, 8}, {500, 16}, {1000, 1}};
    auto encoded = encodeSectorReadReq(7, ranges);

    MsgHeader hdr;
    ASSERT_TRUE(decodeHeader(encoded.data(), encoded.size(), hdr));
    EXPECT_EQ(hdr.type, MsgType::SECTOR_READ_REQ);
    EXPECT_EQ(hdr.req_id, 7u);

    std::vector<SectorRange> decoded;
    ASSERT_TRUE(decodeSectorReadReq(encoded.data() + MsgHeader::WIRE_SIZE,
                                     encoded.size() - MsgHeader::WIRE_SIZE, decoded));
    ASSERT_EQ(decoded.size(), 3u);
    EXPECT_EQ(decoded[0].sector_lba, 100u);
    EXPECT_EQ(decoded[0].count, 8u);
    EXPECT_EQ(decoded[1].sector_lba, 500u);
    EXPECT_EQ(decoded[1].count, 16u);
    EXPECT_EQ(decoded[2].sector_lba, 1000u);
    EXPECT_EQ(decoded[2].count, 1u);
}

TEST(FysetcProtocolTest, HelloAckRoundTrip) {
    auto encoded = encodeHelloAck(3, 1713500000ULL, 42);

    MsgHeader hdr;
    ASSERT_TRUE(decodeHeader(encoded.data(), encoded.size(), hdr));
    EXPECT_EQ(hdr.type, MsgType::HELLO_ACK);
    EXPECT_EQ(hdr.req_id, 3u);

    uint64_t time;
    uint32_t sid;
    std::memcpy(&time, &encoded[MsgHeader::WIRE_SIZE], 8);
    std::memcpy(&sid, &encoded[MsgHeader::WIRE_SIZE + 8], 4);
    EXPECT_EQ(time, 1713500000ULL);
    EXPECT_EQ(sid, 42u);
}

TEST(FysetcProtocolTest, PingEncoding) {
    auto encoded = encodePing(10, 0xDEADBEEF);

    MsgHeader hdr;
    ASSERT_TRUE(decodeHeader(encoded.data(), encoded.size(), hdr));
    EXPECT_EQ(hdr.type, MsgType::PING);

    uint32_t nonce;
    std::memcpy(&nonce, &encoded[MsgHeader::WIRE_SIZE], 4);
    EXPECT_EQ(nonce, 0xDEADBEEF);
}

TEST_F(FysetcTcpServerTest, LogFloodBeforeResp) {
    // Verify the server handles many LOG messages before a SECTOR_READ_RESP
    // without stack overflow (C1 fix: while loop instead of recursion)
    int fd = connectAndHandshake();
    ASSERT_GE(fd, 0);

    int logs_received = 0;
    server->setLogCallback([&](fysetc::LogLevel, const std::string&, const std::string&) {
        logs_received++;
    });

    std::thread responder([&]() {
        // Receive SECTOR_READ_REQ
        std::vector<uint8_t> req_hdr_buf;
        if (!recvAll(fd, req_hdr_buf, MsgHeader::WIRE_SIZE)) return;
        MsgHeader hdr;
        decodeHeader(req_hdr_buf.data(), MsgHeader::WIRE_SIZE, hdr);
        uint32_t plen = hdr.length - 4;
        std::vector<uint8_t> req_payload;
        if (plen > 0) recvAll(fd, req_payload, plen);

        // Send 30 LOG messages before the RESP
        for (int i = 0; i < 30; i++) {
            uint8_t log_payload[20] = {3, 0, 0, 0, 0, 'T', 'S', 'T', 0, 0, 0, 0, 0, 'h', 'i', 0};
            auto log_msg = encodeHeader(MsgType::LOG, 0, 0, 16);
            log_msg.resize(MsgHeader::WIRE_SIZE + 16);
            memcpy(&log_msg[MsgHeader::WIRE_SIZE], log_payload, 16);
            sendAll(fd, log_msg);
        }

        // Now send the actual RESP
        std::vector<uint8_t> data(512, 0xCC);
        auto resp = makeSectorResp(hdr.req_id, 0, 1, SectorReadStatus::OK, data);
        sendAll(fd, resp);
    });

    std::vector<SectorRange> ranges = {{0, 1}};
    std::vector<uint8_t> out_data;
    std::vector<std::pair<uint32_t, uint16_t>> out_delivered;

    bool result = server->readSectors(ranges, out_data, out_delivered);
    responder.join();

    EXPECT_TRUE(result);
    EXPECT_EQ(logs_received, 30);
    EXPECT_EQ(out_data.size(), 512u);
    EXPECT_EQ(out_data[0], 0xCC);

    close(fd);
}

TEST_F(FysetcTcpServerTest, SectorReadErrFailsRequest) {
    // Verify that SECTOR_READ_ERR causes readSectors to return false (C3 fix)
    int fd = connectAndHandshake();
    ASSERT_GE(fd, 0);

    std::thread responder([&]() {
        std::vector<uint8_t> req_hdr_buf;
        if (!recvAll(fd, req_hdr_buf, MsgHeader::WIRE_SIZE)) return;
        MsgHeader hdr;
        decodeHeader(req_hdr_buf.data(), MsgHeader::WIRE_SIZE, hdr);
        uint32_t plen = hdr.length - 4;
        std::vector<uint8_t> req_payload;
        if (plen > 0) recvAll(fd, req_payload, plen);

        // Send ERR instead of RESP
        uint8_t err_payload[] = {0x03, 0, 0, 0};  // BUS_BUSY
        auto err = encodeHeader(MsgType::SECTOR_READ_ERR, 0, hdr.req_id, 4);
        err.resize(MsgHeader::WIRE_SIZE + 4);
        memcpy(&err[MsgHeader::WIRE_SIZE], err_payload, 4);
        sendAll(fd, err);
    });

    std::vector<SectorRange> ranges = {{100, 1}};
    std::vector<uint8_t> out_data;
    std::vector<std::pair<uint32_t, uint16_t>> out_delivered;

    bool result = server->readSectors(ranges, out_data, out_delivered);
    responder.join();

    EXPECT_FALSE(result);
    EXPECT_TRUE(out_data.empty());

    close(fd);
}

TEST_F(FysetcTcpServerTest, MalformedHeaderLengthUnderflow) {
    // Verify that hdr.length < 4 doesn't cause underflow crash (M5 fix)
    int fd = connectAndHandshake();
    ASSERT_GE(fd, 0);

    // Send a message with length = 2 (less than minimum 4)
    uint8_t bad_msg[8] = {0};
    uint32_t bad_length = 2;
    memcpy(bad_msg, &bad_length, 4);
    bad_msg[4] = static_cast<uint8_t>(MsgType::PING);
    sendAll(fd, std::vector<uint8_t>(bad_msg, bad_msg + 8));

    // Server should disconnect, not crash
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // The next ping should fail because server dropped the connection
    bool result = server->ping(42);
    EXPECT_FALSE(result);

    close(fd);
}

TEST(FysetcProtocolTest, ByeEncoding) {
    auto encoded = encodeBye(1, ByeReason::REBOOT);
    MsgHeader hdr;
    ASSERT_TRUE(decodeHeader(encoded.data(), encoded.size(), hdr));
    EXPECT_EQ(hdr.type, MsgType::BYE);
    EXPECT_EQ(encoded[MsgHeader::WIRE_SIZE], 0x01);
}
