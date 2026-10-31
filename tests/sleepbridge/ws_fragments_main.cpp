// A fragmented text message on the device routes is answered.
//
// An ESP-IDF WebSocket client sends any message larger than its buffer as
// fragments (a text frame with FIN clear, continuation frames, the last with
// FIN set), all in one write, then waits for the answer. Drogon's reader
// stopped at the first fragment and left the rest in the buffer until more
// bytes arrived, so a Sleep Bridge's hello went unanswered and its switch to
// this receiver timed out (third_party/drogon/ carries the fix).
//
// This runs the real routes on a loopback port and, over a raw socket, sends
// a hello in one frame and in fragments; each must get {"t":"ack"} within
// 3 s. Its own binary, not run_tests: it needs Drogon's app running, and that
// is one per process.

#include "SleepBridgeTestUtil.h"

#include "controllers/MuleSocket.h"
#include "controllers/SleepBridgeSocket.h"
#include "mule/MuleService.h"
#include "services/As11NightWriter.h"
#include "sleepbridge/SleepBridgeService.h"

#include <drogon/drogon.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace sbtest;

namespace {

// Every device route on the stream listener. A route added beside the Sleep
// Bridge's (another device kind) goes here too.
const std::vector<std::string> kRoutes = {kRoute, hms_cpap::mule::kRoute};

int connectTo(int port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return -1;
    }
    timeval tv{3, 0};   // the answer is due well inside this
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

bool sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

/// One client frame (clients must mask).
std::string wsFrame(uint8_t opcode, bool fin, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
    const size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(0x80 | n));
    } else {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>(n & 0xFF));
    }
    const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    f.append(reinterpret_cast<const char*>(mask), 4);
    for (size_t i = 0; i < n; i++) f.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    return f;
}

/// The upgrade, answered 101; false otherwise.
bool upgrade(int fd, const std::string& route) {
    const std::string req = "GET " + route + " HTTP/1.1\r\n"
                            "Host: 127.0.0.1\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "X-Coyote-Sender: TAP-S3-00000123\r\n"
                            "X-Coyote-Machine: airsense-bench\r\n"
                            "X-Device-Serial: FE-C3-00000123\r\n"   // the Mule & Miner's route
                            "\r\n";
    if (!sendAll(fd, req)) return false;
    std::string head;
    char c;
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (::recv(fd, &c, 1, 0) != 1) return false;
        head.push_back(c);
    }
    return head.rfind("HTTP/1.1 101", 0) == 0;
}

/// The first text frame the server sends, or "" on a timeout.
std::string readText(int fd) {
    auto take = [fd](size_t n) {
        std::string s(n, '\0');
        size_t got = 0;
        while (got < n) {
            const ssize_t r = ::recv(fd, &s[got], n - got, 0);
            if (r <= 0) return std::string();
            got += static_cast<size_t>(r);
        }
        return s;
    };
    for (;;) {
        const std::string h = take(2);
        if (h.empty()) return "";
        size_t n = static_cast<uint8_t>(h[1]) & 0x7F;
        if (n == 126) {
            const std::string l = take(2);
            if (l.empty()) return "";
            n = (static_cast<uint8_t>(l[0]) << 8) | static_cast<uint8_t>(l[1]);
        } else if (n == 127) {
            return "";
        }
        const std::string body = n ? take(n) : std::string();
        if ((static_cast<uint8_t>(h[0]) & 0x0F) == 0x1) return body;   // text; skip pings
    }
}

/// A hello of about `size` bytes (a test hello, so the service keeps nothing).
std::string hello(size_t size) {
    std::string pad(size > 80 ? size - 80 : 0, 'x');
    // state.probe is the Sleep Bridge's test hello, test the Mule & Miner's.
    return R"({"t":"hello","test":true,"state":{"probe":true,"fw_version":"1.0.0","pad":")" + pad + R"("}})";
}

bool check(int port, const std::string& route, size_t size, size_t frag) {
    const int fd = connectTo(port);
    if (fd < 0) {
        std::printf("FAIL %s: no connection\n", route.c_str());
        return false;
    }
    bool ok = upgrade(fd, route);
    const std::string msg = hello(size);
    std::string wire;
    if (frag == 0 || msg.size() <= frag) {
        wire = wsFrame(0x1, true, msg);
    } else {
        for (size_t off = 0; off < msg.size(); off += frag) {
            const bool first = off == 0, last = off + frag >= msg.size();
            wire += wsFrame(first ? 0x1 : 0x0, last, msg.substr(off, frag));
        }
    }
    ok = ok && sendAll(fd, wire);   // one write, as the bridge's client does
    const auto t0     = std::chrono::steady_clock::now();
    const std::string got = ok ? readText(fd) : std::string();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // An ack, whatever else it carries (the Mule & Miner's has the time and
    // the ping period).
    ok = ok && got.find(R"("t":"ack")") != std::string::npos;
    std::printf("%s %s: %zu B hello %s -> %s (%.0f ms)\n", ok ? "ok  " : "FAIL", route.c_str(), msg.size(),
                frag ? ("in " + std::to_string(frag) + " B fragments").c_str() : "in one frame",
                got.empty() ? "no answer" : got.c_str(), ms);
    ::close(fd);
    return ok;
}

}  // namespace

int main() {
    const int port = 30000 + static_cast<int>(::getpid() % 20000);
    TempDir   t;
    SleepBridgeService::Options opt;
    opt.root = t.path.string();
    opt.port = port;
    SleepBridgeService svc(opt, fakeFactory(std::make_shared<FakeLog>()));
    hms_cpap::sleepbridge::installRoutes(&svc);
    hms_cpap::mule::MuleService::Options mo;
    mo.root     = (t.path / "mule").string();
    mo.data_dir = t.path.string();
    mo.port     = port;
    hms_cpap::mule::MuleService mule(
        mo, std::make_shared<hms_cpap::As11NightWriter>(mo.data_dir, hms_cpap::hostLocalClock()));
    hms_cpap::mule::installRoutes(&mule);
    drogon::app().setThreadNum(1);
    std::thread server([] { drogon::app().run(); });

    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        fd = connectTo(port);
    }
    if (fd >= 0) ::close(fd);

    bool ok = fd >= 0;
    for (const auto& r : kRoutes) {
        ok = check(port, r, 200, 0) && ok;
        ok = check(port, r, 3000, 0) && ok;
        ok = check(port, r, 3000, 1024) && ok;   // the bridge's switch test
        ok = check(port, r, 9000, 1024) && ok;   // a bridge's full state
        ok = check(port, r, 300, 100) && ok;     // short fragments
    }

    drogon::app().getLoop()->queueInLoop([] { drogon::app().quit(); });
    server.join();
    std::printf("%s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
