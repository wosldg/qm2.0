#include "predict/Predictor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>

#include <json/json.h>
#include <onnxruntime_cxx_api.h>

#include "core/Logger.h"
#include "core/TimeUtil.h"
#include "network/Mappers.h"
#include "storage/Database.h"

namespace scenicflow {
namespace {

constexpr int kEncoderLen = 96;   // 8h of 5-min ticks
constexpr int kDecoderLen = 12;   // 60 min ahead
constexpr int kCatCount = 3;      // month, dayofweek, is_weekend
constexpr int kContCount = 5;     // season_factor, current_visitors, arrival_rate,
                                  // departure_rate, utilization
constexpr int kTickMinutes = 5;

// ---- civil-date helpers (Howard Hinnant's algorithms, no timezone involved) ----

long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
}

void civilFromDays(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
}

struct TickTime {
    int year;
    unsigned month;
    unsigned day;
    int hour;
    int minute;
    // pandas-compatible: Monday=0 ... Sunday=6
    int dayOfWeek() const {
        const long long days = daysFromCivil(year, month, day);
        const int sunday0 = static_cast<int>((days + 4) % 7 + 7) % 7; // Sunday=0
        return (sunday0 + 6) % 7;
    }
    int isWeekend() const { return dayOfWeek() >= 5 ? 1 : 0; }
    std::string str() const {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02d:%02d:00", year, month, day,
                      hour, minute);
        return buf;
    }
};

bool parseTickTime(const std::string& s, TickTime& out) {
    // expects "YYYY-MM-DD HH:MM:SS"
    if (s.size() < 16) return false;
    try {
        out.year = std::stoi(s.substr(0, 4));
        out.month = static_cast<unsigned>(std::stoul(s.substr(5, 2)));
        out.day = static_cast<unsigned>(std::stoul(s.substr(8, 2)));
        out.hour = std::stoi(s.substr(11, 2));
        out.minute = std::stoi(s.substr(14, 2));
    } catch (...) {
        return false;
    }
    return out.month >= 1 && out.month <= 12 && out.day >= 1 && out.day <= 31;
}

TickTime addMinutes(const TickTime& t, int minutes) {
    const long long total =
        daysFromCivil(t.year, t.month, t.day) * 1440 + t.hour * 60 + t.minute + minutes;
    const long long days = total / 1440;
    const int rem = static_cast<int>(total % 1440);
    TickTime out{};
    civilFromDays(days, out.year, out.month, out.day);
    out.hour = rem / 60;
    out.minute = rem % 60;
    return out;
}

struct NormParam {
    double mean = 0.0;
    double std = 1.0;
    float normalize(double v) const {
        return static_cast<float>((v - mean) / std);
    }
};

} // namespace

struct Predictor::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "scenicflow-predictor"};
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputNames;  // session order
    std::string outputName = "prediction";

    // preprocessing_params.json
    std::unordered_map<std::string, NormParam> norm; // keyed by column name
    NormParam denormTarget;                          // current_visitors stats
    // categorical_maps: column -> (string value -> code)
    std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> catMaps;
    // sim_config.json season_factors: month (1-12) -> factor
    double seasonFactors[13] = {0};

    // sim_config.json tft section
    double uncertaintyWidthThreshold = 0.3;
    int predictIntervalSeconds = 300;
    int tickMinutes = 5;

    bool ready = false;

    double seasonFactor(unsigned month) const {
        if (month >= 1 && month <= 12 && seasonFactors[month] > 0) return seasonFactors[month];
        return 1.0;
    }

    int64_t encodeCat(const std::string& column, const std::string& value) const {
        const auto colIt = catMaps.find(column);
        if (colIt == catMaps.end()) return 0;
        const auto valIt = colIt->second.find(value);
        if (valIt == colIt->second.end()) {
            return 0; // unseen category → fall back to code 0
        }
        return valIt->second;
    }
};

Predictor::~Predictor() = default;

Predictor& Predictor::instance() {
    static Predictor inst;
    return inst;
}

