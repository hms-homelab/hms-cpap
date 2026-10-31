#ifdef BUILD_WITH_WEB
#include "controllers/MuleSocket.h"

#include "controllers/SleepBridgeSocket.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/MultiPart.h>

#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>

namespace hms_cpap::mule {

namespace {

const char* const kSerialAttr = "mule_serial";

struct Entry {
    std::mutex                                 mu;   // one frame at a time per connection
    uint64_t                                   id{0};
    std::string                                serial;
    std::weak_ptr<drogon::WebSocketConnection> conn;
    std::time_t                                last_heard{0};
};

MuleService*                               g_service{nullptr};
FirmwareUpdater*                           g_updater{nullptr};
std::mutex                                 g_mu;
std::map<uint64_t, std::shared_ptr<Entry>> g_entries;

drogon::HttpResponsePtr jsonError(drogon::HttpStatusCode code, const std::string& why) {
    Json::Value v(Json::objectValue);
    v["ok"]    = false;
    v["error"] = why;
    auto resp  = drogon::HttpResponse::newHttpJsonResponse(v);
    resp->setStatusCode(code);
    return resp;
}

std::shared_ptr<Entry> find(uint64_t id) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_entries.find(id);
    return it == g_entries.end() ? nullptr : it->second;
}

// Silence closes a connection: due kSilenceSec after the last thing heard.
// The unit reconnects by itself; until it does, it shows offline.
void armSilence(const std::weak_ptr<Entry>& weak, double after) {
    drogon::app().getLoop()->runAfter(after, [weak] {
        const auto e = weak.lock();
        if (!e) return;
        std::lock_guard<std::mutex> lk(e->mu);
        const auto c = e->conn.lock();
        if (!c || !c->connected()) return;
        const std::time_t idle = std::time(nullptr) - e->last_heard;
        if (idle >= kSilenceSec) {
            std::cout << "[mule] " << e->serial << " silent " << idle << " s, closed" << std::endl;
            c->shutdown(drogon::CloseCode::kNormalClosure, "silent");
            return;
        }
        armSilence(weak, static_cast<double>(kSilenceSec - idle));
    });
}

}  // namespace

