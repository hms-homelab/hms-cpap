#include <gtest/gtest.h>
#include "clients/O2RingClient.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "TestSocketCompat.h"
#ifndef _WIN32
#include <sys/select.h>
#endif

using namespace hms_cpap;

// ── LiveReading struct logic (no HTTP) ──────────────────────────────────

TEST(O2RingLiveReading, ActiveWithValidData) {
    O2RingClient::LiveReading r;
    r.spo2 = 96;
    r.hr = 72;
    r.motion = 3;
    r.vibration = 0;
    r.active = true;
    r.valid = r.active && r.spo2 > 0 && r.hr > 0;

    EXPECT_TRUE(r.active);
    EXPECT_TRUE(r.valid);
    EXPECT_EQ(r.spo2, 96);
    EXPECT_EQ(r.hr, 72);
}

TEST(O2RingLiveReading, InactiveWithRingOff) {
    // Mule responds: ring reachable but off finger
    O2RingClient::LiveReading r;
    r.spo2 = 255;
    r.hr = 255;
    r.motion = 0;
    r.vibration = 0;
    r.active = false;
    r.valid = r.active && r.spo2 > 0 && r.hr > 0;

    EXPECT_FALSE(r.active);
    EXPECT_FALSE(r.valid);
    // Reachable: spo2 != 0 (255 is the invalid marker, not zero)
    bool reachable = (r.spo2 != 0 || r.active);
    EXPECT_TRUE(reachable);
}

TEST(O2RingLiveReading, UnreachableMuleTimeout) {
    // Mule timed out — getLive() returns all zeros
    O2RingClient::LiveReading r;
    r.spo2 = 0;
    r.hr = 0;
    r.motion = 0;
    r.vibration = 0;
    r.active = false;
    r.valid = false;

    EXPECT_FALSE(r.active);
    EXPECT_FALSE(r.valid);
    bool reachable = (r.spo2 != 0 || r.active);
    EXPECT_FALSE(reachable);
}

TEST(O2RingLiveReading, ActiveButInvalidReadings) {
    // Ring on finger but sensor hasn't stabilized yet
    O2RingClient::LiveReading r;
    r.spo2 = 0;
    r.hr = 0;
    r.active = true;
    r.valid = r.active && r.spo2 > 0 && r.hr > 0;

    EXPECT_TRUE(r.active);
    EXPECT_FALSE(r.valid);
}

// ── Loopback HTTP fixture ───────────────────────────────────────────────
//
// Exercises the REAL O2RingClient JSON-decode paths (isConnected / listFiles
// / getLive / getBattery / downloadFile) by pointing the client's curl handle
// at a one-shot in-process HTTP server bound to 127.0.0.1. No real device,
// no external network, no wall-clock dependence — fully deterministic.

namespace {

// A single-connection HTTP server. Accepts one request, replies with the
// canned body (and optional status line), then closes. Runs in a thread so
// curl_easy_perform() in the client can complete the round-trip.
class OneShotHttpServer {
public:
    explicit OneShotHttpServer(std::string body,
                               std::string status = "200 OK",
                               std::string content_type = "application/json")
        : body_(std::move(body)),
          status_(std::move(status)),
          content_type_(std::move(content_type)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(listen_fd_, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ask OS for an ephemeral port
        EXPECT_EQ(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(listen_fd_, 1), 0);

        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this] { serve(); });
    }

    ~OneShotHttpServer() {
        stopListening(listen_fd_);
        if (thread_.joinable()) thread_.join();
        if (listen_fd_ >= 0) closeSocket(listen_fd_);
    }

