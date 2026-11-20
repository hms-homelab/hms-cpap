#ifdef BUILD_WITH_WEB
#include "controllers/OscarHelperSocket.h"

#include "controllers/SleepBridgeSocket.h"

#include <drogon/HttpAppFramework.h>

#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

namespace hms_cpap::oscar::helper {

namespace {

const char* const kAdmitAttr = "oscar_helper_admit";
constexpr int kPingSec    = 30;
constexpr int kSilenceSec = 90;

struct Entry {
    std::mutex                                 mu;
    uint64_t                                   id{0};
    std::weak_ptr<drogon::WebSocketConnection> conn;
    std::time_t                                last_heard{0};
};

Host*                                      g_host{nullptr};
std::mutex                                 g_mu;
std::map<uint64_t, std::shared_ptr<Entry>> g_entries;

std::shared_ptr<Entry> find(uint64_t id) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_entries.find(id);
    return it == g_entries.end() ? nullptr : it->second;
}

// D2: 90 s of silence closes the connection; the helper reconnects by itself.
void armSilence(const std::weak_ptr<Entry>& weak, double after) {
    drogon::app().getLoop()->runAfter(after, [weak] {
        const auto e = weak.lock();
        if (!e) return;
        std::lock_guard<std::mutex> lk(e->mu);
        const auto c = e->conn.lock();
        if (!c || !c->connected()) return;
        const std::time_t idle = std::time(nullptr) - e->last_heard;
        if (idle >= kSilenceSec) {
            std::cout << "OSCAR helper: silent " << idle << " s, closed" << std::endl;
            c->shutdown(drogon::CloseCode::kNormalClosure, "silent");
            return;
        }
        armSilence(weak, static_cast<double>(kSilenceSec - idle));
    });
}

}  // namespace

void installRoutes(Host* host) {
    g_host = host;
    if (!host) return;
    sleepbridge::addStreamRoute(kRoute);
}

void AdmitFilter::doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                           drogon::FilterChainCallback&& fccb) {
    if (!g_host) {
        auto r = drogon::HttpResponse::newHttpJsonResponse(Json::Value("not found"));
        r->setStatusCode(drogon::k404NotFound);
        return fcb(r);
    }
    const Admission a = g_host->admit(req->getHeader("Authorization"), req->getHeader(kHeaderName),
                                      !req->getHeader(kHeaderProbe).empty());
    if (a.as == Admit::Refused) {
        auto r = drogon::HttpResponse::newHttpJsonResponse(a.body);
        r->setStatusCode(static_cast<drogon::HttpStatusCode>(a.status));
        return fcb(r);
    }
    req->attributes()->insert(kAdmitAttr, a);
    fccb();
}

void HelperSocket::handleNewConnection(const drogon::HttpRequestPtr& req, const drogon::WebSocketConnectionPtr& conn) {
    if (!g_host || !req->attributes()->find(kAdmitAttr)) {
        conn->shutdown(drogon::CloseCode::kViolation, "unadmitted");
        return;
    }
    const auto a = req->attributes()->get<Admission>(kAdmitAttr);
    std::weak_ptr<drogon::WebSocketConnection> weak = conn;
    Outlets out;
    out.text = [weak](const std::string& t) {
        const auto c = weak.lock();
        if (!c || !c->connected()) return false;
        c->send(t, drogon::WebSocketMessageType::Text);
        return true;
    };
    out.binary = [weak](const std::string& b) {
        const auto c = weak.lock();
        if (!c || !c->connected()) return false;
        c->send(b, drogon::WebSocketMessageType::Binary);
        return true;
    };
    out.close = [weak] {
        if (const auto c = weak.lock(); c && c->connected()) c->shutdown(drogon::CloseCode::kNormalClosure, "closed");
    };
    auto e = std::make_shared<Entry>();
    e->id = g_host->open(a, std::move(out));
    e->conn = conn;
    e->last_heard = std::time(nullptr);
    conn->setPingMessage("", std::chrono::seconds(kPingSec));
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_entries[e->id] = e;
    }
    conn->setContext(std::make_shared<uint64_t>(e->id));
    armSilence(e, static_cast<double>(kSilenceSec));
}

void HelperSocket::handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                                    const drogon::WebSocketMessageType& type) {
    if (!conn->hasContext() || !g_host) return;
    const uint64_t id = *conn->getContext<uint64_t>();
    const auto e = find(id);
    if (!e) return;
    {
        std::lock_guard<std::mutex> lk(e->mu);
        e->last_heard = std::time(nullptr);
    }
    switch (type) {
        case drogon::WebSocketMessageType::Text:   g_host->onText(id, message); break;
        case drogon::WebSocketMessageType::Binary: g_host->onBinary(id, message); break;
        default: break;   // a ping or pong counts as heard
    }
}

void HelperSocket::handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) {
    if (!conn->hasContext()) return;
    const uint64_t id = *conn->getContext<uint64_t>();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_entries.erase(id);
    }
    if (g_host) g_host->closed(id);
}

}  // namespace hms_cpap::oscar::helper

#endif  // BUILD_WITH_WEB
