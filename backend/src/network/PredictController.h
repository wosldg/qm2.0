#pragma once

#include <drogon/HttpController.h>

#include <functional>

namespace scenicflow {

// TFT forecast endpoints. P0-1 ships the smoke-test endpoint used to validate the
// C++ inference chain against the Python side; P0-2 adds the real per-spot forecast API.
class PredictController : public drogon::HttpController<PredictController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PredictController::testPredict, "/api/predict/test", drogon::Get);
    METHOD_LIST_END

    // GET /api/predict/test?spotId=spot_01
    // Runs one inference for the spot and returns the 12-step quantile forecast.
    void testPredict(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace scenicflow
