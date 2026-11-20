#ifdef BUILD_WITH_WEB
#include "controllers/LocalModelRoutes.h"

#include "model/LocalModelService.h"
#include "model/ModelFile.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/MultiPart.h>

#include <chrono>
#include <filesystem>

namespace hms_cpap::model {

namespace {

LocalModelService* g_service = nullptr;

using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

drogon::HttpResponsePtr json(const Json::Value& v, int status = 200) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(v);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    return resp;
}

drogon::HttpResponsePtr error(int status, const std::string& why) {
    Json::Value v(Json::objectValue);
    v["ok"] = false;
    v["error"] = why;
    return json(v, status);
}

drogon::HttpResponsePtr accepted() {
    Json::Value v = g_service->status();
    v["ok"] = true;
    return json(v, 202);
}

constexpr const char* kNotInBuild = "The local sleep model is not in this build";

}  // namespace

void installRoutes(LocalModelService* service) {
    g_service = service;

    drogon::app().registerHandler(
        "/api/local-model/status",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            if (!g_service) return cb(error(404, kNotInBuild));
            cb(json(g_service->status()));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/local-model/enable",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            if (!g_service) return cb(error(404, kNotInBuild));
            const auto why = g_service->requestEnable();
            if (!why.empty()) return cb(error(409, why));
            cb(accepted());
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/local-model/disable",
        [](const drogon::HttpRequestPtr&, Callback&& cb) {
            if (!g_service) return cb(error(404, kNotInBuild));
            g_service->disable();
            Json::Value v = g_service->status();
            v["ok"] = true;
            cb(json(v));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/local-model/file",
        [](const drogon::HttpRequestPtr& req, Callback&& cb) {
            if (!g_service) return cb(error(404, kNotInBuild));
            if (const auto why = verifyRefusal(pinnedModel()); !why.empty()) return cb(error(409, why));
            drogon::MultiPartParser parser;
            if (parser.parse(req) != 0 || parser.getFiles().empty())
                return cb(error(400, "No model file was uploaded (multipart field 'file')"));
            // Kept beside the models, so the install is a rename on one disk.
            namespace fs = std::filesystem;
            const fs::path dir = g_service->dir();
            std::error_code ec;
            fs::create_directories(dir, ec);
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto tmp = (dir / ("upload-" + std::to_string(stamp) + ".gguf")).string();
            if (parser.getFiles()[0].saveAs(tmp) != 0)
                return cb(error(500, "The uploaded file could not be kept for checking"));
            const auto why = g_service->installFile(tmp);
            if (!why.empty()) return cb(error(409, why));
            cb(accepted());
        },
        {drogon::Post});
}

}  // namespace hms_cpap::model

#endif  // BUILD_WITH_WEB
