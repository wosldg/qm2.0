#include "network/PredictController.h"

#include "network/ApiResult.h"
#include "predict/Predictor.h"

namespace scenicflow {

void PredictController::testPredict(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    if (!Predictor::instance().isReady()) {
        callback(failed(503, "预测模型未加载（检查 config/tft_model.onnx 与 preprocessing_params.json）"));
        return;
    }
    const std::string spotId = req->getParameter("spotId").empty()
                                   ? "spot_01"
                                   : req->getParameter("spotId");
    auto forecast = Predictor::instance().predict(spotId);
    if (!forecast) {
        callback(failed(409, "历史数据不足 96 个 tick（8 小时）或景点不存在: " + spotId));
        return;
    }

    Json::Value j;
    j["spotId"] = forecast->spotId;
    j["baseTime"] = forecast->baseTime;
    Json::Value ticks(Json::arrayValue);
    for (size_t i = 0; i < forecast->q50.size(); ++i) {
        Json::Value t;
        t["time"] = forecast->tickTimes[i];
        t["q10"] = forecast->q10[i];
        t["q50"] = forecast->q50[i];
        t["q90"] = forecast->q90[i];
        ticks.append(t);
    }
    j["ticks"] = ticks;
    callback(ok(j));
}

} // namespace scenicflow
