// A local stand-in for CpapDash's /socket/v1/hms, speaking platform SDD-059's
// frames, for the uploader's tests.
//
// It is a real WebSocket server on 127.0.0.1 (RFC 6455 handshake, masked
// client frames, unmasked server frames), so the uploader is driven through
// the same Drogon client it uses against api.cpapdash.com. What it does:
//   - checks nothing about the token, but records the upgrade's path and
//     headers, and answers it with `refuse_status` instead when that is set;
//   - answers `hello` with `ack` (or `bye`, when `bye_reason` is set);
//   - keeps binary chunks per (req_id, seq) and, on `file_ready`, assembles
//     them as ingest does (count, size and md5), lands the bytes at `offset`
//     (offset 0 replaces the file) and answers `file_ack {req_id, ok, whole}`;
//   - can drop the socket after N chunks, once, to cut a file mid-way.
#pragma once

#include "TestSocketCompat.h"

#include <json/json.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/select.h>
#endif

namespace cdfake {

inline std::string md5Hex(const std::string& b) {
    unsigned char d[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, EVP_md5(), nullptr);
    EVP_DigestUpdate(c, b.data(), b.size());
    EVP_DigestFinal_ex(c, d, &n);
    EVP_MD_CTX_free(c);
    std::ostringstream o;
    for (unsigned i = 0; i < n; ++i) o << std::hex << std::setw(2) << std::setfill('0') << int(d[i]);
    return o.str();
}

inline std::string base64(const unsigned char* p, size_t n) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16;
        if (i + 1 < n) v |= uint32_t(p[i + 1]) << 8;
        if (i + 2 < n) v |= p[i + 2];
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < n ? k[(v >> 6) & 63] : '=';
        out += i + 2 < n ? k[v & 63] : '=';
    }
    return out;
}

class FakeCpapDashServer {
public:
    struct Ready {
        Json::Value frame;      // the file_ready as sent
        std::string body;       // the assembled bytes
        bool        whole = false;
    };

    // Knobs, set before the uploader connects.
    std::atomic<int>  refuse_status{0};     // answer the upgrade with this HTTP status
    std::atomic<int>  drop_after_chunks{-1};// close after this many chunks, once
    std::atomic<bool> ack_ok{true};
    // Answer hello with bye {retry_in_s, reason}, then close 1008 with the
    // reason: the socket turning a refused install away.
    std::string       bye_reason;
    std::atomic<int>  bye_retry_s{1};
    std::atomic<int>  ack_delay_ms{0};      // ingest still handling the file_ready
    std::atomic<bool> refuse_range_once{false};   // ok:false to the next range (offset > 0)

    FakeCpapDashServer() {
        listen_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        int one = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        ::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t len = sizeof(a);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        ::listen(listen_, 8);
        thread_ = std::thread([this] { serve(); });
    }