    std::string baseUrl() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

private:
    void serve() {
        int conn = ::accept(listen_fd_, nullptr, nullptr);
        if (conn < 0) return;

        // Drain the request (best effort — we don't parse it).
        char buf[2048];
        ::recv(conn, buf, sizeof(buf), 0);

        std::string resp =
            "HTTP/1.1 " + status_ + "\r\n"
            "Content-Type: " + content_type_ + "\r\n"
            "Content-Length: " + std::to_string(body_.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body_;

        size_t sent = 0;
        while (sent < resp.size()) {
            ssize_t n = ::send(conn, resp.data() + sent, static_cast<int>(resp.size() - sent), 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        closeSocket(conn);
    }

    std::string body_;
    std::string status_;
    std::string content_type_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
};

}  // namespace

// ── isConnected ─────────────────────────────────────────────────────────

TEST(O2RingHttp, IsConnectedTrueAndCachesBattery) {
    OneShotHttpServer srv(R"({"connected":true,"model":"1652","battery":74})");
    O2RingClient client(srv.baseUrl());

    EXPECT_TRUE(client.isConnected());
    EXPECT_EQ(client.getBattery(), 74);
}

TEST(O2RingHttp, IsConnectedFalseField) {
    OneShotHttpServer srv(R"({"connected":false,"battery":12})");
    O2RingClient client(srv.baseUrl());

    EXPECT_FALSE(client.isConnected());
    EXPECT_EQ(client.getBattery(), 12);
}

TEST(O2RingHttp, IsConnectedMissingFieldDefaultsFalse) {
    // No "connected" key → value(...,false) default
    OneShotHttpServer srv(R"({"battery":50})");
    O2RingClient client(srv.baseUrl());

    EXPECT_FALSE(client.isConnected());
    EXPECT_EQ(client.getBattery(), 50);
}

TEST(O2RingHttp, IsConnectedIgnoresNonNumericBattery) {
    // battery present but not a number → cached battery stays at default -1
    OneShotHttpServer srv(R"({"connected":true,"battery":"unknown"})");
    O2RingClient client(srv.baseUrl());

    EXPECT_TRUE(client.isConnected());
    EXPECT_EQ(client.getBattery(), -1);
}

TEST(O2RingHttp, IsConnectedHandlesMalformedJson) {
    OneShotHttpServer srv("this-is-not-json{{{");
    O2RingClient client(srv.baseUrl());

    // Parse throws → caught → returns false, battery untouched (-1)
    EXPECT_FALSE(client.isConnected());
    EXPECT_EQ(client.getBattery(), -1);
}

TEST(O2RingHttp, BatteryDefaultBeforeAnyCall) {
    O2RingClient client("http://127.0.0.1:1");  // never queried
    EXPECT_EQ(client.getBattery(), -1);
}

// ── listFiles ───────────────────────────────────────────────────────────

TEST(O2RingHttp, ListFilesReturnsNamesAndCachesBattery) {
    OneShotHttpServer srv(
        R"({"files":["20260412065307.vld","20260413071122.vld"],"battery":88})");
    O2RingClient client(srv.baseUrl());

    auto files = client.listFiles();
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0], "20260412065307.vld");
    EXPECT_EQ(files[1], "20260413071122.vld");
    EXPECT_EQ(client.getBattery(), 88);
}

TEST(O2RingHttp, ListFilesEmptyArray) {
    OneShotHttpServer srv(R"({"files":[],"battery":5})");
    O2RingClient client(srv.baseUrl());

    EXPECT_TRUE(client.listFiles().empty());
    EXPECT_EQ(client.getBattery(), 5);
}

TEST(O2RingHttp, ListFilesSkipsNonStringEntries) {
    // Mixed array — only string entries are kept
    OneShotHttpServer srv(R"({"files":["a.vld",42,null,"b.vld"]})");
    O2RingClient client(srv.baseUrl());

    auto files = client.listFiles();
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0], "a.vld");
    EXPECT_EQ(files[1], "b.vld");
}

TEST(O2RingHttp, ListFilesMissingFilesKey) {
    OneShotHttpServer srv(R"({"battery":33})");
    O2RingClient client(srv.baseUrl());

    EXPECT_TRUE(client.listFiles().empty());
    EXPECT_EQ(client.getBattery(), 33);
}

TEST(O2RingHttp, ListFilesMalformedJsonReturnsEmpty) {
    OneShotHttpServer srv("<<broken>>");
    O2RingClient client(srv.baseUrl());

    EXPECT_TRUE(client.listFiles().empty());
}

// ── getLive ─────────────────────────────────────────────────────────────

TEST(O2RingHttp, GetLiveActiveValid) {
    OneShotHttpServer srv(
        R"({"spo2":97,"hr":68,"motion":4,"vibration":1,"active":true})");
    O2RingClient client(srv.baseUrl());

    auto r = client.getLive();
    EXPECT_EQ(r.spo2, 97);
    EXPECT_EQ(r.hr, 68);
    EXPECT_EQ(r.motion, 4);
    EXPECT_EQ(r.vibration, 1);
    EXPECT_TRUE(r.active);
    EXPECT_TRUE(r.valid);
}

TEST(O2RingHttp, GetLiveActiveButZeroReadingsInvalid) {
    OneShotHttpServer srv(R"({"spo2":0,"hr":0,"active":true})");
    O2RingClient client(srv.baseUrl());

    auto r = client.getLive();
    EXPECT_TRUE(r.active);
    EXPECT_FALSE(r.valid);  // valid requires spo2>0 && hr>0
}

TEST(O2RingHttp, GetLiveInactiveIsInvalid) {
    OneShotHttpServer srv(R"({"spo2":96,"hr":70,"active":false})");
    O2RingClient client(srv.baseUrl());

    auto r = client.getLive();
    EXPECT_EQ(r.spo2, 96);
    EXPECT_EQ(r.hr, 70);
    EXPECT_FALSE(r.active);
    EXPECT_FALSE(r.valid);  // valid requires active
}

