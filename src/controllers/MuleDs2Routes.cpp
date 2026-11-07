#ifdef BUILD_WITH_WEB
#include "controllers/MuleDs2Routes.h"

#include <drogon/HttpAppFramework.h>

namespace hms_cpap::mule {

namespace {

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

void answer(Callback& cb, const CommandAnswer& a, int ok_status = 200) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(a.body);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(a.status == 200 ? ok_status : a.status));
    cb(resp);
}

CommandAnswer bad(const std::string& why) {
    CommandAnswer a;
    a.status        = 400;
    a.body["ok"]    = false;
    a.body["error"] = why;
    return a;
}

}  // namespace

void installDs2Routes(MuleDs2* ds2) {
    if (!ds2) return;

    drogon::app().registerHandler(
        "/api/mule/{serial}/ds2/read",
        [ds2](const drogon::HttpRequestPtr&, Callback&& cb, const std::string& serial) {
            if (!validSerial(serial)) return answer(cb, bad("not a unit serial"));
            // The person's own act ("Read now"): the page says it is for after
            // the night, as the direct read's does.
            answer(cb, ds2->readNow(serial), 202);
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/mule/{serial}/ds2/import-from",
        [ds2](const drogon::HttpRequestPtr& req, Callback&& cb, const std::string& serial) {
            if (!validSerial(serial)) return answer(cb, bad("not a unit serial"));
            const auto body = req->getJsonObject();
            if (!body || !(*body)["date"].isString()) return answer(cb, bad("the body is {\"date\": \"YYYY-MM-DD\"}"));
            answer(cb, ds2->setImportFrom(serial, (*body)["date"].asString()));
        },
        {drogon::Post});
}

}  // namespace hms_cpap::mule

#endif  // BUILD_WITH_WEB