    ~FakeCpapDashServer() {
        stop_ = true;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (conn_ >= 0) closeSocket(conn_);
            conn_ = -1;
        }
        stopListening(listen_);
        if (thread_.joinable()) thread_.join();
        if (listen_ >= 0) closeSocket(listen_);
    }

    uint16_t port() const { return port_; }

    /// Waits until [pred] holds (checked under the lock), up to [ms].
    bool waitFor(const std::function<bool()>& pred, int ms = 10000) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms), pred);
    }

    /// Closes the current socket from the server side, as a dropped link.
    void dropNow() {
        std::lock_guard<std::mutex> lk(mu_);
        if (conn_ >= 0) ::shutdown(conn_, 2);
    }

    /// Sends a text frame on the current socket.
    void sendText(const std::string& t) {
        std::lock_guard<std::mutex> lk(mu_);
        if (conn_ >= 0) writeFrame(conn_, 1, t);
    }

    /// Closes the current socket with [code] and [reason], as the gateway
    /// does on a revoked token (1008, "hms token revoked: ...").
    void closeWith(int code, const std::string& reason) {
        std::lock_guard<std::mutex> lk(mu_);
        if (conn_ < 0) return;
        std::string p;
        p += static_cast<char>((code >> 8) & 0xff);
        p += static_cast<char>(code & 0xff);
        p += reason;
        writeFrame(conn_, 8, p);
        ::shutdown(conn_, 2);
    }

    int pings() { std::lock_guard<std::mutex> lk(mu_); return pings_; }
    /// file_ready ("ready:<name>:kept|not") and round ("round:<n>") frames, in arrival order.
    std::vector<std::string> events() { std::lock_guard<std::mutex> lk(mu_); return events_; }
    size_t roundsLocked() const {
        size_t n = 0;
        for (const auto& e : events_) n += e.rfind("round:", 0) == 0;
        return n;
    }
    /// Steady-clock times of each upgrade, for the waits between them.
    std::vector<std::chrono::steady_clock::time_point> upgradeTimes() {
        std::lock_guard<std::mutex> lk(mu_);
        return upgrade_times_;
    }

    // What was seen, read under lock() or through the accessors.
    std::mutex& lock() { return mu_; }
    int upgrades() { std::lock_guard<std::mutex> lk(mu_); return upgrades_; }
    int hellos() { std::lock_guard<std::mutex> lk(mu_); return hellos_; }
    std::string lastPath() { std::lock_guard<std::mutex> lk(mu_); return last_path_; }
    std::map<std::string, std::string> lastHeaders() { std::lock_guard<std::mutex> lk(mu_); return last_headers_; }
    Json::Value lastHello() { std::lock_guard<std::mutex> lk(mu_); return last_hello_; }
    std::vector<Ready> readies() { std::lock_guard<std::mutex> lk(mu_); return readies_; }
    long long chunkBytes() { std::lock_guard<std::mutex> lk(mu_); return chunk_bytes_; }
    /// The file as landed, by machine|date_folder|rel_dir|filename.
    std::map<std::string, std::string> landed() { std::lock_guard<std::mutex> lk(mu_); return landed_; }
    static std::string keyOf(const std::string& machine, const std::string& date_folder,
                             const std::string& rel_dir, const std::string& filename) {
        return machine + "|" + date_folder + "|" + rel_dir + "|" + filename;
    }
    // Under lock(), for waitFor predicates.
    size_t readiesLocked() const { return readies_.size(); }
    int hellosLocked() const { return hellos_; }
    int upgradesLocked() const { return upgrades_; }
    const std::map<std::string, std::string>& landedLocked() const { return landed_; }