bool Predictor::load(const std::string& onnxPath, const std::string& prepParamsPath,
                     const std::string& simConfigPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_impl) m_impl = std::make_unique<Impl>();
    auto& impl = *m_impl;
    impl.ready = false;

    // ---- preprocessing_params.json ----
    {
        std::ifstream in(prepParamsPath);
        if (!in.is_open()) {
            logError("Predictor: cannot open " + prepParamsPath);
            return false;
        }
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        if (!Json::parseFromStream(builder, in, &root, &errs)) {
            logError("Predictor: bad json in " + prepParamsPath + ": " + errs);
            return false;
        }
        for (const auto& col : root["norm_params"].getMemberNames()) {
            NormParam p;
            p.mean = root["norm_params"][col].get("mean", 0.0).asDouble();
            p.std = root["norm_params"][col].get("std", 1.0).asDouble();
            if (std::fabs(p.std) < 1e-12) p.std = 1.0;
            impl.norm[col] = p;
        }
        impl.denormTarget.mean = root["denormalize_target"].get("mean", 0.0).asDouble();
        impl.denormTarget.std = root["denormalize_target"].get("std", 1.0).asDouble();
        if (root.isMember("categorical_maps")) {
            for (const auto& col : root["categorical_maps"].getMemberNames()) {
                std::unordered_map<std::string, int64_t> m;
                for (const auto& v : root["categorical_maps"][col].getMemberNames()) {
                    m[v] = root["categorical_maps"][col][v].asInt64();
                }
                impl.catMaps[col] = std::move(m);
            }
        }
        if (impl.catMaps.empty()) {
            logError("Predictor: categorical_maps missing in preprocessing_params.json "
                     "(retrain with the current train_tft.py)");
            return false;
        }
    }

    // ---- sim_config.json (season factors + TFT config) ----
    {
        std::ifstream in(simConfigPath);
        if (!in.is_open()) {
            logError("Predictor: cannot open " + simConfigPath);
            return false;
        }
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        if (!Json::parseFromStream(builder, in, &root, &errs)) {
            logError("Predictor: bad json in " + simConfigPath + ": " + errs);
            return false;
        }
        for (const auto& k : root["season_factors"].getMemberNames()) {
            const int month = std::atoi(k.c_str());
            if (month >= 1 && month <= 12) {
                impl.seasonFactors[month] = root["season_factors"][k].asDouble();
            }
        }
        // TFT 配置：不确定度阈值、推理间隔、tick 分钟数
        if (root.isMember("tft")) {
            const auto& tft = root["tft"];
            if (tft.isMember("uncertainty_width_threshold"))
                impl.uncertaintyWidthThreshold = tft["uncertainty_width_threshold"].asDouble();
            if (tft.isMember("predict_interval_seconds"))
                impl.predictIntervalSeconds = tft["predict_interval_seconds"].asInt();
            if (tft.isMember("tick_minutes"))
                impl.tickMinutes = tft["tick_minutes"].asInt();
        }
    }

    // ---- onnx session ----
    try {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(2);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        std::wstring wpath(onnxPath.begin(), onnxPath.end());
        impl.session = std::make_unique<Ort::Session>(impl.env, wpath.c_str(), opts);
#else
        impl.session = std::make_unique<Ort::Session>(impl.env, onnxPath.c_str(), opts);
#endif
        Ort::AllocatorWithDefaultOptions alloc;
        const size_t n = impl.session->GetInputCount();
        impl.inputNames.clear();
        for (size_t i = 0; i < n; ++i) {
            auto name = impl.session->GetInputNameAllocated(i, alloc);
            impl.inputNames.emplace_back(name.get());
        }
    } catch (const Ort::Exception& e) {
        logError(std::string("Predictor: onnx load failed: ") + e.what());
        impl.session.reset();
        return false;
    }

    impl.ready = true;
    logInfo("Predictor: model loaded (" + onnxPath + "), inputs=" +
            std::to_string(impl.inputNames.size()));
    return true;
}

bool Predictor::isReady() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_impl && m_impl->ready;
}

