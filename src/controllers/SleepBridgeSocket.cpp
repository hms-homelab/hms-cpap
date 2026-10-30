#ifdef BUILD_WITH_WEB
#include "controllers/SleepBridgeSocket.h"

#include <drogon/HttpAppFramework.h>

#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

namespace hms_cpap::sleepbridge {

namespace {

const char* const kWhoAttr = "sleepbridge_who";

struct Entry {
    std::mutex                                 mu;   // one frame at a time per session
    std::unique_ptr<Conversation>              conv;
    std::weak_ptr<drogon::WebSocketConnection> conn;
    std::time_t                                last_heard{0};
};

SleepBridgeService*                        g_service{nullptr};
std::mutex                                 g_mu;
std::map<uint64_t, std::shared_ptr<Entry>> g_entries;
uint64_t                                   g_next_id{1};

drogon::HttpResponsePtr jsonError(drogon::HttpStatusCode code, const std::string& why) {
    Json::Value v(Json::objectValue);
    v["ok"]    = false;
    v["error"] = why;
    auto resp  = drogon::HttpResponse::newHttpJsonResponse(v);
    resp->setStatusCode(code);
    return resp;
}

bool onStreamPort(const drogon::HttpRequestPtr& req) {
    return g_service && req->getLocalAddr().toPort() == g_service->options().port;
}

std::shared_ptr<Entry> find(uint64_t id) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_entries.find(id);
    return it == g_entries.end() ? nullptr : it->second;
}

// Called with the entry's lock held, so one session's frames leave in order.
void send(Entry& e, const Effects& fx) {
    if (auto c = e.conn.lock())
        for (const auto& t : fx.texts) c->send(t, drogon::WebSocketMessageType::Text);
    if (g_service) g_service->apply(e.conv->who().sender, fx);
}

// Silence closes a session: due kSilenceSec after the last thing heard. The
// bridge reconnects by itself; until it does, it shows offline.
void armSilence(const std::weak_ptr<Entry>& weak, double after) {
    drogon::app().getLoop()->runAfter(after, [weak] {
        const auto e = weak.lock();
        if (!e) return;
        std::lock_guard<std::mutex> lk(e->mu);
        const auto c = e->conn.lock();
        if (!c || !c->connected()) return;
        const std::time_t idle = std::time(nullptr) - e->last_heard;
        if (idle >= kSilenceSec) {
            std::cout << "[sleepbridge] " << e->conv->who().sender << " silent " << idle
                      << " s, closed" << std::endl;
            c->shutdown(drogon::CloseCode::kNormalClosure, "silent");
            return;
        }
        armSilence(weak, static_cast<double>(kSilenceSec - idle));
    });
}

}  // namespace

void installRoutes(SleepBridgeService* service) {
    g_service = service;
    if (!service) return;
    const int port = service->options().port;

    drogon::app().addListener("0.0.0.0", static_cast<uint16_t>(port));
    // Drogon's default (128 KB) is below a full batch (512 records, 258 KB).
    drogon::app().setClientMaxWebSocketMessageSize(kMaxBatchSize + kFrameHeaderSize + 4096);

    // The port split. On the stream port, the stream route and nothing else:
    // no UI, no API, no static files. Elsewhere, never the stream route.
    drogon::app().registerPreRoutingAdvice(
        [](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& stop,
           drogon::AdviceChainCallback&& pass) {
            const bool stream = onStreamPort(req);
            const bool route  = req->path() == kRoute;
            if (stream != route) {
                stop(jsonError(drogon::k404NotFound, "not found"));
                return;
            }
            pass();
        });

    drogon::app().registerHandler(
        "/api/sleepbridge",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            cb(drogon::HttpResponse::newHttpJsonResponse(
                g_service ? g_service->status() : Json::Value(Json::objectValue)));
        },
        {drogon::Get});

    std::cout << "Sleep Bridge: listening on ws://0.0.0.0:" << port << kRoute
              << (service->available() ? "" : " (not in this build: bridges are refused)")
              << std::endl;
}

void AdmitFilter::doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                           drogon::FilterChainCallback&& fccb) {
    if (!onStreamPort(req)) return fcb(jsonError(drogon::k404NotFound, "not found"));
    if (!g_service->available())
        return fcb(jsonError(drogon::k501NotImplemented, "The Sleep Bridge is not in this build"));
    std::string why;
    const auto who = admit([&](const std::string& h) { return req->getHeader(h); }, &why);
    if (!who) return fcb(jsonError(drogon::k400BadRequest, why));
    req->attributes()->insert(kWhoAttr, *who);
    fccb();
}

void DeviceSocket::handleNewConnection(const drogon::HttpRequestPtr& req,
                                       const drogon::WebSocketConnectionPtr& conn) {
    if (!g_service || !req->attributes()->find(kWhoAttr)) {
        conn->shutdown(drogon::CloseCode::kViolation, "unadmitted");
        return;
    }
    auto e        = std::make_shared<Entry>();
    e->conv       = g_service->open(req->attributes()->get<Admission>(kWhoAttr));
    e->conn       = conn;
    e->last_heard = std::time(nullptr);
    conn->setPingMessage("", std::chrono::seconds(30));
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        id            = g_next_id++;
        g_entries[id] = e;
    }
    conn->setContext(std::make_shared<uint64_t>(id));
    armSilence(e, static_cast<double>(kSilenceSec));
}

void DeviceSocket::handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                                    std::string&& message,
                                    const drogon::WebSocketMessageType& type) {
    if (!conn->hasContext()) return;
    const auto e = find(*conn->getContext<uint64_t>());
    if (!e) return;
    std::lock_guard<std::mutex> lk(e->mu);
    e->last_heard = std::time(nullptr);
    switch (type) {
        case drogon::WebSocketMessageType::Text:   send(*e, e->conv->onText(message)); break;
        case drogon::WebSocketMessageType::Binary: send(*e, e->conv->onBinary(message)); break;
        default: break;   // a pong counts as heard
    }
}

void DeviceSocket::handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) {
    if (!conn->hasContext()) return;
    const uint64_t id = *conn->getContext<uint64_t>();
    std::shared_ptr<Entry> e;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto it = g_entries.find(id);
        if (it == g_entries.end()) return;
        e = it->second;
        g_entries.erase(it);
    }
    std::lock_guard<std::mutex> lk(e->mu);
    if (g_service) g_service->closed(e->conv->who().sender);
}

}  // namespace hms_cpap::sleepbridge

#endif  // BUILD_WITH_WEB
