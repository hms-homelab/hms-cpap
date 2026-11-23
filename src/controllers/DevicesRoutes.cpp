#ifdef BUILD_WITH_WEB
#include "controllers/DevicesRoutes.h"

#include "clients/As11Session.h"
#include "devices/Devices.h"
#include "mule/MuleDs2.h"
#include "mule/MuleFirmware.h"
#include "mule/MuleService.h"
#include "services/BurstCollectorService.h"
#include "services/GarminService.h"
#include "services/OscarCardReader.h"
#include "services/OscarHelperHost.h"
#include "services/SetupService.h"
#include "sleepbridge/ReleaseFeed.h"
#include "sleepbridge/SleepBridgeService.h"
#include "utils/AppConfig.h"
#include "utils/OximetryDevice.h"
#include "web/As11PairingApi.h"
#include "web/ClockApi.h"
#include "web/Ds2BleApi.h"
#include "web/QueryService.h"

#include <drogon/HttpAppFramework.h>

#include <cctype>
#include <ctime>
#include <iostream>
#include <mutex>
#include <thread>

namespace hms_cpap::devices {

namespace {

RouteDeps   g;
std::mutex  g_probe_mu;
Json::Value g_probe{Json::nullValue};
// SDD-080: the last check on save, shown again on the next GET.
std::mutex  g_oscar_mu;
Json::Value g_oscar_check{Json::nullValue};

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

std::string trimmed(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

/// GET /api/oscar: the fields as saved, the program found where this OS's
/// installer puts it, the last check, and the reader (null until an
/// encrypted card was found on the source).
Json::Value oscarView() {
    Json::Value v(Json::objectValue);
    {
        std::lock_guard<std::mutex> lk(g_oscar_mu);
        if (g.config) {
            v["program"]  = g.config->oscar.program;
            v["data_dir"] = g.config->oscar.data_dir;
            v["profile"]  = g.config->oscar.profile;
        }
        v["check"] = g_oscar_check;
    }
    v["found_program"] = oscar::findProgram();
    v["containerised"] = SetupService::isContainerised();
    v["reader"] = g.burst ? g.burst->oscarStatus() : Json::Value(Json::nullValue);
    // SDD-081: the helper, remembered or not, and where it is reached.
    v["helper"] = g.oscar_helper ? g.oscar_helper->status() : Json::Value(Json::nullValue);
    v["stream_port"] = g.stream_port;
    return v;
}

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
    v["last_contact"] = static_cast<Json::Int64>(f.last_contact);
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

Json::Value ds2Status() {
    auto r = g.burst ? g.burst->getDs2Reader() : nullptr;
    const bool enabled = g.config && g.config->ds2_ble.enabled;
    const auto trigger = g.burst ? g.burst->ds2Trigger() : ds2api::Trigger{};
    return ds2api::status(r.get(), bleCompiled(), enabled, trigger).body;
}

Json::Value currentDevices(bool fresh) {
    DeviceInputs in;
    in.now         = std::time(nullptr);
    in.stream_port = g.stream_port;
    if (g.mule) {
        in.mule = g.mule->status();
        // SDD-065: what this host keeps for a unit's DreamStation 2 (the
        // import day, the last read), beside what the unit reports.
        if (g.mule_ds2)
            for (auto& u : in.mule["units"]) {
                const Json::Value& seen = u;   // read without adding keys
                if (seen["state"]["ds2"].isObject()) u["ds2_local"] = g.mule_ds2->view(seen["serial"].asString());
            }
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
        in.ds2_enabled   = g.config->ds2_ble.enabled;   // SDD-064
        in.ds2_address   = g.config->ds2_ble.address;
    }
    in.ble_compiled = bleCompiled();
    if (in.transport == "ezshare" || in.transport == "flashair") {
        in.source = sourceStatus();
        if (in.source.isMember("last_test")) in.source_probe = in.source["last_test"];
    }
    if (in.o2_enabled) in.o2 = oximetryStatus();
    if (in.as11_enabled) in.as11 = as11Status();
    if (in.ds2_enabled) in.ds2 = ds2Status();
    // SDD-078: read for the card only. The page refreshes /api/devices every
    // few seconds, so this is never a Garmin event (D2).
    in.garmin_enabled = g.config && g.config->garmin.enabled;
    if (in.garmin_enabled && g.garmin) in.garmin = g.garmin->status();
    in.ledger_live = ledgerLive();
    if (g.burst) in.oscar = g.burst->oscarStatus();   // SDD-080 D7
    if (g.oscar_helper) in.oscar_helper = g.oscar_helper->status();   // SDD-081 D4
    Json::Value out = buildDevices(in, *g.store);
    // SDD-074 D10 / decision 10: the setting lives on each machine's card,
    // keyed by the device id its nights are stored under; the ring's card
    // carries the "Ring clock" (decision 8).
    if (g.db && g.config) {
        const long long now = static_cast<long long>(std::time(nullptr));
        for (auto& d : out["devices"]) {
            const bool ring = d["kind"].asString() == "o2ring";
            d["clock"] = clockapi::view(*g.db, ring ? std::string(kOximetryDeviceId) : g.config->device_id,
                                        now, !ring);
        }
    }
    return out;
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

    // SDD-074: the clock setting. Registered before {kind}/{id} would match.
    drogon::app().registerHandler(
        "/api/clock/zones", [](const drogon::HttpRequestPtr&, Callback&& cb) { cb(json(clockapi::zones())); },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/devices/clock",
        [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!g.db || !g.config) return cb(error(503, "no database"));
            const auto body = req->getJsonObject();
            if (!body || !(*body)["device_id"].isString() || !(*body)["setting"].isString())
                return cb(error(400, "the body is {\"device_id\": \"...\", \"setting\": \"...\"}"));
            const std::string device = (*body)["device_id"].asString();
            const bool ring = device == kOximetryDeviceId;
            if (!ring && device != g.config->device_id) return cb(error(404, "no such machine is stored here"));
            const auto r = clockapi::set(*g.db, device, (*body)["setting"].asString(),
                                         static_cast<long long>(std::time(nullptr)), !ring);
            if (r.status == 200)
                std::cout << "Clock: " << device << " is now read on "
                          << (r.body["clock"]["setting"].asString().empty()
                                  ? std::string("this computer's clock")
                                  : r.body["clock"]["setting"].asString())
                          << " (Devices page)" << std::endl;
            cb(json(r.body, r.status));
        },
        {drogon::Put});

    drogon::app().registerHandler(
        "/api/devices/clock/reread",
        [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!g.config) return cb(error(503, "not configured"));
            const auto body = req->getJsonObject();
            const std::string device =
                body && (*body)["device_id"].isString() ? (*body)["device_id"].asString() : g.config->device_id;
            if (device != g.config->device_id) return cb(error(404, "no such machine is stored here"));
            if (!g.reread) return cb(error(503, "no archive of this machine's files to read again"));
            const Json::Value r = g.reread(device);
            cb(json(r, r.get("ok", false).asBool() ? 202 : 500));
        },
        {drogon::Post});

