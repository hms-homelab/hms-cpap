#include "services/CpapDashLink.h"

#ifdef BUILD_WITH_WEB

#include <curl/curl.h>
#include <drogon/HttpRequest.h>
#include <drogon/WebSocketClient.h>
#include <trantor/net/EventLoopThread.h>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>

namespace hms_cpap {

namespace {

using Frame = CpapDashLink::Frame;

const char* reqResultText(drogon::ReqResult r) {
    switch (r) {
        case drogon::ReqResult::Ok:                 return "ok";
        case drogon::ReqResult::BadResponse:        return "the server refused the connection";
        case drogon::ReqResult::NetworkFailure:     return "network failure";
        case drogon::ReqResult::BadServerAddress:   return "the server could not be reached";
        case drogon::ReqResult::Timeout:            return "timed out";
        case drogon::ReqResult::HandshakeError:     return "TLS handshake failed";
        case drogon::ReqResult::InvalidCertificate: return "the server certificate is not valid";
        case drogon::ReqResult::EncryptionFailure:  return "encryption failure";
    }
    return "unknown error";
}

// What the loop thread hands the uploader's thread. Shared with the handlers,
// so a frame arriving while the link is torn down has somewhere to go.
struct Inbox {
    std::mutex              mu;
    std::condition_variable cv;
    std::deque<Frame>       q;

    void push(Frame f) {
        {
            std::lock_guard<std::mutex> lk(mu);
            q.push_back(std::move(f));
        }
        cv.notify_all();
    }
};

size_t discardBody(char*, size_t size, size_t n, void*) { return size * n; }

class DrogonLink final : public CpapDashLink {
public:
    explicit DrogonLink(CpapDashEndpoint ep) : ep_(std::move(ep)), inbox_(std::make_shared<Inbox>()) {}
    ~DrogonLink() override { close(); }

    Connect connect(const std::string& token, const std::string& install_id,
                    const std::string& user_agent, std::chrono::milliseconds timeout) override {
        Connect out;
        close();
        loop_ = std::make_unique<trantor::EventLoopThread>("cpapdash-link");
        loop_->run();
        const std::string base = std::string(ep_.tls ? "wss://" : "ws://") + ep_.host + ":" +
                                 std::to_string(ep_.port);
        client_ = drogon::WebSocketClient::newWebSocketClient(base, loop_->getLoop());

        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath(ep_.path);
        req->addHeader("Authorization", "Bearer " + token);
        req->addHeader("X-Hms-Install", install_id);
        req->addHeader("User-Agent", user_agent);

        auto inbox = inbox_;
        client_->setMessageHandler([inbox](std::string&& msg, const drogon::WebSocketClientPtr&,
                                           const drogon::WebSocketMessageType& type) {
            if (type == drogon::WebSocketMessageType::Text) {
                Frame f;
                f.kind = Frame::Kind::Text;
                f.text = std::move(msg);
                inbox->push(std::move(f));
            } else if (type == drogon::WebSocketMessageType::Ping) {
                // Drogon has already answered it with a pong.
                Frame f;
                f.kind = Frame::Kind::Ping;
                inbox->push(std::move(f));
            } else if (type == drogon::WebSocketMessageType::Close) {
                // RFC 6455: a 2-byte code, then the reason. The gateway's close
                // on a revoked token carries the reason Settings shows (D7).
                Frame f;
                f.kind = Frame::Kind::Closed;
                if (msg.size() >= 2) {
                    f.close_code = (static_cast<unsigned char>(msg[0]) << 8) |
                                   static_cast<unsigned char>(msg[1]);
                    f.close_reason = msg.substr(2);
                }
                inbox->push(std::move(f));
            }
        });
        client_->setConnectionClosedHandler([inbox](const drogon::WebSocketClientPtr&) {
            Frame f;
            f.kind = Frame::Kind::Closed;
            inbox->push(std::move(f));
        });

        auto done = std::make_shared<std::promise<drogon::ReqResult>>();
        auto fut  = done->get_future();
        client_->connectToServer(req, [done](drogon::ReqResult r, const drogon::HttpResponsePtr&,
                                             const drogon::WebSocketClientPtr&) {
            try {
                done->set_value(r);
            } catch (...) {
                // A second answer for the same attempt: the first one stands.
            }
        });
        if (fut.wait_for(timeout) != std::future_status::ready) {
            out.error = "timed out";
            close();
            return out;
        }
        const auto r = fut.get();
        if (r == drogon::ReqResult::Ok) {
            out.ok = true;
            return out;
        }
        out.error = reqResultText(r);
        // Drogon does not hand over a refused upgrade's response, so the
        // same request is asked once more over plain HTTP to learn its
        // status: a 401 is the token, which Settings says (SDD-059 D9).
        if (r == drogon::ReqResult::BadResponse) out.http_status = probe(token, install_id, user_agent);
        close();
        return out;
    }

