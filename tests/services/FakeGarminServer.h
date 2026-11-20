// SDD-078: a local stand-in for Garmin's sign-in, token and data endpoints,
// for GarminConnectClient's and GarminService's tests. Plain HTTP on
// 127.0.0.1, one request per connection (Connection: close); each request is
// recorded and answered by `route`, which the test sets.
#pragma once

#include "TestSocketCompat.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace garminfake {

struct Request {
    std::string method;
    std::string path;     ///< without the query
    std::string query;
    std::map<std::string, std::string> headers;   ///< names lowercased
    std::string body;
};

struct Response {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::vector<std::string> extra_headers;   ///< e.g. "Set-Cookie: SESSION=abc"
};

class Server {
public:
    std::function<Response(const Request&)> route;

    Server() {
        fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        ::listen(fd_, 16);
        socklen_t len = sizeof addr;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { loop(); });
    }

    ~Server() {
        stopping_ = true;
        // A connection of our own wakes accept() on every platform (shutdown
        // on a listening socket does not on macOS).
        const int w = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        ::connect(w, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        closeSocket(w);
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) closeSocket(fd_);
    }

    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lk(mu_);
        return log_;
    }
    size_t count(const std::string& path) const {
        std::lock_guard<std::mutex> lk(mu_);
        size_t n = 0;
        for (const auto& r : log_) if (r.path == path) ++n;
        return n;
    }
    size_t total() const {
        std::lock_guard<std::mutex> lk(mu_);
        return log_.size();
    }

private:
    void loop() {
        while (!stopping_) {
            const int c = static_cast<int>(::accept(fd_, nullptr, nullptr));
            if (c < 0) break;
            if (stopping_) { closeSocket(c); break; }
            serve(c);
            closeSocket(c);
        }
    }

    static std::string lower(std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    }

    void serve(int c) {
#ifndef _WIN32
        timeval tv{5, 0};   // a client that never sends cannot hold the test
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        std::string raw;
        char buf[4096];
        size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const auto n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) return;
            raw.append(buf, static_cast<size_t>(n));
            header_end = raw.find("\r\n\r\n");
        }
        Request req;
        {
            const std::string head = raw.substr(0, header_end);
            size_t line_end = head.find("\r\n");
            const std::string first = head.substr(0, line_end);
            const auto sp1 = first.find(' ');
            const auto sp2 = first.find(' ', sp1 + 1);
            req.method = first.substr(0, sp1);
            const std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
            const auto q = target.find('?');
            req.path = target.substr(0, q);
            if (q != std::string::npos) req.query = target.substr(q + 1);
            size_t at = line_end == std::string::npos ? head.size() : line_end + 2;
            while (at < head.size()) {
                auto e = head.find("\r\n", at);
                if (e == std::string::npos) e = head.size();
                const std::string h = head.substr(at, e - at);
                const auto colon = h.find(':');
                if (colon != std::string::npos) {
                    std::string v = h.substr(colon + 1);
                    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                    req.headers[lower(h.substr(0, colon))] = v;
                }
                at = e + 2;
            }
        }
        size_t want = 0;
        if (auto it = req.headers.find("content-length"); it != req.headers.end())
            want = static_cast<size_t>(std::stoul(it->second));
        req.body = raw.substr(header_end + 4);
        while (req.body.size() < want) {
            const auto n = ::recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.body.append(buf, static_cast<size_t>(n));
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            log_.push_back(req);
        }
        Response res = route ? route(req) : Response{404, "{}"};
        std::string out = "HTTP/1.1 " + std::to_string(res.status) + " X\r\n";
        out += "Content-Type: " + res.content_type + "\r\n";
        for (const auto& h : res.extra_headers) out += h + "\r\n";
        out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += res.body;
        size_t sent = 0;
        while (sent < out.size()) {
            const auto n = ::send(c, out.data() + sent, out.size() - sent, 0);
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
    }

    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    std::vector<Request> log_;
};

/// An unsigned JWT whose payload is [payload_json] (the client reads exp and
/// client_id without verifying).
inline std::string jwt(const std::string& payload_json) {
    auto b64url = [](const std::string& in) {
        static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                               (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
            out += k[(v >> 18) & 63]; out += k[(v >> 12) & 63]; out += k[(v >> 6) & 63]; out += k[v & 63];
        }
        if (i + 1 == in.size()) {
            const unsigned v = static_cast<unsigned char>(in[i]) << 16;
            out += k[(v >> 18) & 63]; out += k[(v >> 12) & 63];
        } else if (i + 2 == in.size()) {
            const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
            out += k[(v >> 18) & 63]; out += k[(v >> 12) & 63]; out += k[(v >> 6) & 63];
        }
        return out;
    };
    return b64url(R"({"alg":"RS256","typ":"JWT"})") + "." + b64url(payload_json) + ".sig";
}