    // SDD-078 D4: the Garmin card.
    drogon::app().registerHandler(
        "/api/garmin/fetch",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            if (!g.garmin || !g.garmin->fetchNow())
                return cb(error(409, "Garmin is not set up: turn it on and enter the account in Settings"));
            Json::Value v(Json::objectValue);
            v["ok"]       = true;
            v["accepted"] = true;
            cb(json(v, 202));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/garmin/mfa",
        [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            const auto body = req->getJsonObject();
            if (!body || !(*body)["code"].isString()) return cb(error(400, "the body is {\"code\": \"...\"}"));
            std::string code;
            for (char c : (*body)["code"].asString())
                if (c != ' ' && c != '-') code += c;
            if (!g.garmin || !g.garmin->submitMfaCode(code))
                return cb(error(409, "no Garmin sign-in is waiting for a code"));
            Json::Value v(Json::objectValue);
            v["ok"]       = true;
            v["accepted"] = true;
            cb(json(v, 202));
        },
        {drogon::Post});

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

    // #108: "Pair a different machine": the pairing and the saved machine
    // (address, serial) go, so the next pairing takes the machine it reaches.
    drogon::app().registerHandler(
        "/api/airsense11/ble/machine",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            auto p = g.burst ? g.burst->getAirSense11Pairing() : nullptr;
            const bool due = g.burst && g.burst->as11Facts().pull_due;
            const auto r = as11api::forget(p.get(), as11::credentialsPath(AppConfig::dataDir()), due, true);
            if (r.status == 200) {
                if (g.burst) g.burst->forgetAs11Machine();
                std::cout << "AirSense11: the pairing and the saved machine were removed, to pair a different "
                             "machine (Settings)"
                          << std::endl;
            }
            cb(json(r.body, r.status));
        },
        {drogon::Delete});

