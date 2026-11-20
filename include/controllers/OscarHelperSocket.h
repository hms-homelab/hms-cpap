#pragma once
#ifdef BUILD_WITH_WEB
//
// SDD-081: the Drogon side of the OSCAR helper's connection.
//
//   ws://<host>:8894/oscar/v1/helper   on the Sleep Bridge's port, beside its
//                                      stream and the Mule & Miner's
//
// The upgrade is decided by the Host (trust on first use, D4): a refused one
// answers HTTP 401 with {"error":"has_helper","helper"} or
// {"error":"forgotten"}. Pings every 30 s; a connection silent for 90 s is
// closed (D2), which ends a read waiting on it.
//
#include "services/OscarHelperHost.h"

#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>

namespace hms_cpap::oscar::helper {

/// The route, on the stream port. Must precede app().run(), after the Sleep
/// Bridge's installRoutes (its listener and port split). `host` outlives the app.
void installRoutes(Host* host);

class AdmitFilter : public drogon::HttpFilter<AdmitFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                  drogon::FilterChainCallback&& fccb) override;
};

class HelperSocket : public drogon::WebSocketController<HelperSocket> {
public:
    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                          const drogon::WebSocketMessageType& type) override;
    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/oscar/v1/helper", "hms_cpap::oscar::helper::AdmitFilter");
    WS_PATH_LIST_END
};

}  // namespace hms_cpap::oscar::helper

#endif  // BUILD_WITH_WEB
