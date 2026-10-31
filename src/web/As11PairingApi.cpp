#include "web/As11PairingApi.h"

#include <cctype>

namespace hms_cpap::as11api {

const char* resultCode(as11::Result r) {
    switch (r) {
    case as11::Result::Ok:        return "ok";
    case as11::Result::WaitCode:  return "wait_code";
    case as11::Result::NotPaired: return "not_paired";
    case as11::Result::Connect:   return "connect";
    case as11::Result::RadioOff:  return "radio_off";
    case as11::Result::Verify:    return "verify";
    case as11::Result::Protocol:  return "protocol";
    case as11::Result::Spool:     return "spool";
    }
    return "protocol";
}

std::string normaliseCode(const std::string& typed) {
    std::string out;
    for (char c : typed) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (!std::isdigit(static_cast<unsigned char>(c))) return {};
        out += c;
    }
    if (out.size() < 4 || out.size() > 8) return {};
    return out;
}

namespace {

// Not available: a build without Bluetooth, or airsense11_ble switched off
// (or switched on but not yet saved, so no client exists).
bool unavailable(const IAirSense11Pairing* p, bool compiled, bool enabled, Reply* r) {
    if (compiled && enabled && p) return false;
    r->status = 503;
    r->body["ok"] = false;
    r->body["state"] = "failed";
    r->body["result"] = !compiled ? "not_compiled" : "disabled";
    r->body["error"] = !compiled ? "this build has no Bluetooth support"
                                 : "AirSense 11 over Bluetooth is off: turn it on and save first";
    return true;
}

void failed(Reply* r, as11::Result res, const std::string& why) {
    r->body["ok"] = false;
    r->body["state"] = "failed";
    r->body["result"] = resultCode(res);
    if (!why.empty()) r->body["error"] = why;
}

} // namespace

Reply status(const IAirSense11Pairing* p, bool compiled, bool enabled) {
    Reply r;
    r.body["compiled"] = compiled;
    r.body["enabled"] = enabled;
    r.body["available"] = compiled && enabled && p != nullptr;
    r.body["paired"] = p ? p->paired() : false;
    r.body["pairing"] = p ? p->pairPending() : false;
    return r;
}

Reply start(IAirSense11Pairing* p, bool compiled, bool enabled) {
    Reply r;
    if (unavailable(p, compiled, enabled, &r)) return r;
    const auto res = p->pairStart();
    if (res == as11::Result::WaitCode) {
        r.body["ok"] = true;
        r.body["state"] = "wait_code";
        r.body["result"] = resultCode(res);
        return r;
    }
    failed(&r, res, p->lastError());
    return r;
}

Reply confirm(IAirSense11Pairing* p, bool compiled, bool enabled, const std::string& code) {
    Reply r;
    if (unavailable(p, compiled, enabled, &r)) return r;
    if (!p->pairPending()) {
        r.status = 409;
        r.body["ok"] = false;
        r.body["state"] = "failed";
        r.body["result"] = "no_pairing";
        r.body["error"] = "no pairing in progress: start pairing first";
        return r;
    }
    const std::string c = normaliseCode(code);
    if (c.empty()) {
        // The exchange stays open: a typo is not a reason to make the person
        // start again inside the machine's short window.
        r.status = 400;
        r.body["ok"] = false;
        r.body["state"] = "failed";
        r.body["result"] = "bad_code";
        r.body["error"] = "the code is the digits on the machine's screen";
        return r;
    }
    const auto res = p->pairConfirm(c);
    if (res == as11::Result::Ok) {
        r.body["ok"] = true;
        r.body["state"] = "paired";
        r.body["result"] = resultCode(res);
        return r;
    }
    failed(&r, res, p->lastError());
    return r;
}

Reply cancel(IAirSense11Pairing* p) {
    Reply r;
    if (p) p->pairCancel();
    r.body["ok"] = true;
    r.body["state"] = "cancelled";
    return r;
}

} // namespace hms_cpap::as11api
