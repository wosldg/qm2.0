#include "pipeline/DataPipeline.h"

#include "core/Logger.h"
#include "core/Settings.h"
#include "core/TimeUtil.h"
#include "network/Mappers.h"
#include "pipeline/RuleEngine.h"
#include "pipeline/SchedulingEngine.h"
#include "pipeline/SimulationEngine.h"
#include "predict/Predictor.h"
#include "storage/Database.h"

#include <chrono>
#include <cmath>

namespace scenicflow {

namespace {

constexpr int kRuleIntervalSeconds = 3;
constexpr int kSchedulingIntervalSeconds = 5;
constexpr long long kCleanupIntervalMillis = 3600LL * 1000LL;
constexpr long long kPredictIntervalMillis = 300LL * 1000LL;   // 5 分钟一次
constexpr long long kLogBackfillIntervalMillis = 60LL * 1000LL; // 1 分钟回算一次

} // namespace

DataPipeline& DataPipeline::instance() {
    static DataPipeline inst;
    return inst;
}

void DataPipeline::start() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread(&DataPipeline::loop, this);
    logInfo("data pipeline started");
}

void DataPipeline::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    logInfo("data pipeline stopped");
}

std::vector<PipelineStatus> DataPipeline::statuses() const {
    const bool running = m_running.load();
    std::vector<PipelineStatus> out;
    out.push_back({"数据接入线程", running, 0});
    out.push_back({"预处理线程", running, 0});
    out.push_back({"拥挤计算线程", running, 0});
    out.push_back({"规则引擎线程", running,
                   static_cast<int>(RuleEngine::instance().activeEventCount())});
    out.push_back({"数据写库线程", running,
                   static_cast<int>(SchedulingEngine::instance().queueDepth())});
    out.push_back({"预测推理线程", running,
                   Predictor::instance().isReady() ? 1 : 0});
    return out;
}

void DataPipeline::runRetentionCleanup() {
    const SystemSettings cfg = currentSettings();

    Database::instance().execParams(
        "DELETE FROM flow_record WHERE record_time < ?",
        {formatTime(std::time(nullptr) - static_cast<std::time_t>(cfg.retentionDays) * 86400,
                    "%Y-%m-%d %H:%M:%S")});

    Database::instance().execParams(
        "UPDATE scheduling_record SET is_deleted=1 WHERE create_time < ? AND is_deleted=0",
        {formatTime(std::time(nullptr) - 30LL * 86400, "%Y-%m-%d %H:%M:%S")});

    // P0-4: 超过 7 天的 prediction_log 也清理（保留一个月的用于效果评估）
    Database::instance().execParams(
        "DELETE FROM prediction_log WHERE predict_time < ?",
        {formatTime(std::time(nullptr) - 30LL * 86400, "%Y-%m-%d %H:%M:%S")});

    logInfo("retention cleanup done (flow retention " + std::to_string(cfg.retentionDays) + "d)");
}

void DataPipeline::backfillPredictionLogs() {
    // 扫描 prediction_log 中 actual_value IS NULL 的记录，
    // 找对应时间点（predict_time ± 2.5min 范围内）的 flow_record 来填 actual_value
    // 最多每次处理 500 条，避免一次扫太多
    auto pending = Database::instance().query(
        "SELECT id, spot_id, predict_time, predicted_value FROM prediction_log"
        " WHERE actual_value IS NULL ORDER BY predict_time ASC LIMIT 500");
    if (pending.empty()) return;

    int filled = 0;
    Database::instance().exec("BEGIN");
    for (const auto& row : pending) {
        const std::string spotId = field(row, "spot_id");
        const std::string predictTime = field(row, "predict_time");
        const double predictedValue = toDouble(field(row, "predicted_value"));

        // predict_time 格式是 YYYY-MM-DD HH:MM:SS，往前找 2.5min 范围内最近的 flow_record
        auto actuals = Database::instance().queryParams(
            "SELECT current_visitors FROM flow_record"
            " WHERE spot_id=? AND is_deleted=0"
            " AND record_time >= datetime(?, '-3 minutes')"
            " AND record_time <= datetime(?, '+3 minutes')"
            " ORDER BY ABS(strftime('%s', record_time) - strftime('%s', ?)) ASC LIMIT 1",
            {spotId, predictTime, predictTime, predictTime});
        if (actuals.empty()) continue;

        const double actual = toDouble(field(actuals[0], "current_visitors"));
        const double denom = std::max(1.0, std::abs(predictedValue));
        const double deviation = std::abs(actual - predictedValue) / denom;

        Database::instance().execParams(
            "UPDATE prediction_log SET actual_value=?, deviation_rate=? WHERE id=?",
            {std::to_string(actual), std::to_string(deviation), field(row, "id")});
        ++filled;
    }
    Database::instance().exec("COMMIT");
    if (filled > 0) {
        logInfo("backfilled " + std::to_string(filled) + " prediction_log rows");
    }
}

void DataPipeline::loop() {
    m_lastCleanupMillis = nowMillis();
    m_lastPredictMillis = nowMillis();
    m_lastLogBackfillMillis = nowMillis();
    int lastRuleTick = -kRuleIntervalSeconds;
    int lastSchedulingTick = -kSchedulingIntervalSeconds;

    while (m_running) {
        const int tick = ++m_tickCount;
        const long long now = nowMillis();

        try {
            SimulationEngine::instance().step();

            if (tick - lastRuleTick >= kRuleIntervalSeconds) {
                lastRuleTick = tick;
                RuleEngine::instance().evaluate();
            }

            if (tick - lastSchedulingTick >= kSchedulingIntervalSeconds) {
                lastSchedulingTick = tick;
                SchedulingEngine::instance().evaluate();
            }

            if (now - m_lastPredictMillis >= kPredictIntervalMillis) {
                m_lastPredictMillis = now;
                // P0-2: TFT 全景点预测（仅在模型 ready 时触发）
                if (Predictor::instance().isReady()) {
                    try {
                        Predictor::instance().predictAll();
                    } catch (const std::exception& e) {
                        logWarn(std::string("predictAll failed: ") + e.what());
                    }
                }
            }

            if (now - m_lastLogBackfillMillis >= kLogBackfillIntervalMillis) {
                m_lastLogBackfillMillis = now;
                try {
                    backfillPredictionLogs();
                } catch (const std::exception& e) {
                    logWarn(std::string("prediction_log backfill failed: ") + e.what());
                }
            }

            if (now - m_lastCleanupMillis >= kCleanupIntervalMillis) {
                m_lastCleanupMillis = now;
                runRetentionCleanup();
            }
        } catch (const std::exception& e) {
            logError(std::string("data pipeline tick failed: ") + e.what());
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace scenicflow