std::optional<SpotForecast> Predictor::predict(const std::string& spotId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_impl || !m_impl->ready) return std::nullopt;
    auto& impl = *m_impl;

    // ---- 1. 拉取该景点足够多的 flow_record，按 5min 分桶聚合 ----
    // 取最新 3000 条（seed 数据约 1008 条/景点，SimulationEngine 每秒写，
    // 3000 条 ≈ 覆盖多日），按 5min 桶取每个桶内最后一条作为桶值。
    auto rawRows = Database::instance().queryParams(
        "SELECT record_time, current_visitors, arrival_rate, departure_rate "
        "FROM flow_record WHERE spot_id=? AND is_deleted=0 "
        "ORDER BY record_time DESC, id DESC LIMIT 3000",
        {spotId});
    if (rawRows.empty()) {
        logWarn("Predictor: spot " + spotId + " has no flow_record at all");
        return std::nullopt;
    }

    // 按 5min 分桶（time 向下取整到 5min）
    // 桶 key = (yyyy-mm-dd hh:mm)，mm 强制为 00/05/.../55
    struct Bucket {
        std::string bucketTime;
        std::string lastRecordTime;
        double cur = 0, arr = 0, dep = 0;
    };
    std::map<std::string, Bucket> buckets; // key 有序 → 自动按时间排序

    for (const auto& row : rawRows) {
        const std::string& rt = row.at("record_time");
        TickTime t{};
        if (!parseTickTime(rt, t)) continue;
        // 向下取整到 5 分钟
        const int flooredMin = (t.minute / 5) * 5;
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02d %02d:%02d:00",
                      t.year, t.month, t.day, t.hour, flooredMin);
        const std::string key(buf);
        auto& b = buckets[key];
        b.bucketTime = key;
        b.lastRecordTime = rt;
        b.cur = std::atof(row.at("current_visitors").c_str());
        b.arr = std::atof(row.at("arrival_rate").c_str());
        b.dep = std::atof(row.at("departure_rate").c_str());
    }

    if (static_cast<int>(buckets.size()) < kEncoderLen) {
        logWarn("Predictor: spot " + spotId + " has only " +
                std::to_string(buckets.size()) + " 5min buckets (< 96), skip prediction");
        return std::nullopt;
    }

    // 取最新的 96 个桶
    std::vector<Bucket> histBuckets;
    auto it = buckets.end();
    std::advance(it, -kEncoderLen);
    for (; it != buckets.end(); ++it) histBuckets.push_back(it->second);

    auto capRows = Database::instance().queryParams(
        "SELECT instant_capacity FROM scenic_spot WHERE spot_id=?", {spotId});
    if (capRows.empty()) {
        logWarn("Predictor: unknown spot " + spotId);
        return std::nullopt;
    }
    const double capacity = std::max(1.0, std::atof(capRows[0]["instant_capacity"].c_str()));

    // ---- 2. 组装 encoder tensor（96 个 5min 桶）----
    std::vector<int64_t> encoderCat(kEncoderLen * kCatCount);
    std::vector<float> encoderCont(kEncoderLen * kContCount);
    std::vector<TickTime> histTimes(kEncoderLen);

    for (int i = 0; i < kEncoderLen; ++i) {
        TickTime t{};
        if (!parseTickTime(histBuckets[i].bucketTime, t)) {
            // 桶 key 是 YYYY-MM-DD HH:MM:SS，可以直接 parse
            logWarn("Predictor: bad bucket_time '" + histBuckets[i].bucketTime + "'");
            return std::nullopt;
        }
        histTimes[i] = t;
        encoderCat[i * kCatCount + 0] = impl.encodeCat("month", std::to_string(t.month));
        encoderCat[i * kCatCount + 1] = impl.encodeCat("dayofweek", std::to_string(t.dayOfWeek()));
        encoderCat[i * kCatCount + 2] = impl.encodeCat("is_weekend", std::to_string(t.isWeekend()));

        const double cur = histBuckets[i].cur;
        const double arr = histBuckets[i].arr;
        const double dep = histBuckets[i].dep;
        encoderCont[i * kContCount + 0] = impl.norm["season_factor"].normalize(impl.seasonFactor(t.month));
        encoderCont[i * kContCount + 1] = impl.norm["current_visitors"].normalize(cur);
        encoderCont[i * kContCount + 2] = impl.norm["arrival_rate"].normalize(arr);
        encoderCont[i * kContCount + 3] = impl.norm["departure_rate"].normalize(dep);
        encoderCont[i * kContCount + 4] = impl.norm["utilization"].normalize(cur / capacity);
    }

    // ---- 3. 组装 decoder tensor（12 个未来 5min tick）----
    std::vector<int64_t> decoderCat(kDecoderLen * kCatCount);
    std::vector<float> decoderCont(kDecoderLen * kContCount, 0.0f);
    SpotForecast result;
    result.spotId = spotId;
    result.baseTime = histTimes.back().str();
    for (int j = 0; j < kDecoderLen; ++j) {
        const TickTime t = addMinutes(histTimes.back(), (j + 1) * kTickMinutes);
        result.tickTimes.push_back(t.str());
        decoderCat[j * kCatCount + 0] = impl.encodeCat("month", std::to_string(t.month));
        decoderCat[j * kCatCount + 1] = impl.encodeCat("dayofweek", std::to_string(t.dayOfWeek()));
        decoderCat[j * kCatCount + 2] = impl.encodeCat("is_weekend", std::to_string(t.isWeekend()));
        decoderCont[j * kContCount + 0] =
            impl.norm["season_factor"].normalize(impl.seasonFactor(t.month));
    }

    std::vector<int64_t> encoderLengths{kEncoderLen};
    std::vector<int64_t> decoderLengths{kDecoderLen};
    std::vector<float> targetScale{0.0f, 1.0f};

    // ---- 4. run onnx ----
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    const int64_t encCatDims[3] = {1, kEncoderLen, kCatCount};
    const int64_t encContDims[3] = {1, kEncoderLen, kContCount};
    const int64_t lenDims[1] = {1};
    const int64_t decCatDims[3] = {1, kDecoderLen, kCatCount};
    const int64_t decContDims[3] = {1, kDecoderLen, kContCount};
    const int64_t scaleDims[2] = {1, 2};

    std::unordered_map<std::string, Ort::Value> tensorByName;
    tensorByName.emplace("encoder_cat", Ort::Value::CreateTensor<int64_t>(
        memInfo, encoderCat.data(), encoderCat.size(), encCatDims, 3));
    tensorByName.emplace("encoder_cont", Ort::Value::CreateTensor<float>(
        memInfo, encoderCont.data(), encoderCont.size(), encContDims, 3));
    tensorByName.emplace("encoder_lengths", Ort::Value::CreateTensor<int64_t>(
        memInfo, encoderLengths.data(), encoderLengths.size(), lenDims, 1));
    tensorByName.emplace("decoder_cat", Ort::Value::CreateTensor<int64_t>(
        memInfo, decoderCat.data(), decoderCat.size(), decCatDims, 3));
    tensorByName.emplace("decoder_cont", Ort::Value::CreateTensor<float>(
        memInfo, decoderCont.data(), decoderCont.size(), decContDims, 3));
    tensorByName.emplace("decoder_lengths", Ort::Value::CreateTensor<int64_t>(
        memInfo, decoderLengths.data(), decoderLengths.size(), lenDims, 1));
    tensorByName.emplace("target_scale", Ort::Value::CreateTensor<float>(
        memInfo, targetScale.data(), targetScale.size(), scaleDims, 2));

    std::vector<const char*> inputNamesC;
    std::vector<Ort::Value> inputValues;
    inputNamesC.reserve(impl.inputNames.size());
    inputValues.reserve(impl.inputNames.size());
    for (const auto& name : impl.inputNames) {
        auto it = tensorByName.find(name);
        if (it == tensorByName.end()) {
            logError("Predictor: model expects unexpected input '" + name + "'");
            return std::nullopt;
        }
        inputNamesC.push_back(it->first.c_str());
        inputValues.push_back(std::move(it->second));
    }
    const char* outputNamesC[] = {impl.outputName.c_str()};

    Ort::Value output = Ort::Value(nullptr);
    try {
        auto outputs = impl.session->Run(Ort::RunOptions{nullptr}, inputNamesC.data(),
                                         inputValues.data(), inputValues.size(),
                                         outputNamesC, 1);
        output = std::move(outputs.front());
    } catch (const Ort::Exception& e) {
        logError(std::string("Predictor: inference failed: ") + e.what());
        return std::nullopt;
    }

    const float* out = output.GetTensorData<float>();
    const auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 3 || shape[1] != kDecoderLen || shape[2] != 3) {
        logError("Predictor: unexpected output shape");
        return std::nullopt;
    }

    // ---- 5. denormalize + compute uncertainty_width ----
    for (int j = 0; j < kDecoderLen; ++j) {
        const double q10 = impl.denormTarget.std * out[j * 3 + 0] + impl.denormTarget.mean;
        const double q50 = impl.denormTarget.std * out[j * 3 + 1] + impl.denormTarget.mean;
        const double q90 = impl.denormTarget.std * out[j * 3 + 2] + impl.denormTarget.mean;
        result.q10.push_back(q10);
        result.q50.push_back(q50);
        result.q90.push_back(q90);
        // uncertainty_width = (q90 - q10) / max(q50, 1)
        const double uw = std::abs(q50) < 1.0 ? 0.0 : (q90 - q10) / q50;
        result.uncertaintyWidth.push_back(uw);
    }
    return result;
}