    // #67: "Reset Bluetooth": forget the machine's OS object and power-cycle
    // the adapter, on demand.
    drogon::app().registerHandler(
        "/api/airsense11/ble/reset",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            auto p = g.burst ? g.burst->getAirSense11Pairing() : nullptr;
            const bool enabled = g.config && g.config->airsense11_ble.enabled;
            const auto r = as11api::resetBluetooth(p.get(), bleCompiled(), enabled);
            cb(json(r.body, r.status));
        },
        {drogon::Post});

    // ── SDD-080: a DreamStation 2 card read through the user's OSCAR ──
    drogon::app().registerHandler(
        "/api/oscar",
        [](const drogon::HttpRequestPtr&, Callback&& cb) { cb(json(oscarView())); },
        {drogon::Get});

    // D7: checked on save. The check runs the program (--help), so it is
    // answered from its own thread, never the web server's.
    drogon::app().registerHandler(
        "/api/oscar",
        [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!g.config) return cb(error(500, "no configuration"));
            const auto body = req->getJsonObject();
            if (!body || !body->isObject()) return cb(error(400, "the body is {\"program\",\"data_dir\",\"profile\"}"));
            oscar::Settings s;
            s.program  = trimmed(body->get("program", "").asString());
            s.data_dir = trimmed(body->get("data_dir", "").asString());
            s.profile  = trimmed(body->get("profile", oscar::kDefaultProfile).asString());
            std::thread([s, cb = std::move(cb)] {
                oscar::SystemProcessRunner runner;
                const Json::Value check = oscar::OscarCardReader::check(s, runner, SetupService::isContainerised());
                {
                    std::lock_guard<std::mutex> lk(g_oscar_mu);
                    g.config->oscar.program  = s.program;
                    g.config->oscar.data_dir = s.data_dir;
                    g.config->oscar.profile  = s.profile;
                    if (!g.config_path.empty()) g.config->save(g.config_path);
                    g_oscar_check = check;
                }
                if (g.burst) g.burst->setOscarSettings(s);
                Json::Value v = oscarView();
                v["ok"] = true;
                cb(json(v));
            }).detach();
        },
        {drogon::Put});

    drogon::app().registerHandler(
        "/api/oscar/read",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            if (!g.burst || !g.burst->requestOscarRead())
                return cb(error(409, "no encrypted DreamStation 2 card has been found on the source yet"));
            Json::Value v = oscarView();
            v["ok"] = true;
            cb(json(v, 202));
        },
        {drogon::Post});

    // SDD-081 D4: Forget. The next helper to connect with no token is
    // remembered; the forgotten PC, still on, is refused and says so.
    drogon::app().registerHandler(
        "/api/oscar/helper",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            if (!g.oscar_helper || !g.oscar_helper->forget()) return cb(error(404, "no OSCAR helper is remembered"));
            Json::Value v = oscarView();
            v["ok"] = true;
            cb(json(v));
        },
        {drogon::Delete});

    std::cout << "Devices: /api/devices ready" << std::endl;
}

}  // namespace hms_cpap::devices

#endif  // BUILD_WITH_WEB
