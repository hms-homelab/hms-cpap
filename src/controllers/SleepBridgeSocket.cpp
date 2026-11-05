#ifdef BUILD_WITH_WEB
#include "controllers/SleepBridgeSocket.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/MultiPart.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace hms_cpap::sleepbridge {

namespace {

const char* const kWhoAttr = "sleepbridge_who";

ReleaseFeed* g_feed{nullptr};

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
std::vector<std::string>                   g_more_routes;   // addStreamRoute, before run()

bool streamRoute(const std::string& path) {
    return path == kRoute ||
           std::find(g_more_routes.begin(), g_more_routes.end(), path) != g_more_routes.end();
}

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

void addStreamRoute(const std::string& path) {
    if (!streamRoute(path)) g_more_routes.push_back(path);
}

void installRoutes(SleepBridgeService* service, ReleaseFeed* feed) {
    g_service = service;
    g_feed    = feed;
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
            const bool route  = streamRoute(req->path());
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

    // One command to one bridge: {"sender": "...", "cmd": "...", ...its
    // arguments}. Sent at once when the bridge is online, else on its next
    // hello; the answer shows in GET /api/sleepbridge under its req_id.
    drogon::app().registerHandler(
        "/api/sleepbridge/command",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            const auto body = req->getJsonObject();
            if (!body || !(*body)["sender"].isString())
                return cb(jsonError(drogon::k400BadRequest,
                                    "a command is {\"sender\": name, \"cmd\": name, ...}"));
            const CommandAnswer a = g_service->queueCommand((*body)["sender"].asString(), *body);
            auto resp = drogon::HttpResponse::newHttpJsonResponse(a.body);
            resp->setStatusCode(static_cast<drogon::HttpStatusCode>(a.status));
            cb(resp);
        },
        {drogon::Post});

    // A command still queued (the bridge away), taken back before it is
    // sent: DELETE /api/sleepbridge/command?req_id=N. 409 once sent.
    drogon::app().registerHandler(
        "/api/sleepbridge/command",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            const std::string id = req->getParameter("req_id");
            if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos)
                return cb(jsonError(drogon::k400BadRequest, "req_id is the command's number"));
            const CommandAnswer a = g_service->cancelCommand(std::stoull(id));
            auto resp = drogon::HttpResponse::newHttpJsonResponse(a.body);
            resp->setStatusCode(static_cast<drogon::HttpStatusCode>(a.status));
            cb(resp);
        },
        {drogon::Delete});

    // The public release, for one bridge: {"state", "version", "available",
    // "variant", "size", "error"?}. Read fresh here (the Devices page asks
    // when it loads); its 3 s refreshes read what this kept.
    drogon::app().registerHandler(
        "/api/sleepbridge/release",
        [](const drogon::HttpRequestPtr& req,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            const std::string sender = req->getParameter("sender");
            if (!g_feed) {
                Json::Value v(Json::objectValue);
                v["state"]     = "off";
                v["available"] = false;
                return cb(drogon::HttpResponse::newHttpJsonResponse(v));
            }
            const auto bridges = g_service->status()["bridges"];
            std::string fw;
            Json::Value state(Json::objectValue);
            for (const auto& b : bridges)
                if (b["sender"].asString() == sender) {
                    fw    = b["fw_version"].asString();
                    state = b["state"];
                }
            // A blocking read on a worker of its own: the web's threads stay free.
            std::thread([sender, fw, state, cb = std::move(cb)]() {
                if (g_feed) g_feed->refresh();
                Json::Value v = g_feed ? g_feed->answer(fw, bridgeVariant(state)) : Json::Value(Json::objectValue);
                v["sender"] = sender;
                cb(drogon::HttpResponse::newHttpJsonResponse(v));
            }).detach();
        },
        {drogon::Get});

    // A firmware image to one bridge, over its own stream connection
    // (docs/SLEEP_BRIDGE_LOCAL_STREAM.md, "Updating the bridge"). Three bodies:
    //   application/json     {"source": "release", "confirm"?}: the public
    //                        release for the bridge's board
    //   multipart/form-data  a file field (the image); "confirm" as a field
    //   anything else        the image itself; ?confirm=true
    // While the machine records it answers 409 with the warning unless
    // confirmed.
    using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
    drogon::app().registerHandler(
        "/api/sleepbridge/{sender}/update",
        [](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string& sender) {
            if (!validSender(sender)) return cb(jsonError(drogon::k400BadRequest, "not a bridge name"));
            auto send = [&cb](const CommandAnswer& a) {
                auto resp = drogon::HttpResponse::newHttpJsonResponse(a.body);
                resp->setStatusCode(static_cast<drogon::HttpStatusCode>(a.status));
                cb(resp);
            };
            bool confirm = req->getParameter("confirm") == "true";
            if (req->contentType() == drogon::CT_APPLICATION_JSON) {
                const auto body = req->getJsonObject();
                if (!body || (*body)["source"].asString() != "release")
                    return cb(jsonError(drogon::k400BadRequest,
                                        "a JSON body is {\"source\": \"release\"}; a file is sent as the body"));
                return send(g_service->startReleaseUpdate(sender, confirm || devices::confirmed(*body)));
            }
            std::string image, name = "upload";
            if (req->contentType() == drogon::CT_MULTIPART_FORM_DATA) {
                drogon::MultiPartParser form;
                if (form.parse(req) != 0 || form.getFiles().empty())
                    return cb(jsonError(drogon::k400BadRequest, "the form carries no file"));
                const auto& params = form.getParameters();
                if (params.count("confirm")) confirm = confirm || params.at("confirm") == "true";
                image = std::string(form.getFiles().front().fileContent());
                name  = form.getFiles().front().getFileName();
            } else {
                image = std::string(req->getBody());
            }
            send(g_service->startUpdate(sender, image, confirm, name));
        },
        {drogon::Post});

    // The update as it runs: the job object (devices/OtaJob.h) at the top,
    // and the older shape under "update", kept one release for scripts.
    drogon::app().registerHandler(
        "/api/sleepbridge/{sender}/update",
        [](const drogon::HttpRequestPtr&, Callback&& cb, const std::string& sender) {
            if (!validSender(sender)) return cb(jsonError(drogon::k400BadRequest, "not a bridge name"));
            Json::Value v = g_service->ota(sender);
            if (!v.isObject()) v = Json::Value(Json::objectValue);
            v["ok"]     = true;
            v["update"] = g_service->update(sender);
            cb(drogon::HttpResponse::newHttpJsonResponse(v));
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
    auto e  = std::make_shared<Entry>();
    // Commands (text) and an update's image (binary) go down on the
    // connection itself; send() is safe from any thread.
    std::weak_ptr<drogon::WebSocketConnection> weak = conn;
    e->conv = g_service->open(
        req->attributes()->get<Admission>(kWhoAttr),
        [weak](const std::string& text) {
            const auto c = weak.lock();
            if (!c || !c->connected()) return false;
            c->send(text, drogon::WebSocketMessageType::Text);
            return true;
        },
        [weak](const std::string& bytes) {
            const auto c = weak.lock();
            if (!c || !c->connected()) return false;
            c->send(bytes, drogon::WebSocketMessageType::Binary);
            return true;
        });
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
    if (g_service) g_service->closed(*e->conv);
}

}  // namespace hms_cpap::sleepbridge

#endif  // BUILD_WITH_WEB
