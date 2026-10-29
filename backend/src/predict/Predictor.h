#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace scenicflow {

// Result of one TFT inference for a single spot: the next 12 ticks (5 min each,
// i.e. 60 minutes ahead) of current_visitors as quantiles, denormalized.
struct SpotForecast {
    std::string spotId;
    std::string baseTime;              // record_time of the newest history tick used
    std::vector<std::string> tickTimes; // 12 future tick timestamps (YYYY-MM-DD HH:MM:SS)
    std::vector<double> q10;           // 12 values, visitors
    std::vector<double> q50;
    std::vector<double> q90;
    std::vector<double> uncertaintyWidth; // (q90 - q10) / max(q50, 1) per tick
};

// Aggregated result from predictAll: all forecasts + metadata for persistence.
struct PredictAllResult {
    int spotCount = 0;
    int successCount = 0;
    int skippedCount = 0;
    std::string predictTime;          // when predictAll was called
};

// Wraps the exported TFT onnx model (onnxruntime, CPU).
//
// Data contract (must match offline_tools/train_tft.py + probe verification):
//   inputs:
//     encoder_cat      int64 [1, 96, 3]  columns [month, dayofweek, is_weekend],
//                                        encoded via categorical_maps (pandas codes)
//     encoder_cont     float [1, 96, 5]  columns [season_factor, current_visitors,
//                                        arrival_rate, departure_rate, utilization],
//                                        z-score normalized with norm_params
//     encoder_lengths  int64 [1]         = 96
//     decoder_cat      int64 [1, 12, 3]  same encoding for the 12 future ticks
//     decoder_cont     float [1, 12, 5]  season_factor real (normalized); the 4
//                                        observed columns are filled with 0
//                                        (probe_decoder_fill.py: output insensitive)
//     decoder_lengths  int64 [1]         = 12
//     target_scale     float [1, 2]      = {0, 1} (target_normalizer=None => identity)
//   output:
//     prediction       float [1, 12, 3]  quantiles [0.1, 0.5, 0.9], normalized target
//
// History comes from flow_record (aggregated into 5-min buckets, needs >= 96 buckets = 8h);
// utilization is computed as current_visitors / scenic_spot.instant_capacity.
class Predictor {
public:
    static Predictor& instance();

    // Loads the onnx model, preprocessing_params.json (norm stats + categorical maps)
    // and season factors from sim_config.json. Safe to call once at startup.
    bool load(const std::string& onnxPath, const std::string& prepParamsPath,
              const std::string& simConfigPath);

    bool isReady() const;

    // Runs inference for one spot. Returns nullopt when the model is not loaded,
    // the spot is unknown, or fewer than 96 history buckets exist after 5-min aggregation.
    std::optional<SpotForecast> predict(const std::string& spotId);

    // Runs inference for all active spots; writes flow_forecast (per spot, granularity=5MIN)
    // and prediction_log for every successful result.
    PredictAllResult predictAll();

    double uncertaintyWidthThreshold() const;

private:
    Predictor() = default;
    ~Predictor();
    Predictor(const Predictor&) = delete;
    Predictor& operator=(const Predictor&) = delete;

    void writeForecastRows(const SpotForecast& fc, const std::string& predictTime);
    void writePredictionLogRows(const SpotForecast& fc);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    mutable std::mutex m_mutex; // onnxruntime Session::Run is thread-safe, but load is not
};

} // namespace scenicflow
