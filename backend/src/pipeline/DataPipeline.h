#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scenicflow {

struct PipelineStatus {
    std::string name;
    bool running = false;
    int queueDepth = 0;
};

// Orchestrates the 1s data pipeline:
//   simulation source -> congestion calc (level derived on read) -> rule engine -> scheduling engine
// Also owns the periodic data-retention cleanup (soft delete + aggregation policy).
// P0-2: every predictIntervalSec calls Predictor::predictAll(), writes flow_forecast
//       (per spot 5min granularity) and prediction_log, then backfills actual values.
class DataPipeline {
public:
    static DataPipeline& instance();

    void start();
    void stop();

    std::vector<PipelineStatus> statuses() const;

    // Deletes expired flow records and old scheduling records.
    void runRetentionCleanup();

private:
    void loop();

    // P0-4: 扫描 prediction_log 中 actual_value IS NULL 的记录，
    // 找到对应时间点的 flow_record.current_visitors 填进去，并计算 deviation_rate
    void backfillPredictionLogs();

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_tickCount{0};
    long long m_lastCleanupMillis = 0;
    long long m_lastPredictMillis = 0;
    long long m_lastLogBackfillMillis = 0;
};

} // namespace scenicflow