/// Garmin as the client sees it: the mobile sign-in, an optional MFA code,
/// the DI token endpoint and the three connectapi calls, with knobs.
class FakeGarmin {
public:
    std::string password = "right-password";
    bool        mfa = false;
    std::string mfa_code = "123456";
    bool        changed_sign_in = false;   ///< the login answers an HTML page
    bool        refuse_refresh = false;
    long long   token_exp = 4102444800;     ///< 2100-01-01: the access token's exp
    std::string accepted_client = "GARMIN_CONNECT_MOBILE_ANDROID_DI";
    std::map<std::string, std::string> sleep_by_date;   ///< date -> JSON body
    std::string device_name = "fenix 7X";
    std::string display_name = "runner-42";

    int tokens_issued = 0;
    int refreshes = 0;

    Server server;

    FakeGarmin() {
        server.route = [this](const Request& r) { return answer(r); };
    }

    std::string base() const { return server.base(); }
    std::string accessToken() const {
        return jwt("{\"exp\":" + std::to_string(token_exp) + ",\"client_id\":\"" + accepted_client +
                   "\",\"n\":" + std::to_string(tokens_issued) + "}");
    }

private:
    static std::string field(const std::string& body, const std::string& key) {
        // form or JSON, enough for the fields the client sends
        const std::string f = key + "=";
        auto at = body.find(f);
        if (at != std::string::npos && (at == 0 || body[at - 1] == '&')) {
            const auto end = body.find('&', at);
            return body.substr(at + f.size(), end == std::string::npos ? std::string::npos : end - at - f.size());
        }
        const std::string j = "\"" + key + "\":\"";
        at = body.find(j);
        if (at == std::string::npos) return "";
        const auto end = body.find('"', at + j.size());
        return body.substr(at + j.size(), end - at - j.size());
    }

    Response tokens() {
        ++tokens_issued;
        return {200, "{\"access_token\":\"" + accessToken() + "\",\"refresh_token\":\"refresh-" +
                         std::to_string(tokens_issued) + "\",\"expires_in\":3600}"};
    }

    Response answer(const Request& r) {
        if (r.path == "/mobile/sso/en_US/sign-in")
            return {200, "<html>sign in</html>", "text/html", {"Set-Cookie: SESSION=s1; Path=/"}};
        if (r.path == "/mobile/api/login") {
            if (changed_sign_in) return {200, "<html>a new sign-in page</html>", "text/html"};
            if (r.headers.count("cookie") == 0 || r.headers.at("cookie").find("SESSION=s1") == std::string::npos)
                return {429, "{}"};
            if (field(r.body, "password") != password)
                return {200, R"({"responseStatus":{"type":"INVALID_USERNAME_PASSWORD"}})"};
            if (mfa)
                return {200, R"({"responseStatus":{"type":"MFA_REQUIRED"},"customerMfaInfo":{"mfaLastMethodUsed":"email"}})"};
            return {200, R"({"responseStatus":{"type":"SUCCESSFUL"},"serviceTicketId":"ST-1"})"};
        }
        if (r.path == "/mobile/api/mfa/verifyCode") {
            if (field(r.body, "mfaVerificationCode") != mfa_code)
                return {200, R"({"responseStatus":{"type":"INVALID_MFA_CODE"}})"};
            return {200, R"({"responseStatus":{"type":"SUCCESSFUL"},"serviceTicketId":"ST-2"})"};
        }
        if (r.path == "/di-oauth2-service/oauth/token") {
            const std::string grant = field(r.body, "grant_type");
            if (grant == "refresh_token") {
                ++refreshes;
                if (refuse_refresh) return {400, R"({"error":"invalid_grant"})"};
                return tokens();
            }
            if (field(r.body, "client_id") != accepted_client) return {400, R"({"error":"unauthorized_client"})"};
            return tokens();
        }
        // connectapi: the bearer must be the current token.
        const auto auth = r.headers.count("authorization") ? r.headers.at("authorization") : "";
        if (auth != "Bearer " + accessToken()) return {401, "{}"};
        if (r.path == "/userprofile-service/socialProfile")
            return {200, "{\"displayName\":\"" + display_name + "\",\"userName\":\"u\"}"};
        if (r.path == "/device-service/deviceservice/mylastused")
            return {200, "{\"lastUsedDeviceName\":\"" + device_name + "\"}"};
        if (r.path == "/wellness-service/wellness/dailySleepData/" + display_name) {
            const auto d = r.query.find("date=");
            const std::string date = d == std::string::npos ? "" : r.query.substr(d + 5, 10);
            const auto it = sleep_by_date.find(date);
            return {200, it == sleep_by_date.end() ? "{}" : it->second};
        }
        return {404, "{}"};
    }
};

}  // namespace garminfake
