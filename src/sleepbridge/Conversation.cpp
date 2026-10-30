#include "sleepbridge/Conversation.h"

#include <sstream>

namespace hms_cpap::sleepbridge {

namespace {

std::string compact(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

std::optional<Json::Value> parseObject(const std::string& text) {
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::istringstream ss(text);
    if (!Json::parseFromStream(rb, ss, &v, nullptr) || !v.isObject()) return std::nullopt;
    return v;
}

std::string errorFrame(const std::string& why) {
    Json::Value e(Json::objectValue);
    e["t"]     = "error";
    e["error"] = why;
    return compact(e);
}

}  // namespace

std::optional<Admission> admit(const std::function<std::string(const std::string&)>& header,
                               std::string* why) {
    Admission a;
    a.sender  = header("X-Coyote-Sender");
    a.machine = header("X-Coyote-Machine");
    a.chip_id = header("X-Chip-Id");
    if (!validSender(a.sender)) {
        if (why) *why = "X-Coyote-Sender must be 1-64 of [A-Za-z0-9._-], not starting with '.'";
        return std::nullopt;
    }
    if (!validMachine(a.machine)) {
        if (why) *why = "X-Coyote-Machine must be 1-64 of [A-Za-z0-9._-], not starting with '.'";
        return std::nullopt;
    }
    // Shown on the status page, so held to a printable, short shape.
    if (a.chip_id.size() > 64) a.chip_id.resize(64);
    for (auto& c : a.chip_id)
        if (c < 0x20 || c > 0x7e) c = '?';
    return a;
}

Conversation::Conversation(BatchStore& store, Admission who)
    : store_(store), who_(std::move(who)) {}

Effects Conversation::onText(const std::string& frame) {
    Effects e;
    const auto v = parseObject(frame);
    if (!v || !(*v)["t"].isString()) {
        e.texts.push_back(errorFrame("a text frame is one JSON object with a string \"t\""));
        return e;
    }
    const std::string t = (*v)["t"].asString();
    const Json::Value state =
        (*v)["state"].isObject() ? (*v)["state"] : Json::Value(Json::objectValue);
    if (t == "hello") {
        e.hello = true;
        e.state = state;
        Json::Value ack(Json::objectValue);
        ack["t"] = "ack";
        e.texts.push_back(compact(ack));
        return e;
    }
    if (t == "state") {
        e.state = state;
        return e;
    }
    if (t == "result") {
        e.log.push_back("result ignored: no command is sent in local mode");
        return e;
    }
    e.texts.push_back(errorFrame("unknown frame type \"" + t + "\""));
    return e;
}

Effects Conversation::onBinary(const std::string& frame) {
    Effects e;
    const auto h = parseFrameHeader(frame);
    if (!h) {
        e.texts.push_back(errorFrame("a binary frame starts with the 12-byte header"));
        return e;
    }
    if (h->kind == kKindSectorBatch) {
        Stored stored;
        const Answer a = store_.handle(
            who_.sender, who_.machine,
            reinterpret_cast<const uint8_t*>(frame.data()) + kFrameHeaderSize,
            frame.size() - kFrameHeaderSize, &stored);
        Json::Value ack = a.body;
        ack["t"]      = "ack_sectors";
        ack["seq"]    = static_cast<Json::UInt>(h->seq);
        ack["status"] = a.status;
        e.texts.push_back(compact(ack));
        e.banked = a.status == 200 && stored.with_records;
        e.card   = a.status == 200 && stored.card;
        if (a.status != 200)
            e.log.push_back("batch refused (" + std::to_string(a.status) + "): " +
                            a.body.get("error", "").asString());
        return e;
    }
    if (h->kind == kKindLogText || h->kind == kKindRingFile) {
        e.log.push_back("binary kind " + std::to_string(h->kind) +
                        " ignored: not taken in local mode");
        return e;
    }
    e.log.push_back("binary kind " + std::to_string(h->kind) + " ignored");
    return e;
}

}  // namespace hms_cpap::sleepbridge
