#include <drogon/drogon.h>

#include <exception>
#include <filesystem>
#include <string>

#include "core/Config.h"
#include "core/Logger.h"
#include "core/Settings.h"
#include "network/ApiResult.h"
#include "pipeline/DataPipeline.h"
#include "pipeline/SchedulingEngine.h"
#include "pipeline/SimulationEngine.h"
#include "predict/Predictor.h"
#include "security/AuthService.h"
#include "storage/Database.h"
#include "storage/Schema.h"

using namespace drogon;
using namespace scenicflow;

namespace {

std::string bearerToken(const HttpRequestPtr& req) {
    const std::string header = req->getHeader("authorization");
    const std::string prefix = "Bearer ";
    if (header.size() > prefix.size() && header.compare(0, prefix.size(), prefix) == 0) {
        return header.substr(prefix.size());
    }
    return "";
}

// Normalizes trailing slashes so "/api/spots/" matches the "/api/spots" routes.
std::string normalizedPath(const HttpRequestPtr& req) {
    std::string path = req->path();
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

} // namespace

int main(int argc, char** argv) {
    // Base directory used to resolve config/, sql/, data/ and logs/.
    std::string base = ".";
    if (argc > 1) {
        base = argv[1];
        if (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
            base.pop_back();
        }
    }

    try {
        AppConfig cfg = AppConfig::load(base + "/config/app.json");
        appConfig() = cfg;

        std::error_code ec;
        std::filesystem::create_directories(base + "/data", ec);
        std::filesystem::create_directories(base + "/" + cfg.logDir, ec);

        Logger::instance().init(base + "/" + cfg.logDir, cfg.logLevel);
        logInfo("ScenicFlow backend starting (base=" + base + ")");

        if (!Database::instance().open(base + "/" + cfg.dbPath)) {
            logCritical("failed to open database: " + Database::instance().lastError());
            return 1;
        }
        logInfo("database opened: " + cfg.dbPath);

        if (!Schema::init(base + "/sql/schema.sql")) {
            logCritical("failed to initialize database schema");
            return 1;
        }
        logInfo("database schema ready");

        AuthService::instance().seedUsers();
        loadSettings();
        SimulationEngine::instance().init();

        // TFT forecast model; absence of the model files is not fatal (predict
        // endpoints report 503 until config/tft_model.onnx is deployed).
        Predictor::instance().load(base + "/config/tft_model.onnx",
                                   base + "/config/preprocessing_params.json",
                                   base + "/config/sim_config.json");

        // Background stage workers must be running before the pipeline starts feeding them.
        SchedulingEngine::instance().start();
        DataPipeline::instance().start();

        logInfo("loading drogon config: " + base + "/config/config.json");
        app().loadConfigFile(base + "/config/config.json");
        logInfo("drogon config loaded");

        // Auth advice: whitelisted paths pass, everything else needs a valid Bearer token.
        app().registerPreRoutingAdvice(
            [](const HttpRequestPtr& req, AdviceCallback&& callback, AdviceChainCallback&& chain) {
                const std::string method = req->methodString();
                const std::string path = normalizedPath(req);

                // CORS preflight is answered directly (the Vite dev proxy avoids it, but
                // direct cross-origin calls from the browser need it).
                if (method == "OPTIONS") {
                    auto resp = HttpResponse::newHttpResponse();
                    resp->setStatusCode(k204NoContent);
                    resp->addHeader("Access-Control-Allow-Origin", "*");
                    resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
                    resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
                    resp->addHeader("Access-Control-Max-Age", "86400");
                    callback(resp);
                    return;
                }

                if (AuthService::isPublicPath(method, path)) {
                    chain();
                    return;
                }

                auto session = AuthService::instance().validate(bearerToken(req));
                if (!session) {
                    callback(failed(401, "未登录或登录已过期"));
                    return;
                }
                if (!AuthService::canAccess(session->role, method, path)) {
                    callback(failed(403, "当前角色无权限执行该操作"));
                    return;
                }
                chain();
            });

        // Allow cross-origin calls from the Vite dev server.
        app().registerPostHandlingAdvice([](const HttpRequestPtr&, const HttpResponsePtr& resp) {
            resp->addHeader("Access-Control-Allow-Origin", "*");
        });

        logInfo("listening on http://0.0.0.0:8848");
        app().run();

        DataPipeline::instance().stop();
        SchedulingEngine::instance().stop();
        Database::instance().close();
        logInfo("ScenicFlow backend stopped");
        return 0;
    } catch (const std::exception& e) {
        logCritical(std::string("fatal: unhandled exception: ") + e.what());
        return 1;
    } catch (...) {
        logCritical("fatal: unhandled unknown exception");
        return 1;
    }
}