private:
    bool readable(int fd, int ms) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        return ::select(fd + 1, &set, nullptr, nullptr, &tv) > 0;
    }

    // Reads exactly n bytes, or false when the socket ended or the server stops.
    bool readExact(int fd, std::string& out, size_t n) {
        out.clear();
        while (out.size() < n) {
            if (stop_) return false;
            if (!readable(fd, 100)) continue;
            char buf[16384];
            const size_t want = std::min(sizeof(buf), n - out.size());
            const auto got = ::recv(fd, buf, static_cast<int>(want), 0);
            if (got <= 0) return false;
            out.append(buf, static_cast<size_t>(got));
        }
        return true;
    }

    static void sendAll(int fd, const std::string& s) {
        size_t at = 0;
        while (at < s.size()) {
            const auto n = ::send(fd, s.data() + at, static_cast<int>(s.size() - at), 0);
            if (n <= 0) return;
            at += static_cast<size_t>(n);
        }
    }

    static void writeFrame(int fd, int opcode, const std::string& payload) {
        std::string f;
        f += static_cast<char>(0x80 | opcode);
        if (payload.size() < 126) {
            f += static_cast<char>(payload.size());
        } else if (payload.size() < 65536) {
            f += static_cast<char>(126);
            f += static_cast<char>((payload.size() >> 8) & 0xff);
            f += static_cast<char>(payload.size() & 0xff);
        } else {
            f += static_cast<char>(127);
            for (int i = 7; i >= 0; --i) f += static_cast<char>((uint64_t(payload.size()) >> (8 * i)) & 0xff);
        }
        f += payload;
        sendAll(fd, f);
    }

    void serve() {
        while (!stop_) {
            if (!readable(listen_, 100)) continue;
            const int fd = static_cast<int>(::accept(listen_, nullptr, nullptr));
            if (fd < 0) continue;
            handle(fd);
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (conn_ == fd) conn_ = -1;
            }
            closeSocket(fd);
            cv_.notify_all();
        }
    }

    void handle(int fd) {
        // The upgrade request.
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
            std::string c;
            if (!readExact(fd, c, 1)) return;
            req += c;
            if (req.size() > 65536) return;
        }
        std::istringstream in(req);
        std::string line;
        std::getline(in, line);
        std::string method, path;
        {
            std::istringstream l(line);
            l >> method >> path;
        }
        std::map<std::string, std::string> headers;
        while (std::getline(in, line) && line != "\r") {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string k = line.substr(0, colon), v = line.substr(colon + 1);
            for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            while (!v.empty() && (v.front() == ' ')) v.erase(0, 1);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            headers[k] = v;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++upgrades_;
            upgrade_times_.push_back(std::chrono::steady_clock::now());
            last_path_ = path;
            last_headers_ = headers;
        }
        cv_.notify_all();
        if (const int st = refuse_status.load(); st != 0) {
            sendAll(fd, "HTTP/1.1 " + std::to_string(st) +
                            " Refused\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
                            "Connection: close\r\n\r\n{}");
            return;
        }
        const std::string key = headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        unsigned char sha[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const unsigned char*>(key.data()), key.size(), sha);
        sendAll(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + base64(sha, SHA_DIGEST_LENGTH) + "\r\n\r\n");
        {
            std::lock_guard<std::mutex> lk(mu_);
            conn_ = fd;
        }

        std::map<uint32_t, std::map<uint32_t, std::string>> chunks;
        while (!stop_) {
            std::string h;
            if (!readExact(fd, h, 2)) return;
            const int opcode = h[0] & 0x0f;
            const bool masked = (h[1] & 0x80) != 0;
            uint64_t len = h[1] & 0x7f;
            std::string ext;
            if (len == 126) {
                if (!readExact(fd, ext, 2)) return;
                len = (uint64_t(uint8_t(ext[0])) << 8) | uint8_t(ext[1]);
            } else if (len == 127) {
                if (!readExact(fd, ext, 8)) return;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(ext[i]);
            }
            std::string mask;
            if (masked && !readExact(fd, mask, 4)) return;
            std::string payload;
            if (!readExact(fd, payload, static_cast<size_t>(len))) return;
            if (masked)
                for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);

            if (opcode == 8) {
                std::lock_guard<std::mutex> lk(mu_);
                writeFrame(fd, 8, payload.substr(0, 2));
                return;
            }
            if (opcode == 9) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    ++pings_;
                    writeFrame(fd, 10, payload);
                }
                cv_.notify_all();
                continue;
            }
            if (opcode == 2) {
                if (payload.size() < 12) continue;
                auto u32 = [&](size_t at) {
                    return uint32_t(uint8_t(payload[at])) | uint32_t(uint8_t(payload[at + 1])) << 8 |
                           uint32_t(uint8_t(payload[at + 2])) << 16 | uint32_t(uint8_t(payload[at + 3])) << 24;
                };
                const uint32_t req = u32(0), seq = u32(4);
                chunks[req][seq] = payload.substr(12);
                bool drop = false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    chunk_bytes_ += static_cast<long long>(payload.size() - 12);
                    ++chunks_seen_;
                    const int d = drop_after_chunks.load();
                    if (d >= 0 && chunks_seen_ >= d) {
                        drop_after_chunks = -1;
                        drop = true;
                    }
                }
                if (drop) {
                    cv_.notify_all();
                    return;   // the link drops mid-file
                }
                continue;
            }
            if (opcode != 1) continue;

            Json::Value v;
            Json::CharReaderBuilder rb;
            std::istringstream js(payload);
            if (!Json::parseFromStream(rb, js, &v, nullptr) || !v.isObject()) continue;
            const std::string t = v.get("t", "").asString();
            if (t == "hello") {
                std::lock_guard<std::mutex> lk(mu_);
                ++hellos_;
                last_hello_ = v;
                if (!bye_reason.empty()) {
                    writeFrame(fd, 1, R"({"t":"bye","retry_in_s":)" + std::to_string(bye_retry_s.load()) +
                                          R"(,"reason":")" + bye_reason + "\"}");
                    std::string p;
                    p += static_cast<char>(1008 >> 8);
                    p += static_cast<char>(1008 & 0xff);
                    p += bye_reason;
                    writeFrame(fd, 8, p);
                    return;
                } else {
                    writeFrame(fd, 1, R"({"t":"ack","server_time":"2026-11-16T00:00:00Z","ping_s":30})");
                }
            } else if (t == "round") {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    events_.push_back("round:" + std::to_string(v.get("new_files", -1).asInt()));
                }
                cv_.notify_all();
            } else if (t == "file_ready") {
                const uint32_t req = v.get("req_id", 0).asUInt();
                const uint32_t n = v.get("chunks", 0).asUInt();
                std::string body;
                bool whole = true;
                auto& got = chunks[req];
                for (uint32_t i = 0; i < n; ++i) {
                    auto it = got.find(i);
                    if (it == got.end()) {
                        whole = false;
                        break;
                    }
                    body += it->second;
                }
                if (body.size() != v.get("size", 0).asUInt64()) whole = false;
                if (md5Hex(body) != v.get("md5", "").asString()) whole = false;
                chunks.erase(req);
                // file_ack only once ingest has handled the file_ready.
                if (const int d = ack_delay_ms.load(); d > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(d));
                bool keep = whole && ack_ok.load();
                if (keep && v.get("offset", 0).asUInt64() > 0 && refuse_range_once.exchange(false))
                    keep = false;   // a range past what ingest holds: nothing kept
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    readies_.push_back({v, body, keep});
                    events_.push_back("ready:" + v.get("filename", "").asString() + (keep ? ":kept" : ":not"));
                    if (keep) {
                        auto& file = landed_[keyOf(v.get("machine_serial", "").asString(),
                                                   v.get("date_folder", "").asString(),
                                                   v.get("rel_dir", "").asString(),
                                                   v.get("filename", "").asString())];
                        const uint64_t off = v.get("offset", 0).asUInt64();
                        if (off == 0) file.clear();              // a replacement
                        if (file.size() > off) file.resize(off);
                        if (file.size() == off) file += body;
                    }
                    Json::Value ack;
                    ack["t"] = "file_ack";
                    ack["req_id"] = req;
                    ack["ok"] = keep;   // ingest: ok:false means nothing was kept
                    ack["whole"] = whole;
                    Json::StreamWriterBuilder wb;
                    wb["indentation"] = "";
                    writeFrame(fd, 1, Json::writeString(wb, ack));
                }
                cv_.notify_all();
            }
            cv_.notify_all();
        }
    }

    int listen_ = -1;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::mutex mu_;
    std::condition_variable cv_;
    int conn_ = -1;
    int upgrades_ = 0;
    int hellos_ = 0;
    int chunks_seen_ = 0;
    int pings_ = 0;
    std::vector<std::string> events_;
    std::vector<std::chrono::steady_clock::time_point> upgrade_times_;
    long long chunk_bytes_ = 0;
    std::string last_path_;
    std::map<std::string, std::string> last_headers_;
    Json::Value last_hello_;
    std::vector<Ready> readies_;
    std::map<std::string, std::string> landed_;
};

}  // namespace cdfake
