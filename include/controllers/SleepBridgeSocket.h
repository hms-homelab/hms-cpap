#pragma once
#ifdef BUILD_WITH_WEB
//
// The Drogon side of the Sleep Bridge local stream:
//
//   ws://<host>:8894/sleepbridge/v1/device   the stream (its own listener)
//   GET /api/sleepbridge                     the bridges and their commands,
//                                            on the web port
//   POST /api/sleepbridge/command            one command to one bridge
//
// The stream has a port of its own so it can be published alone (the Home
// Assistant add-on publishes 8894 and keeps the UI behind Ingress). Drogon
// serves every route on every listener, so the split is enforced here: on the
// stream port only the stream route answers, and the stream route answers on
// no other port.
//
// No authentication (local network only). A connection is admitted on its
// X-Coyote-Sender header; without the rebuild in this build every upgrade is
// refused with 501 "not in this build".
//
#include "sleepbridge/SleepBridgeService.h"

#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>

namespace hms_cpap::sleepbridge {

/// Adds the stream listener, the port split, the status route and the
/// socket's hooks. Must precede app().run(). `service` outlives the app.
void installRoutes(SleepBridgeService* service);

/// Another route the stream port serves beside the bridge's (the Mule &
/// Miner's, /mule/v1/device). The port split lets it through on the stream
/// port and refuses it on every other. Must precede app().run().
void addStreamRoute(const std::string& path);

class AdmitFilter : public drogon::HttpFilter<AdmitFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                  drogon::FilterChainCallback&& fccb) override;
};

class DeviceSocket : public drogon::WebSocketController<DeviceSocket> {
public:
    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                          const drogon::WebSocketMessageType& type) override;
    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/sleepbridge/v1/device", "hms_cpap::sleepbridge::AdmitFilter");
    WS_PATH_LIST_END
};

}  // namespace hms_cpap::sleepbridge

#endif  // BUILD_WITH_WEB
