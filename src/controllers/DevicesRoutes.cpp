#ifdef BUILD_WITH_WEB
#include "controllers/DevicesRoutes.h"

#include "clients/As11Session.h"
#include "devices/Devices.h"
#include "mule/MuleFirmware.h"
#include "mule/MuleService.h"
#include "services/BurstCollectorService.h"
#include "sleepbridge/ReleaseFeed.h"
#include "sleepbridge/SleepBridgeService.h"
#include "utils/AppConfig.h"
#include "web/As11PairingApi.h"
#include "web/QueryService.h"

#include <drogon/HttpAppFramework.h>

#include <ctime>
#include <iostream>
#include <mutex>

namespace hms_cpap::devices {

namespace {

RouteDeps   g;
std::mutex  g_probe_mu;
Json::Value g_probe{Json::nullValue};

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

drogon::HttpResponsePtr json(const Json::Value& v, int status = 200) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(v);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    return resp;
}

drogon::HttpResponsePtr error(int status, const std::string& why) {
    Json::Value v(Json::objectValue);
    v["ok"]    = false;
    v["error"] = why;
    return json(v, status);
}

bool bleCompiled() { return BurstCollectorService::airSense11BleCompiled(); }

/// "yes" while the latest night in the ledger is live (what /api/realtime
/// reads), "no", or "unknown" when it cannot be read.
std::string ledgerLive() {
    if (!g.query) return "unknown";
    try {
        auto sessions = g.query->getSessions(1, 0);
        if (sessions.empty()) return "no";
        const auto& s0 = sessions[0];
        if (s0.isMember("night_state") && !s0["night_state"].isNull())
            return s0["night_state"].asString() == "live" ? "yes" : "no";
        return s0.isMember("has_live") && std::stoi(s0["has_live"].asString()) > 0 ? "yes" : "no";
    } catch (...) {
        return "unknown";
    }
}

Json::Value sourceStatus() {
    Json::Value v(Json::objectValue);
    const std::string transport = g.config ? g.config->transport : "";
    v["transport"]  = transport;
    v["applicable"] = transport == "ezshare" || transport == "flashair";
    v["url"]        = !g.config ? "" : transport == "flashair" ? g.config->flashair_url : g.config->ezshare_url;
    const auto f = g.burst ? g.burst->cardFacts() : BurstCollectorService::CardFacts{};
    v["reachable"] = f.last_attempt == 0 ? Json::Value(Json::nullValue) : Json::Value(f.answering);
    v["health"]    = f.health;
    v["last_answered"]        = static_cast<Json::Int64>(f.last_answered);
    v["consecutive_failures"] = f.consecutive_failures;
    Json::Value sync(Json::objectValue);
    sync["at"]       = static_cast<Json::Int64>(f.last_attempt);
    sync["answered"] = f.answering;
    v["last_sync"]   = f.last_attempt ? sync : Json::Value(Json::nullValue);
    // The CardFacts shape, for the aggregate.
    v["last_attempt"] = static_cast<Json::Int64>(f.last_attempt);
    v["answering"]    = f.answering;
    {
        std::lock_guard<std::mutex> lk(g_probe_mu);
        if (g_probe.isObject() && g_probe.get("url", "").asString() == v["url"].asString())
            v["last_test"] = g_probe;
    }
    return v;
}

Json::Value oximetryStatus() {
    Json::Value v(Json::objectValue);
    v["compiled"] = bleCompiled();
    v["enabled"]  = g.config && g.config->o2ring.enabled;
    v["mode"]     = g.config ? g.config->o2ring.mode : "";
    v["url"]      = g.config ? g.config->o2ring.mule_url : "";
    OximetryService* svc = g.burst ? g.burst->getOximetryService() : nullptr;
    v["running"] = svc != nullptr;
    if (!svc) return v;
    const auto f = svc->facts();
    v["checked_at"]   = static_cast<Json::Int64>(f.checked_at);
    v["connected"]    = f.checked_at ? Json::Value(f.connected) : Json::Value(Json::nullValue);
    v["battery"]      = f.battery;
    v["last_read_at"] = static_cast<Json::Int64>(f.last_read_at);
    v["last_file"]    = f.last_file;
    Json::Value live(Json::objectValue);
    live["active"] = f.live.active;
    live["valid"]  = f.live.valid;
    live["spo2"]   = f.live.spo2;
    live["hr"]     = f.live.hr;
    v["live"]      = live;
    return v;
}

Json::Value as11Status() {
    auto p = g.burst ? g.burst->getAirSense11Pairing() : nullptr;
    const bool enabled = g.config && g.config->airsense11_ble.enabled;
    Json::Value v = as11api::status(p.get(), bleCompiled(), enabled).body;
    const auto f = g.burst ? g.burst->as11Facts() : BurstCollectorService::As11Facts{};
    v["last_summary_at"]    = static_cast<Json::Int64>(f.summary_at);
    v["last_summary_day"]   = f.summary_day;
    v["last_summary_error"] = f.error;
    v["pull_due"]           = f.pull_due;
    return v;
}

Json::Value currentDevices(bool fresh) {
    DeviceInputs in;
    in.now         = std::time(nullptr);
    in.stream_port = g.stream_port;
    if (g.mule) {
        in.mule = g.mule->status();
        if (g.updater)
            for (const auto& u : in.mule["units"]) {
                const std::string s = u["serial"].asString();
                in.mule_firmware[s] = g.updater->firmware(s, fresh);
            }
    }
    if (g.bridges) {
        if (fresh && g.feed) g.feed->refreshAsync();
        in.sleepbridge = g.bridges->status();
    }
    if (g.config) {
        in.transport     = g.config->transport;
        in.source_url    = in.transport == "flashair" ? g.config->flashair_url : g.config->ezshare_url;
        in.format        = g.config->format;
        in.ezshare_range = g.config->ezshare_range;
        in.o2_enabled    = g.config->o2ring.enabled;
        in.o2_mode       = g.config->o2ring.mode;
        in.o2_url        = g.config->o2ring.mule_url;
        in.as11_enabled  = g.config->airsense11_ble.enabled;
        in.as11_address  = g.config->airsense11_ble.address;
        in.as11_pull_hours = g.config->airsense11.pull_hours > 0 ? g.config->airsense11.pull_hours : 6;
    }
    in.ble_compiled = bleCompiled();
    if (in.transport == "ezshare" || in.transport == "flashair") {
        in.source = sourceStatus();
        if (in.source.isMember("last_test")) in.source_probe = in.source["last_test"];
    }
    if (in.o2_enabled) in.o2 = oximetryStatus();
    if (in.as11_enabled) in.as11 = as11Status();
    in.ledger_live = ledgerLive();
    return buildDevices(in, *g.store);
}

bool listed(const std::string& kind, const std::string& id) {
    const Json::Value all = currentDevices(false);
    for (const auto& d : all["devices"])
        if (d["kind"].asString() == kind && d["id"].asString() == id) return true;
    return false;
}

}  // namespace

