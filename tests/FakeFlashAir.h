// A Toshiba FlashAir on loopback, serving a folder as the card would: op=100
// listings of any DIR (the root is DIR=/), files at their card path with Range
// honoured by a 206, and a missing file or folder answered 307 to
// http://flashair/. Every file and folder is stamped 2025-03-11 07:00:00, so
// a file that changes is seen by its size. Requests are recorded.
//
// Many requests, one at a time, as the card serves them.
#pragma once

#include "TestSocketCompat.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class FakeFlashAir {
public:
    explicit FakeFlashAir(std::filesystem::path dir) : dir_(std::move(dir)) {
        fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        EXPECT_GE(fd_, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        EXPECT_EQ(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        EXPECT_EQ(::listen(fd_, 8), 0);
        socklen_t len = sizeof(addr);
        EXPECT_EQ(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeFlashAir() {
        // A shutdown() does not wake accept() on every platform (macOS): one
        // last connection does, and the loop sees the stop.
        stop_ = true;
        const int wake = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port_);
        ::connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        closeSocket(wake);
        stopListening(fd_);
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) closeSocket(fd_);
    }
    FakeFlashAir(const FakeFlashAir&) = delete;
    FakeFlashAir& operator=(const FakeFlashAir&) = delete;

    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

    /// "GET <target>" per request, with " range=<from>" when one was asked.
    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }
    void clearRequests() {
        std::lock_guard<std::mutex> lk(mu_);
        requests_.clear();
    }
    /// Any request containing [part].
    bool asked(const std::string& part) {
        const auto r = requests();
        return std::any_of(r.begin(), r.end(),
                           [&](const std::string& s) { return s.find(part) != std::string::npos; });
    }

private:
    void serve() {
        for (;;) {
            const int conn = static_cast<int>(::accept(fd_, nullptr, nullptr));
            if (conn < 0) return;
            if (stop_) {
                closeSocket(conn);
                return;
            }
            std::string req;
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                const auto n = ::recv(conn, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
            }
            reply(conn, req);
            closeSocket(conn);
        }
    }

    /// "/a/b" -> dir_/a/b; "" for anything outside the folder.
    std::filesystem::path local(const std::string& card_path) const {
        if (card_path.empty() || card_path[0] != '/' || card_path.find("..") != std::string::npos) return {};
        return card_path == "/" ? dir_ : dir_ / card_path.substr(1);
    }

    void reply(int conn, const std::string& req) {
        namespace fs = std::filesystem;
        const auto sp1 = req.find(' ');
        const auto sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
        const std::string target =
            sp2 == std::string::npos ? "" : req.substr(sp1 + 1, sp2 - sp1 - 1);
        long long from = -1;
        const auto r = req.find("Range: bytes=");
        if (r != std::string::npos) from = std::stoll(req.substr(r + 13));
        {
            std::lock_guard<std::mutex> lk(mu_);
            requests_.push_back("GET " + target + (from >= 0 ? " range=" + std::to_string(from) : ""));
        }

        std::string status = "307 Temporary Redirect", headers = "Location: http://flashair/\r\n", body;
        const std::string kList = "/command.cgi?op=100&DIR=";
        std::error_code ec;
        if (target.rfind(kList, 0) == 0) {
            const std::string dir = target.substr(kList.size());
            const fs::path p = local(dir);
            if (!p.empty() && fs::is_directory(p, ec)) {
                status = "200 OK";
                headers.clear();
                body = "WLANSD_FILELIST\r\n";
                const unsigned fdate = ((2025 - 1980) << 9) | (3 << 5) | 11;
                const unsigned ftime = (7 << 11);
                const std::string parent = dir == "/" ? "" : dir;
                for (const auto& e : fs::directory_iterator(p)) {
                    const bool is_dir = e.is_directory(ec);
                    body += parent + "," + e.path().filename().string() + "," +
                            std::to_string(is_dir ? 0 : fs::file_size(e.path())) + "," +
                            (is_dir ? "16" : "32") + "," + std::to_string(fdate) + "," +
                            std::to_string(ftime) + "\r\n";
                }
            } else {
                status = "404 Not Found";
                headers.clear();
            }
        } else if (target.find('?') == std::string::npos) {
            const fs::path p = local(target);
            if (!p.empty() && fs::is_regular_file(p, ec)) {
                std::ifstream in(p, std::ios::binary);
                const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                headers.clear();
                if (from > 0 && static_cast<size_t>(from) < all.size()) {
                    status = "206 Partial Content";
                    body = all.substr(static_cast<size_t>(from));
                    headers = "Content-Range: bytes " + std::to_string(from) + "-" +
                              std::to_string(all.size() - 1) + "/" + std::to_string(all.size()) + "\r\n";
                } else {
                    status = "200 OK";
                    body = all;
                }
            }
        }
        const std::string resp = "HTTP/1.1 " + status + "\r\n" + headers +
                                 "Content-Length: " + std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + body;
        size_t sent = 0;
        while (sent < resp.size()) {
            const auto s = ::send(conn, resp.data() + sent, static_cast<int>(resp.size() - sent), 0);
            if (s <= 0) break;
            sent += static_cast<size_t>(s);
        }
    }

    std::filesystem::path dir_;
    int fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mu_;
    std::vector<std::string> requests_;
};
