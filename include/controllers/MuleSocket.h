#pragma once
#ifdef BUILD_WITH_WEB
//
// The Drogon side of the Mule & Miner local stream (docs/MULE_LOCAL_STREAM.md):
//
//   ws://<host>:8894/mule/v1/device   the stream, on the Sleep Bridge's port
//   GET  /api/mule                    the units and their commands, web port
//   POST /api/mule/command            one command to one unit, web port
//
// The stream port is the Sleep Bridge's listener; its port split lets this
// route through there and nowhere else (addStreamRoute).
//
// No authentication (local network only). A connection is admitted on its
// X-Device-Serial header alone.
//
#include "mule/MuleService.h"

#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>

namespace hms_cpap::mule {

/// The status and command routes and the socket's hooks. Must precede
/// app().run(), and the stream port must be listening (the Sleep Bridge's
/// installRoutes). `service` outlives the app.
void installRoutes(MuleService* service);

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
    WS_PATH_ADD("/mule/v1/device", "hms_cpap::mule::AdmitFilter");
    WS_PATH_LIST_END
};

}  // namespace hms_cpap::mule

#endif  // BUILD_WITH_WEB
