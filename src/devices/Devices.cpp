#include "devices/Devices.h"

#include <algorithm>
#include <vector>

namespace hms_cpap::devices {

namespace {

constexpr size_t kCommandsShown = 10;

Json::Value entry(const std::string& kind, const std::string& id, const std::string& default_name) {
    Json::Value d(Json::objectValue);
    d["kind"]             = kind;
    d["id"]               = id;
    d["key"]              = kind + "/" + id;
    d["default_name"]     = default_name;
    d["own_name"]         = Json::Value(Json::nullValue);
    d["present"]          = "unknown";
    d["last_heard"]       = 0;
    d["recording"]        = "no";
    d["recording_reason"] = "";
    d["version"]          = "";
    d["forgettable"]      = forgettableKind(kind);
    d["detail"]           = Json::Value(Json::objectValue);
    return d;
}

void ledgerRecording(Json::Value& d, const std::string& live) {
    d["recording"] = live;
    if (live == "yes") d["recording_reason"] = "the night being recorded is still open";
    else if (live == "unknown") d["recording_reason"] = "the night ledger could not be read";
}

/// A command list for the page: the newest kCommandsShown, and a log pull's
/// text moved out to `last_logs` (only the newest one), so a 3 s refresh does
/// not carry every log ever pulled.
void trimCommands(Json::Value& detail) {
    if (!detail["commands"].isArray()) return;
    Json::Value out(Json::arrayValue);
    bool logs_taken = false;
    for (const auto& c : detail["commands"]) {
        if (out.size() >= kCommandsShown) break;
        Json::Value k = c;
        if (k.isMember("logs")) {
            if (!logs_taken && !k["logs"].asString().empty()) {
                Json::Value l(Json::objectValue);
                l["req_id"]      = k["req_id"];
                l["text"]        = k["logs"];
                l["cut"]         = k.get("logs_cut", false);
                l["answered_at"] = k.get("answered_at", 0);
                detail["last_logs"] = l;
                logs_taken = true;
            }
            k["logs_size"] = static_cast<Json::UInt64>(k["logs"].asString().size());
            k.removeMember("logs");
        }
        out.append(k);
    }
    detail["commands"] = out;
}

}  // namespace

bool knownKind(const std::string& kind) {
    return kind == "mule" || kind == "sleepbridge" || kind == "o2ring" || kind == "ezshare" || kind == "flashair" ||
           kind == "as11_ble";
}

bool forgettableKind(const std::string& kind) { return kind == "mule" || kind == "sleepbridge"; }

Json::Value buildDevices(const DeviceInputs& in, DevicesStore& store) {
    std::vector<Json::Value> list;

    // ── Mule & Miner ──
    if (in.mule.isObject() && in.mule["units"].isArray()) {
        for (const auto& u : in.mule["units"]) {
            const std::string serial = u["serial"].asString();
            Json::Value d = entry("mule", serial, serial);
            const bool online = u.get("online", false).asBool();
            const auto heard  = u.get("last_heard", 0).asInt64();
            d["present"]    = online ? "yes" : "no";
            d["last_heard"] = static_cast<Json::Int64>(heard);
            if (u.get("streaming", false).asBool()) {
                d["recording"]        = "yes";
                d["recording_reason"] = "the unit is streaming the machine's night";
            } else if (u.get("night_landing", false).asBool()) {
                d["recording"]        = "yes";
                d["recording_reason"] = "the night the unit streamed has not landed here yet";
            } else if (heard == 0) {
                d["recording"]        = "unknown";
                d["recording_reason"] = "the unit has not been heard from since hms-cpap started";
            }
            d["version"] = u.get("fw", "").asString();
            Json::Value detail = u;
            trimCommands(detail);
            const auto f = in.mule_firmware.find(serial);
            if (f != in.mule_firmware.end() && f->second.isObject()) {
                Json::Value fw(Json::objectValue);
                fw["latest"]           = f->second["latest"];
                fw["update_available"] = f->second["update_available"];
                fw["manifest_error"]   = f->second["manifest"].get("error", "");
                fw["pairing_set"]      = f->second.get("pairing_set", false);
                detail["firmware"]     = fw;
                detail["ota"]          = f->second["ota"];
            } else {
                detail["firmware"] = Json::Value(Json::nullValue);
                detail["ota"]      = Json::Value(Json::nullValue);
            }
            d["detail"] = detail;
            list.push_back(d);
        }
    }

    // ── Sleep Bridge ──
    if (in.sleepbridge.isObject() && in.sleepbridge["bridges"].isArray()) {
        for (const auto& b : in.sleepbridge["bridges"]) {
            const std::string sender = b["sender"].asString();
            Json::Value d = entry("sleepbridge", sender, sender);
            d["present"]          = b.get("online", false).asBool() ? "yes" : "no";
            d["last_heard"]       = b.get("last_heard", 0);
            d["recording"]        = b.get("recording", "unknown").asString();
            d["recording_reason"] = b.get("recording_reason", "").asString();
            d["version"]          = b.get("fw_version", "").asString();
            Json::Value detail = b;
            trimCommands(detail);
            d["detail"] = detail;
            list.push_back(d);
        }
    }

    // ── The O2 ring, read by this host ──
    if (in.o2_enabled) {
        Json::Value d = entry("o2ring", "o2ring", "O2 ring");
        Json::Value detail(Json::objectValue);
        detail["mode"] = in.o2_mode;
        detail["url"]  = in.o2_url;
        if (in.o2_mode != "ble") {
            // D10: read through a bridge; no Bluetooth facts from here.
            detail["through_bridge"] = true;
        } else if (!in.ble_compiled) {
            detail["not_compiled"] = true;
        } else if (in.o2.isObject() && in.o2.get("checked_at", 0).asInt64() > 0) {
            const bool connected = in.o2.get("connected", false).asBool();
            d["present"] = connected ? "yes" : "no";
            const auto checked = in.o2["checked_at"].asInt64();
            const auto read    = in.o2.get("last_read_at", 0).asInt64();
            d["last_heard"]    = static_cast<Json::Int64>(connected ? std::max(checked, read) : read);
            detail["battery"]      = in.o2.get("battery", -1);
            detail["last_read_at"] = in.o2.get("last_read_at", 0);
            detail["last_file"]    = in.o2.get("last_file", "");
            detail["live"]         = in.o2.get("live", Json::Value(Json::nullValue));
            // The ring recording is not the machine recording: shown, never
            // a reason to warn (nothing on this card warns).
            if (in.o2["live"].isObject() && in.o2["live"].get("active", false).asBool()) {
                d["recording"]        = "yes";
                d["recording_reason"] = "the ring is recording";
            }
        }
        d["detail"] = detail;
        list.push_back(d);
    }

    // ── The card, read by this host over WiFi ──
    if (in.transport == "ezshare" || in.transport == "flashair") {
        const bool flashair = in.transport == "flashair";
        Json::Value d = entry(in.transport, "source", flashair ? "FlashAir card" : "ez Share card");
        Json::Value detail(Json::objectValue);
        detail["url"]    = in.source_url;
        detail["format"] = in.format;
        if (!flashair) detail["range"] = in.ezshare_range;
        if (in.source.isObject() && in.source.get("last_attempt", 0).asInt64() > 0) {
            const bool answering = in.source.get("answering", false).asBool();
            d["present"]                  = answering ? "yes" : "no";
            d["last_heard"]               = in.source.get("last_answered", 0);
            detail["health"]              = in.source.get("health", "");
            detail["consecutive_failures"] = in.source.get("consecutive_failures", 0);
            Json::Value sync(Json::objectValue);
            sync["at"]          = in.source["last_attempt"];
            sync["answered"]    = answering;
            detail["last_sync"] = sync;
        } else {
            detail["health"]    = "";
            detail["last_sync"] = Json::Value(Json::nullValue);
        }
        // D11: a Mule & Miner in card mode answers as an ez Share.
        if (!flashair && in.source_probe.isObject() && in.source_probe["bridge_serial"].isString() &&
            !in.source_probe["bridge_serial"].asString().empty()) {
            Json::Value br(Json::objectValue);
            br["serial"]     = in.source_probe["bridge_serial"];
            br["fw"]         = in.source_probe.get("bridge_fw", "");
            detail["bridge"] = br;
            d["version"]     = in.source_probe.get("bridge_fw", "").asString();
        }
        ledgerRecording(d, in.ledger_live);
        d["detail"] = detail;
        list.push_back(d);
    }

    // ── The AirSense 11 over this host's own Bluetooth ──
    if (in.as11_enabled) {
        Json::Value d = entry("as11_ble", in.as11_address.empty() ? "as11" : in.as11_address, "AirSense 11");
        Json::Value detail = in.as11.isObject() ? in.as11 : Json::Value(Json::objectValue);
        detail["address"]    = in.as11_address;
        detail["pull_hours"] = in.as11_pull_hours;
        if (!in.ble_compiled) {
            detail["not_compiled"] = true;
        } else {
            const bool connected = detail.get("connected", false).asBool();
            // long long, not Json::Int64: that is long on Linux and long long
            // on macOS, and std::max wants one type.
            const long long contact = detail.get("last_contact", 0).asInt64();
            const long long pulled  = detail.get("last_summary_at", 0).asInt64();
            const bool pull_ok   = pulled > 0 && detail.get("last_summary_error", "").asString().empty();
            const bool recent    = pull_ok && in.now - pulled < static_cast<long long>(in.as11_pull_hours) * 3600;
            if (connected || recent) d["present"] = "yes";
            else if (contact > 0 || pulled > 0) d["present"] = "no";
            d["last_heard"] = static_cast<Json::Int64>(std::max(contact, pull_ok ? pulled : 0LL));
            if (connected && detail["therapy"].isBool()) {
                d["recording"]        = detail["therapy"].asBool() ? "yes" : "no";
                d["recording_reason"] = detail["therapy"].asBool() ? "the machine is in therapy" : "";
            } else {
                ledgerRecording(d, in.ledger_live);
            }
        }
        d["detail"] = detail;
        list.push_back(d);
    }

    // Forgotten until heard from again; names; then the order.
    std::vector<Json::Value> shown;
    for (auto& d : list) {
        const std::string kind = d["kind"].asString(), id = d["id"].asString();
        if (forgettableKind(kind)) {
            if (const auto at = store.forgottenAt(kind, id)) {
                if (d["last_heard"].asInt64() <= static_cast<Json::Int64>(*at)) continue;
                store.unforget(kind, id);   // back: listed as new
            }
        }
        const std::string own = store.name(kind, id);
        d["name"]     = own.empty() ? d["default_name"].asString() : own;
        d["own_name"] = own.empty() ? Json::Value(Json::nullValue) : Json::Value(own);
        shown.push_back(d);
    }
    std::stable_sort(shown.begin(), shown.end(), [](const Json::Value& a, const Json::Value& b) {
        const bool ah = a["present"].asString() == "yes", bh = b["present"].asString() == "yes";
        if (ah != bh) return ah;
        const auto la = a["last_heard"].asInt64(), lb = b["last_heard"].asInt64();
        if ((la == 0) != (lb == 0)) return lb == 0;
        return la > lb;
    });

    Json::Value out(Json::objectValue);
    Json::Value arr(Json::arrayValue);
    for (auto& d : shown) arr.append(d);
    out["devices"]     = arr;
    out["stream_port"] = in.stream_port;
    out["now"]         = static_cast<Json::Int64>(in.now);
    return out;
}

}  // namespace hms_cpap::devices