double Predictor::uncertaintyWidthThreshold() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_impl ? m_impl->uncertaintyWidthThreshold : 0.3;
}

PredictAllResult Predictor::predictAll() {
    PredictAllResult result;
    result.predictTime = nowString();

    std::vector<Row> spotRows;
    try {
        spotRows = Database::instance().query(
            "SELECT spot_id FROM scenic_spot WHERE status=1 ORDER BY id");
    } catch (...) {
        logWarn("Predictor: failed to list spots for predictAll");
        return result;
    }
    result.spotCount = static_cast<int>(spotRows.size());

    for (const auto& r : spotRows) {
        const std::string spotId = field(r, "spot_id");
        auto forecast = predict(spotId);
        if (!forecast) {
            ++result.skippedCount;
            continue;
        }
        ++result.successCount;

        // P0-2: write per-spot 5MIN forecast rows into flow_forecast
        // P0-4: write prediction_log rows
        writeForecastRows(*forecast, result.predictTime);
        writePredictionLogRows(*forecast);
    }

    logInfo("Predictor::predictAll done: " + std::to_string(result.successCount) +
            "/" + std::to_string(result.spotCount) + " spots predicted, " +
            std::to_string(result.skippedCount) + " skipped");
    return result;
}

// ---- P0-2 persist helpers ----