    bool sendText(const std::string& frame) override {
        auto c = client_;
        if (!c) return false;
        auto conn = c->getConnection();
        if (!conn || !conn->connected()) return false;
        conn->send(frame, drogon::WebSocketMessageType::Text);
        return true;
    }

    bool sendBinary(const std::string& frame) override {
        auto c = client_;
        if (!c) return false;
        auto conn = c->getConnection();
        if (!conn || !conn->connected()) return false;
        conn->send(frame, drogon::WebSocketMessageType::Binary);
        return true;
    }

    std::optional<Frame> next(std::chrono::milliseconds wait) override {
        std::unique_lock<std::mutex> lk(inbox_->mu);
        if (inbox_->q.empty()) inbox_->cv.wait_for(lk, wait, [&] { return !inbox_->q.empty(); });
        if (inbox_->q.empty()) return std::nullopt;
        Frame f = std::move(inbox_->q.front());
        inbox_->q.pop_front();
        return f;
    }

    void close() override {
        if (loop_) {
            // The client is stopped and dropped on its own loop, then the
            // loop is ended and joined.
            auto stopped = std::make_shared<std::promise<void>>();
            auto f       = stopped->get_future();
            loop_->getLoop()->queueInLoop([c = std::move(client_), stopped]() mutable {
                if (c) {
                    if (auto conn = c->getConnection(); conn && conn->connected()) conn->shutdown();
                    c->stop();
                }
                c.reset();
                stopped->set_value();
            });
            f.wait_for(std::chrono::seconds(3));
            loop_.reset();
        }
        client_.reset();
        // Frames of a socket now closed must not reach the next one.
        std::lock_guard<std::mutex> lk(inbox_->mu);
        inbox_->q.clear();
    }

private:
    int probe(const std::string& token, const std::string& install_id, const std::string& user_agent) {
        CURL* c = curl_easy_init();
        if (!c) return 0;
        const std::string url = std::string(ep_.tls ? "https://" : "http://") + ep_.host + ":" +
                                std::to_string(ep_.port) + ep_.path;
        struct curl_slist* h = nullptr;
        const std::string auth    = "Authorization: Bearer " + token;
        const std::string install = "X-Hms-Install: " + install_id;
        const std::string ua      = "User-Agent: " + user_agent;
        h = curl_slist_append(h, auth.c_str());
        h = curl_slist_append(h, install.c_str());
        h = curl_slist_append(h, ua.c_str());
        h = curl_slist_append(h, "Connection: Upgrade");
        h = curl_slist_append(h, "Upgrade: websocket");
        h = curl_slist_append(h, "Sec-WebSocket-Version: 13");
        h = curl_slist_append(h, "Sec-WebSocket-Key: aG1zLWNwYXAgcHJvYmUgIQ==");
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discardBody);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
        return static_cast<int>(status);
    }

    CpapDashEndpoint                          ep_;
    std::shared_ptr<Inbox>                    inbox_;
    std::unique_ptr<trantor::EventLoopThread> loop_;
    drogon::WebSocketClientPtr                client_;
};

}  // namespace

std::unique_ptr<CpapDashLink> makeCpapDashLink(const CpapDashEndpoint& ep) {
    return std::make_unique<DrogonLink>(ep);
}

}  // namespace hms_cpap

#else  // BUILD_WITH_WEB

namespace hms_cpap {
std::unique_ptr<CpapDashLink> makeCpapDashLink(const CpapDashEndpoint&) { return nullptr; }
}  // namespace hms_cpap

#endif  // BUILD_WITH_WEB
