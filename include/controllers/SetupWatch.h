#pragma once
#ifdef BUILD_WITH_WEB
//
// SDD-073: what the Sleep Bridge setup steps wait on, pushed as it happens.
//
//   ws://<host>:<web port>/api/sleepbridge/watch
//
// On connect: a snapshot of the bridges (GET /api/sleepbridge's body) and of
// the CpapDash stream (GET /api/cpapdash/stream's). Then one frame per event:
// a bridge's own hello, its card statement, its session ending, a rebuild
// that wrote files or recognised the machine, and each change of the stream's
// state (sleepbridge/BridgeSource.h has the frames). Nothing is sent on a
// clock; the 30 s ping only keeps the connection open. Nothing is read from
// the client. On the web port only (the stream port's split refuses it).
//
#include <drogon/WebSocketController.h>
#include <json/json.h>

#include <functional>
#include <string>

namespace hms_cpap {

class SetupWatch : public drogon::WebSocketController<SetupWatch> {
public:
    /// Where the snapshots come from; either may be empty (receiver off, no
    /// stream in this build).
    static void setBridgeSource(std::function<Json::Value()> bridges);
    static void setStreamSource(std::function<Json::Value()> stream);

    /// A bridge event (SleepBridgeService::Options::on_event): one frame to
    /// every watcher, carrying the bridges as they are now.
    static void bridgeEvent(const std::string& event, const std::string& sender);
    /// The stream's state changed: one frame to every watcher.
    static void streamEvent(const Json::Value& stream);

    void handleNewMessage(const drogon::WebSocketConnectionPtr&, std::string&&,
                          const drogon::WebSocketMessageType&) override {}
    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/api/sleepbridge/watch");
    WS_PATH_LIST_END
};

}  // namespace hms_cpap

#endif  // BUILD_WITH_WEB