TEST(O2RingHttp, GetLiveMissingFieldsUseDefaults) {
    OneShotHttpServer srv(R"({})");
    O2RingClient client(srv.baseUrl());

    auto r = client.getLive();
    EXPECT_EQ(r.spo2, 0);
    EXPECT_EQ(r.hr, 0);
    EXPECT_EQ(r.motion, 0);
    EXPECT_EQ(r.vibration, 0);
    EXPECT_FALSE(r.active);
    EXPECT_FALSE(r.valid);
}

TEST(O2RingHttp, GetLiveMalformedJsonReturnsDefault) {
    OneShotHttpServer srv("not json");
    O2RingClient client(srv.baseUrl());

    auto r = client.getLive();
    EXPECT_EQ(r.spo2, 0);
    EXPECT_EQ(r.hr, 0);
    EXPECT_FALSE(r.valid);
}

// ── downloadFile ────────────────────────────────────────────────────────

TEST(O2RingHttp, DownloadFileReturnsBinaryBytes) {
    // Raw .vld payload including embedded NUL and high bytes
    std::string body;
    body.push_back('\x01');
    body.push_back('\x00');
    body.push_back('\xFF');
    body.push_back('\x7E');
    OneShotHttpServer srv(body, "200 OK", "application/octet-stream");
    O2RingClient client(srv.baseUrl());

    auto data = client.downloadFile("20260412065307.vld");
    ASSERT_EQ(data.size(), 4u);
    EXPECT_EQ(data[0], 0x01);
    EXPECT_EQ(data[1], 0x00);
    EXPECT_EQ(data[2], 0xFF);
    EXPECT_EQ(data[3], 0x7E);
}

TEST(O2RingHttp, DownloadFileNon200ReturnsEmpty) {
    OneShotHttpServer srv("nope", "404 Not Found");
    O2RingClient client(srv.baseUrl());

    EXPECT_TRUE(client.downloadFile("missing.vld").empty());
}

TEST(O2RingHttp, DownloadFileEmptyBodyReturnsEmpty) {
    OneShotHttpServer srv("", "200 OK", "application/octet-stream");
    O2RingClient client(srv.baseUrl());

    EXPECT_TRUE(client.downloadFile("empty.vld").empty());
}

TEST(O2RingHttp, DownloadFileUrlEncodesFilename) {
    // Filename with spaces/special chars must be accepted (curl_easy_escape).
    // Server replies with a fixed body regardless of query.
    std::string body = "OK";
    OneShotHttpServer srv(body, "200 OK", "application/octet-stream");
    O2RingClient client(srv.baseUrl());

    auto data = client.downloadFile("weird name &.vld");
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0], 'O');
    EXPECT_EQ(data[1], 'K');
}

// ── getBaseURL passthrough ──────────────────────────────────────────────

TEST(O2RingHttp, GetBaseUrlReturnsConfigured) {
    O2RingClient client("http://192.168.1.9");
    EXPECT_EQ(client.getBaseURL(), "http://192.168.1.9");
}
// ── #135: one request at a time, and a failure never sticks ─────────────
//
// In http mode the collect endpoint's download (a drogon thread) and the burst
// worker's live poll shared the client's one curl handle at the same time. The
// poll swapped the write target out from under the download, the append threw
// std::bad_alloc through libcurl, and the handle then failed every request with
// "Failed initialization" until a restart.

namespace {

// A loopback mule: any number of connections, each on its own thread.
// /o2ring/files?name=... streams kDownloadSize bytes slowly (about a second),
// everything else answers at once with a live reading.
class SlowMuleServer {
public:
    static constexpr size_t kChunk = 64;
    static constexpr size_t kChunks = 64;
    static constexpr size_t kDownloadSize = kChunk * kChunks;

    static uint8_t byteAt(size_t i) { return static_cast<uint8_t>((i * 7 + 3) & 0xFF); }

    SlowMuleServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GE(listen_fd_, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(listen_fd_, 16), 0);
        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);
        accept_thread_ = std::thread([this] { acceptLoop(); });
    }

    ~SlowMuleServer() {
        stopping_ = true;
        if (accept_thread_.joinable()) accept_thread_.join();
        if (listen_fd_ >= 0) closeSocket(listen_fd_);
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& t : conns_) if (t.joinable()) t.join();
    }

    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