void Predictor::writeForecastRows(const SpotForecast& fc, const std::string& predictTime) {
    // 每个 tick 写一行 flow_forecast，granularity='5MIN'，spot_id 非空
    Database::instance().exec("BEGIN");
    for (size_t i = 0; i < fc.tickTimes.size(); ++i) {
        const std::string& tickTime = fc.tickTimes[i];
        // 解析 tickTime 拿 forecast_date 和 hour
        const std::string dateStr = tickTime.substr(0, 10);
        const int hour = tickTime.size() >= 13
                              ? std::stoi(tickTime.substr(11, 2))
                              : 0;
        const int tickOffset = tickTime.size() >= 16
                                   ? std::stoi(tickTime.substr(14, 2)) / 5
                                   : 0;

        Database::instance().execParams(
            "INSERT OR REPLACE INTO flow_forecast(forecast_date,hour,predicted_visitors,predicted_level,"
            "spot_id,granularity,tick_offset,q10,q50,q90,uncertainty_width,created_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            {dateStr,
             std::to_string(hour),
             std::to_string(static_cast<int>(std::lround(fc.q50[i]))),
             std::string("PREDICTED"),
             fc.spotId,
             std::string("5MIN"),
             std::to_string(tickOffset),
             std::to_string(fc.q10[i]),
             std::to_string(fc.q50[i]),
             std::to_string(fc.q90[i]),
             std::to_string(fc.uncertaintyWidth[i]),
             predictTime});
    }
    Database::instance().exec("COMMIT");
}

void Predictor::writePredictionLogRows(const SpotForecast& fc) {
    // 每个 tick 写一行 prediction_log（actual_value / deviation_rate 留空，后续回算）
    Database::instance().exec("BEGIN");
    for (size_t i = 0; i < fc.tickTimes.size(); ++i) {
        Database::instance().execParams(
            "INSERT INTO prediction_log(spot_id,predict_time,base_time,predicted_value,"
            "q10,q50,q90,uncertainty_width,created_at)"
            " VALUES(?,?,?,?,?,?,?,?,?)",
            {fc.spotId, fc.tickTimes[i], fc.baseTime,
             std::to_string(fc.q50[i]),
             std::to_string(fc.q10[i]),
             std::to_string(fc.q50[i]),
             std::to_string(fc.q90[i]),
             std::to_string(fc.uncertaintyWidth[i]),
             nowString()});
    }
    Database::instance().exec("COMMIT");
}

} // namespace scenicflow
