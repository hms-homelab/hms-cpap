#include "web/Ds2BleApi.h"

namespace hms_cpap::ds2api {

namespace {

Reply unavailable(bool compiled) {
    Reply r;
    r.status = 409;
    r.body["ok"] = false;
    r.body["result"] = compiled ? "disabled" : "not_compiled";
    r.body["state"] = r.body["result"];
    return r;
}

Reply busy() {
    Reply r;
    r.status = 409;
    r.body["ok"] = false;
    r.body["result"] = "busy";
    r.body["state"] = "busy";
    return r;
}

}  // namespace

Reply status(const Ds2BleReader* r, bool compiled, bool enabled, const Trigger& trigger) {
    Reply out;
    Json::Value& b = out.body;
    b["compiled"] = compiled;
    b["enabled"] = enabled;
    b["backend"] = ble::backendName();
    b["trigger"] = trigger.sources.empty() ? "manual" : "night_end";
    Json::Value srcs(Json::arrayValue);
    for (const auto& s : trigger.sources) srcs.append(s);
    b["night_sources"] = srcs;
    b["machine"] = Json::nullValue;
    b["last_read"] = Json::nullValue;
    b["import_from"] = "";
    b["first_read_done"] = false;
    Json::Value pairing(Json::objectValue);
    pairing["state"] = "idle";
    pairing["code"] = "";
    pairing["seconds_left"] = 0;
    pairing["machine_name"] = "";
    pairing["error"] = "";
    if (!r) {
        b["os_dialog"] = std::string(ble::backendName()) == "corebluetooth";
        b["paired"] = false;
        b["busy"] = false;
        b["pairing"] = pairing;
        return out;
    }
    b["os_dialog"] = r->osShowsCode();
    const auto st = r->store().load();
    b["paired"] = st.paired.has_value();
    b["busy"] = r->busy();
    const auto ps = r->pairStatus();
    pairing["state"] = Ds2BleReader::stateCode(ps.state);
    pairing["code"] = ps.code;
    pairing["seconds_left"] = ps.seconds_left;
    pairing["machine_name"] = ps.machine_name;
    pairing["error"] = ps.error;
    b["pairing"] = pairing;
    if (st.paired) {
        Json::Value m(Json::objectValue);
        m["name"] = st.paired->name;
        m["address"] = st.paired->address;
        m["serial"] = st.paired->serial;
        auto it = st.machines.find(st.paired->serial);
        m["model"] = it != st.machines.end() ? it->second.model : "";
        m["firmware"] = it != st.machines.end() ? it->second.firmware : "";
        m["newest_session"] = it != st.machines.end() ? it->second.newest_session : 0u;
        b["machine"] = m;
        b["import_from"] = st.paired->import_from;
        b["first_read_done"] = it != st.machines.end() && it->second.last_read_at > 0;
        if (it != st.machines.end() && it->second.last_attempt_at > 0) {
            Json::Value lr(Json::objectValue);
            lr["at"] = static_cast<Json::Int64>(it->second.last_read_at);
            lr["attempt_at"] = static_cast<Json::Int64>(it->second.last_attempt_at);
            lr["records"] = it->second.last_records;
            lr["error"] = it->second.last_error;
            b["last_read"] = lr;
        }
    }
    return out;
}

Reply pairStart(Ds2BleReader* r, bool compiled, bool enabled) {
    if (!compiled || !enabled || !r) return unavailable(compiled);
    if (!r->pairStart()) return busy();
    Reply out;
    out.body["ok"] = true;
    out.body["state"] = Ds2BleReader::stateCode(r->pairStatus().state);
    return out;
}

Reply pairConfirm(Ds2BleReader* r, const Json::Value& body) {
    Reply out;
    if (!r || !body.isObject() || !body["match"].isBool()) {
        out.status = 400;
        out.body["ok"] = false;
        out.body["result"] = "bad_request";
        return out;
    }
    if (!r->pairAnswer(body["match"].asBool())) {
        out.status = 409;
        out.body["ok"] = false;
        out.body["result"] = "no_code";
        out.body["state"] = Ds2BleReader::stateCode(r->pairStatus().state);
        return out;
    }
    out.body["ok"] = true;
    out.body["state"] = "pairing";
    return out;
}

Reply pairCancel(Ds2BleReader* r) {
    Reply out;
    if (r) r->pairCancel();
    out.body["ok"] = true;
    out.body["state"] = "cancelled";
    return out;
}

Reply read(Ds2BleReader* r, bool compiled, bool enabled, const std::function<bool()>& start) {
    if (!compiled || !enabled || !r) return unavailable(compiled);
    Reply out;
    if (!r->paired()) {
        out.status = 409;
        out.body["ok"] = false;
        out.body["result"] = "not_paired";
        return out;
    }
    if (r->busy() || !start || !start()) return busy();
    out.status = 202;
    out.body["ok"] = true;
    out.body["result"] = "reading";
    return out;
}

Reply setImportFrom(Ds2BleReader* r, const Json::Value& body) {
    Reply out;
    const std::string date = body.isObject() && body["date"].isString() ? body["date"].asString() : "";
    if (!Ds2Store::localMidnight(date)) {
        out.status = 400;
        out.body["ok"] = false;
        out.body["result"] = "bad_date";
        return out;
    }
    if (!r || !r->paired() || !r->store().setImportFrom(date)) {
        out.status = 409;
        out.body["ok"] = false;
        out.body["result"] = "not_paired";
        return out;
    }
    out.body["ok"] = true;
    out.body["import_from"] = date;
    return out;
}

Reply forget(Ds2BleReader* r) {
    Reply out;
    if (!r) {
        out.body["ok"] = true;
        out.body["unpaired_os"] = false;
        return out;
    }
    if (r->busy()) return busy();
    out.body["ok"] = true;
    out.body["unpaired_os"] = r->forget();
    return out;
}

}  // namespace hms_cpap::ds2api