private:
    // Waits in select() with a timeout, not in accept(): a shutdown() of the
    // listening socket does not wake accept() on macOS.
    void acceptLoop() {
        const int fd = listen_fd_;
        while (!stopping_) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(fd, &rd);
            timeval tv{0, 50 * 1000};
            const int ready = ::select(fd + 1, &rd, nullptr, nullptr, &tv);
            if (ready < 0) return;
            if (ready == 0) continue;
            int conn = ::accept(fd, nullptr, nullptr);
            if (conn < 0) return;
            std::lock_guard<std::mutex> lk(mtx_);
            conns_.emplace_back([this, conn] { serve(conn); });
        }
    }

    static void sendAll(int conn, const char* p, size_t n) {
        size_t sent = 0;
        while (sent < n) {
            ssize_t k = ::send(conn, p + sent, static_cast<int>(n - sent), 0);
            if (k <= 0) return;
            sent += static_cast<size_t>(k);
        }
    }

    void serve(int conn) {
        std::string req;
        char buf[1024];
        while (req.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) { closeSocket(conn); return; }
            req.append(buf, static_cast<size_t>(n));
        }
        if (req.find("/o2ring/files?name=") != std::string::npos) {
            std::string head =
                "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                "Content-Length: " + std::to_string(kDownloadSize) +
                "\r\nConnection: close\r\n\r\n";
            sendAll(conn, head.data(), head.size());
            std::string chunk(kChunk, '\0');
            for (size_t c = 0; c < kChunks && !stopping_; ++c) {
                for (size_t i = 0; i < kChunk; ++i)
                    chunk[i] = static_cast<char>(byteAt(c * kChunk + i));
                sendAll(conn, chunk.data(), chunk.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        } else {
            const std::string body =
                R"({"spo2":97,"hr":68,"motion":1,"vibration":0,"active":true,"connected":true,"battery":61})";
            std::string resp =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            sendAll(conn, resp.data(), resp.size());
        }
        closeSocket(conn);
    }

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
    std::mutex mtx_;
    std::vector<std::thread> conns_;
};

std::vector<uint8_t> expectedDownload() {
    std::vector<uint8_t> v(SlowMuleServer::kDownloadSize);
    for (size_t i = 0; i < v.size(); ++i) v[i] = SlowMuleServer::byteAt(i);
    return v;
}

// A body chunk that cannot be kept: the first [throws] chunks throw, as the
// append did in #135.
class ThrowingO2RingClient : public O2RingClient {
public:
    using O2RingClient::O2RingClient;
    std::atomic<int> throws{0};
protected:
    void onBodyChunk(size_t) override {
        if (throws.load() > 0) { --throws; throw std::bad_alloc(); }
    }
};

}  // namespace

// The collect endpoint downloads while the burst worker polls live: both get
// their own whole answer, and the client still works afterwards.
TEST(O2RingHttp, LivePollDuringADownloadTakesItsTurn) {
    SlowMuleServer srv;
    O2RingClient client(srv.baseUrl());

    std::vector<uint8_t> downloaded;
    std::atomic<bool> download_done{false};
    std::thread downloader([&] {
        downloaded = client.downloadFile("20261128005300.vld");
        download_done = true;
    });

    // The burst worker's polls, landing inside the download.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int polls = 0, good = 0;
    for (int i = 0; i < 5; ++i) {
        auto r = client.getLive();
        ++polls;
        if (r.valid && r.spo2 == 97 && r.hr == 68) ++good;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    downloader.join();

    EXPECT_TRUE(download_done.load());
    EXPECT_EQ(downloaded, expectedDownload()) << "the download got the live poll's bytes, or was cut";
    EXPECT_EQ(good, polls) << "a live poll got the download's bytes";

    // And the handle is not wedged.
    EXPECT_EQ(client.downloadFile("again.vld"), expectedDownload());
    EXPECT_TRUE(client.getLive().valid);
}

// A write callback that throws aborts that one download; the next request
// works, not "Failed initialization" until a restart.
TEST(O2RingHttp, ThrowingDownloadCallbackDoesNotWedgeTheClient) {
    SlowMuleServer srv;
    ThrowingO2RingClient client(srv.baseUrl());

    client.throws = 1;
    EXPECT_TRUE(client.downloadFile("20261128005300.vld").empty());
    EXPECT_EQ(client.throws.load(), 0);

    EXPECT_EQ(client.downloadFile("20261128005300.vld"), expectedDownload());
    auto r = client.getLive();
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(client.isConnected());
    EXPECT_EQ(client.getBattery(), 61);
}

TEST(O2RingHttp, ThrowingTextCallbackDoesNotWedgeTheClient) {
    SlowMuleServer srv;
    ThrowingO2RingClient client(srv.baseUrl());

    client.throws = 1;
    EXPECT_FALSE(client.getLive().valid);
    EXPECT_EQ(client.throws.load(), 0);

    EXPECT_TRUE(client.getLive().valid);
    EXPECT_EQ(client.downloadFile("20261128005300.vld"), expectedDownload());
}