void rememberSourceProbe(const Json::Value& probe) {
    std::lock_guard<std::mutex> lk(g_probe_mu);
    g_probe = probe;
}

void installRoutes(const RouteDeps& deps) {
    g = deps;
    if (!g.store) return;

    drogon::app().registerHandler(
        "/api/devices",
        [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            cb(json(currentDevices(req->getParameter("fresh") == "1")));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/devices/{kind}/{id}/name",
        [](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string& kind, const std::string& id) {
            if (!knownKind(kind)) return cb(error(400, "no such kind of device"));
            const auto body = req->getJsonObject();
            if (!body || !(*body)["name"].isString()) return cb(error(400, "the body is {\"name\": \"...\"}"));
            if (!listed(kind, id)) return cb(error(404, "no such device is listed here"));
            std::string why;
            if (!g.store->setName(kind, id, (*body)["name"].asString(), &why)) return cb(error(400, why));
            Json::Value v(Json::objectValue);
            v["ok"]   = true;
            v["name"] = g.store->name(kind, id);
            cb(json(v));
        },
        {drogon::Put});

    drogon::app().registerHandler(
        "/api/devices/{kind}/{id}",
        [](const drogon::HttpRequestPtr&, Callback&& cb, const std::string& kind, const std::string& id) {
            if (!forgettableKind(kind))
                return cb(error(400, "only a Mule & Miner or a Sleep Bridge is forgotten here; the others "
                                     "leave when they are turned off in Settings"));
            if (!listed(kind, id)) return cb(error(404, "no such device is listed here"));
            Json::Value body;
            int         status = 200;
            if (kind == "mule") {
                if (!g.mule) return cb(error(404, "the Mule & Miner stream is off"));
                const auto a = g.mule->forget(id);
                body = a.body, status = a.status;
            } else {
                if (!g.bridges) return cb(error(404, "the Sleep Bridge stream is off"));
                const auto a = g.bridges->forget(id);
                body = a.body, status = a.status;
            }
            if (status == 200) g.store->forget(kind, id, std::time(nullptr));
            cb(json(body, status));
        },
        {drogon::Delete});

    drogon::app().registerHandler(
        "/api/source/status", [](const drogon::HttpRequestPtr&, Callback&& cb) { cb(json(sourceStatus())); },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/oximetry/status", [](const drogon::HttpRequestPtr&, Callback&& cb) { cb(json(oximetryStatus())); },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/airsense11/ble/pull",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            if (!g.burst || !g.burst->requestAs11Pull())
                return cb(error(409, "no AirSense 11 summary pull is set up: turn it on in Settings"));
            Json::Value v(Json::objectValue);
            v["ok"]       = true;
            v["accepted"] = true;
            cb(json(v, 202));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/airsense11/ble/pairing",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            auto p = g.burst ? g.burst->getAirSense11Pairing() : nullptr;
            const bool due = g.burst && g.burst->as11Facts().pull_due;
            const auto r = as11api::forget(p.get(), as11::credentialsPath(AppConfig::dataDir()), due);
            if (r.status == 200)
                std::cout << "AirSense11: the saved pairing was "
                          << (r.body["removed"].asBool() ? "removed" : "already gone") << " (Devices page)"
                          << std::endl;
            cb(json(r.body, r.status));
        },
        {drogon::Delete});

    std::cout << "Devices: /api/devices ready" << std::endl;
}

}  // namespace hms_cpap::devices

#endif  // BUILD_WITH_WEB
