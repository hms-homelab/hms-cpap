#ifdef BUILD_WITH_WEB
#include "controllers/SetupWatch.h"

#include "sleepbridge/BridgeSource.h"

#include <chrono>
#include <mutex>
#include <set>

namespace hms_cpap {

namespace {

std::mutex                                g_mu;
std::set<drogon::WebSocketConnectionPtr>  g_watchers;
std::function<Json::Value()>              g_bridges;
std::function<Json::Value()>              g_stream;

Json::Value bridgesNow() {
    if (g_bridges) return g_bridges();
    Json::Value v(Json::objectValue);
    v["available"] = false;
    v["bridges"]   = Json::Value(Json::arrayValue);
    v["message"]   = "The Sleep Bridge receiver is off";
    return v;
}

Json::Value streamNow() {
    if (g_stream) return g_stream();
    Json::Value v(Json::objectValue);
    v["state"] = "unavailable";
    return v;
}

void sendAll(const std::string& frame) {
    std::set<drogon::WebSocketConnectionPtr> to;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        to = g_watchers;
    }
    for (const auto& c : to)
        if (c->connected()) c->send(frame, drogon::WebSocketMessageType::Text);
}

}  // namespace

// Both are set once at start, before app().run(), and read after.
void SetupWatch::setBridgeSource(std::function<Json::Value()> bridges) { g_bridges = std::move(bridges); }
void SetupWatch::setStreamSource(std::function<Json::Value()> stream) { g_stream = std::move(stream); }

void SetupWatch::bridgeEvent(const std::string& event, const std::string& sender) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_watchers.empty()) return;
    }
    sendAll(sleepbridge::bridgeWatchFrame(event, sender, bridgesNow()));
}

void SetupWatch::streamEvent(const Json::Value& stream) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_watchers.empty()) return;
    }
    sendAll(sleepbridge::streamWatchFrame("state", stream));
}

std::string SetupWatch::nightFrame(bool live) {
    return std::string(R"({"t":"night","event":")") + (live ? "live" : "settled") + "\"}";
}

void SetupWatch::nightEvent(bool live) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_watchers.empty()) return;
    }
    sendAll(nightFrame(live));
}

void SetupWatch::handleNewConnection(const drogon::HttpRequestPtr&, const drogon::WebSocketConnectionPtr& conn) {
    // Keeps the connection open through proxies; never news.
    conn->setPingMessage("", std::chrono::seconds(30));
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_watchers.insert(conn);
    }
    conn->send(sleepbridge::bridgeWatchFrame("snapshot", "", bridgesNow()), drogon::WebSocketMessageType::Text);
    conn->send(sleepbridge::streamWatchFrame("snapshot", streamNow()), drogon::WebSocketMessageType::Text);
}

void SetupWatch::handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_watchers.erase(conn);
}

}  // namespace hms_cpap

#endif  // BUILD_WITH_WEB