void installRoutes(MuleService* service, FirmwareUpdater* updater) {
    g_service = service;
    g_updater = updater;
    if (!service) return;
    sleepbridge::addStreamRoute(kRoute);

    using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
    auto answer    = [](Callback& cb, const CommandAnswer& a) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(a.body);
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(a.status));
        cb(resp);
    };

    // The unit's firmware: what it runs, what is published, and the update
    // as it runs.
    drogon::app().registerHandler(
        "/api/mule/{serial}/firmware",
        [](const drogon::HttpRequestPtr&, Callback&& cb, const std::string& serial) {
            if (!g_updater) return cb(jsonError(drogon::k503ServiceUnavailable, "firmware updates are not available"));
            if (!validSerial(serial)) return cb(jsonError(drogon::k400BadRequest, "not a unit serial"));
            cb(drogon::HttpResponse::newHttpJsonResponse(g_updater->firmware(serial)));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/mule/{serial}/update",
        [](const drogon::HttpRequestPtr&, Callback&& cb, const std::string& serial) {
            if (!g_updater) return cb(jsonError(drogon::k503ServiceUnavailable, "firmware updates are not available"));
            if (!validSerial(serial)) return cb(jsonError(drogon::k400BadRequest, "not a unit serial"));
            Json::Value v(Json::objectValue);
            v["ok"]  = true;
            v["job"] = g_updater->job(serial);
            cb(drogon::HttpResponse::newHttpJsonResponse(v));
        },
        {drogon::Get});

    // One update to one unit. Three bodies are taken:
    //   application/json       {"target": "mule"|"miner"|"both"}: from the manifest
    //   multipart/form-data    a file field (the image), target as a field or ?target=
    //   anything else          the image itself as the raw body, ?target=mule|miner
    drogon::app().registerHandler(
        "/api/mule/{serial}/update",
        [answer](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string& serial) {
            if (!g_updater) return cb(jsonError(drogon::k503ServiceUnavailable, "firmware updates are not available"));
            if (!validSerial(serial)) return cb(jsonError(drogon::k400BadRequest, "not a unit serial"));
            std::string target = req->getParameter("target");
            const auto  type   = req->contentType();
            if (type == drogon::CT_APPLICATION_JSON) {
                const auto body = req->getJsonObject();
                if (body && (*body)["target"].isString()) target = (*body)["target"].asString();
                return answer(cb, g_updater->start(serial, target));
            }
            if (type == drogon::CT_MULTIPART_FORM_DATA) {
                drogon::MultiPartParser form;
                if (form.parse(req) != 0 || form.getFiles().empty())
                    return cb(jsonError(drogon::k400BadRequest, "the form carries no file"));
                const auto& params = form.getParameters();
                if (params.count("target")) target = params.at("target");
                const auto& f = form.getFiles().front();
                return answer(cb, g_updater->start(serial, target, std::string(f.fileContent()), f.getFileName()));
            }
            const auto body = req->getBody();
            if (body.empty()) return cb(jsonError(drogon::k400BadRequest, "the body is the image, or {\"target\"}"));
            answer(cb, g_updater->start(serial, target, std::string(body), "upload"));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/mule",
        [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            cb(drogon::HttpResponse::newHttpJsonResponse(g_service ? g_service->status()
                                                                   : Json::Value(Json::objectValue)));
        },
        {drogon::Get});

    // One command to one unit: {"serial": "...", "cmd": "...", ...its
    // arguments}. Sent at once when the unit is online and idle, else when it
    // is; the answer shows in GET /api/mule under its req_id.
    drogon::app().registerHandler(
        "/api/mule/command",
        [](const drogon::HttpRequestPtr& req, std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            const auto body = req->getJsonObject();
            if (!body || !(*body)["serial"].isString())
                return cb(jsonError(drogon::k400BadRequest,
                                    "a command is {\"serial\": unit, \"cmd\": name, ...}"));
            const CommandAnswer a = g_service->queueCommand((*body)["serial"].asString(), *body);
            auto resp = drogon::HttpResponse::newHttpJsonResponse(a.body);
            resp->setStatusCode(static_cast<drogon::HttpStatusCode>(a.status));
            cb(resp);
        },
        {drogon::Post});

    std::cout << "Mule & Miner: listening on ws://0.0.0.0:" << service->options().port << kRoute
              << std::endl;
}

void AdmitFilter::doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                           drogon::FilterChainCallback&& fccb) {
    if (!g_service || req->getLocalAddr().toPort() != g_service->options().port)
        return fcb(jsonError(drogon::k404NotFound, "not found"));
    const std::string serial = req->getHeader("X-Device-Serial");
    if (!validSerial(serial))
        return fcb(jsonError(drogon::k400BadRequest,
                             "X-Device-Serial must be 1-64 of [A-Za-z0-9._-], not starting with '.'"));
    req->attributes()->insert(kSerialAttr, serial);
    fccb();
}

void DeviceSocket::handleNewConnection(const drogon::HttpRequestPtr& req,
                                       const drogon::WebSocketConnectionPtr& conn) {
    if (!g_service || !req->attributes()->find(kSerialAttr)) {
        conn->shutdown(drogon::CloseCode::kViolation, "unadmitted");
        return;
    }
    auto e    = std::make_shared<Entry>();
    e->serial = req->attributes()->get<std::string>(kSerialAttr);
    std::weak_ptr<drogon::WebSocketConnection> weak = conn;
    // The peer address is where the unit's own control server answers.
    e->id = g_service->open(
        e->serial,
        [weak](const std::string& text) {
            const auto c = weak.lock();
            if (!c || !c->connected()) return false;
            c->send(text, drogon::WebSocketMessageType::Text);
            return true;
        },
        req->getPeerAddr().toIp());
    e->conn       = conn;
    e->last_heard = std::time(nullptr);
    conn->setPingMessage("", std::chrono::seconds(kPingSec));
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_entries[e->id] = e;
    }
    conn->setContext(std::make_shared<uint64_t>(e->id));
    armSilence(e, static_cast<double>(kSilenceSec));
}

void DeviceSocket::handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                                    const drogon::WebSocketMessageType& type) {
    if (!conn->hasContext()) return;
    const auto e = find(*conn->getContext<uint64_t>());
    if (!e) return;
    std::lock_guard<std::mutex> lk(e->mu);
    e->last_heard = std::time(nullptr);
    switch (type) {
        case drogon::WebSocketMessageType::Text:   g_service->onText(e->id, message); break;
        case drogon::WebSocketMessageType::Binary: g_service->onBinary(e->id, message); break;
        default: break;   // a ping or pong counts as heard
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
    if (g_service) g_service->closed(id);
}

}  // namespace hms_cpap::mule

#endif  // BUILD_WITH_WEB
