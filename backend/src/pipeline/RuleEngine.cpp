#include "pipeline/RuleEngine.h"

#include "core/Logger.h"
#include "core/Settings.h"
#include "core/TimeUtil.h"
#include "network/Mappers.h"
#include "pipeline/SimulationEngine.h"
#include "predict/Predictor.h"
#include "storage/Database.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <vector>

namespace scenicflow {

namespace {

bool compare(double value, const std::string& op, double threshold) {
    if (op == ">")  return value > threshold;
    if (op == ">=") return value >= threshold;
    if (op == "<")  return value < threshold;
    if (op == "<=") return value <= threshold;
    return false;
}

std::string fmt1(double v) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << v;
    return oss.str();
}

const char* eventTypeForMetric(const std::string& metric, double valueRatio) {
    if (metric == "visitor_ratio") return valueRatio >= 1.0 ? "OVERLOAD" : "CONGESTION";
    if (metric == "arrival_rate") return "DATA_ANOMALY";
    if (metric == "parking_free") return "PARKING_SHORTAGE";
    if (metric == "shuttle_load") return "CAPACITY_SHORTAGE";
    return "CONGESTION";
}

// 返回当前日期的场景：WEEKDAY / WEEKEND
std::string currentScenario() {
    const auto now = std::time(nullptr);
    const auto tm = toLocalTm(now);
    // tm_wday: 0=Sunday, 6=Saturday
    return (tm.tm_wday == 0 || tm.tm_wday == 6) ? "WEEKEND" : "WEEKDAY";
}

// rule 的 scenario 字段：ALL 总是匹配；WEEKDAY/WEEKEND 只在对应日期匹配
bool scenarioMatches(const std::string& ruleScenario, const std::string& todayScenario) {
    return ruleScenario == "ALL" || ruleScenario == todayScenario || ruleScenario.empty();
}

// rule 的 applicable_spot_type：ALL 匹配所有；具体类型只匹配对应 spot
bool spotTypeMatches(const std::string& ruleSpotType, const std::string& spotType) {
    if (ruleSpotType == "ALL" || ruleSpotType.empty()) return true;
    return ruleSpotType == spotType;
}

// 给定景点，查最新的 5MIN flow_forecast 预测（TFT 输出）
// 返回 {预测拥挤度, 不确定度}；没有预测数据时返回 {0, 0}
std::pair<double, double> latestForecastRatio(const std::string& spotId, int capacity) {
    // 取最新的 granularity='5MIN' + spot_id 非空的一条
    auto rows = Database::instance().queryParams(
        "SELECT q50, uncertainty_width FROM flow_forecast"
        " WHERE spot_id=? AND granularity='5MIN' AND q50 IS NOT NULL"
        " ORDER BY created_at DESC, id DESC LIMIT 1",
        {spotId});
    if (rows.empty()) return {0.0, 0.0};
    const double q50 = toDouble(field(rows[0], "q50"));
    const double uw = toDouble(field(rows[0], "uncertainty_width"));
    const double ratio = capacity > 0 ? q50 / capacity : 0.0;
    return {ratio, uw};
}

} // namespace

RuleEngine& RuleEngine::instance() {
    static RuleEngine inst;
    return inst;
}

size_t RuleEngine::activeEventCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeEvents.size();
}

void RuleEngine::evaluate() {
    const std::string todayScenario = currentScenario();
    const double uwThreshold = Predictor::instance().isReady()
                                   ? Predictor::instance().uncertaintyWidthThreshold()
                                   : 0.3;

    auto ruleRows = Database::instance().query(
        "SELECT * FROM rule WHERE status=1 ORDER BY priority, id");
    if (ruleRows.empty()) return;

    const auto spots = SimulationEngine::instance().spots();
    auto parkingRows = Database::instance().query(
        "SELECT * FROM parking_lot WHERE status=1 ORDER BY id");
    auto shuttleRows = Database::instance().query("SELECT * FROM shuttle_bus ORDER BY id");

    int minFree = -1;
    std::string minFreeName;
    for (const auto& p : parkingRows) {
        const int total = toInt(field(p, "total_spaces"));
        const int occupied = toInt(field(p, "occupied_spaces"));
        const int free = total - occupied;
        if (minFree < 0 || free < minFree) {
            minFree = free;
            minFreeName = field(p, "name");
        }
    }

    double maxShuttleLoad = -1.0;
    std::string maxShuttleName;
    for (const auto& s : shuttleRows) {
        const int cap = toInt(field(s, "capacity"));
        if (cap <= 0) continue;
        const double load = static_cast<double>(toInt(field(s, "current_passengers"))) / cap;
        if (load > maxShuttleLoad) {
            maxShuttleLoad = load;
            maxShuttleName = field(s, "name");
        }
    }

    const int debounce = std::max(1, currentSettings().debounceSeconds);

    std::lock_guard<std::mutex> lock(m_mutex);
    std::set<std::string> hitKeys;

    for (const auto& rule : ruleRows) {
        // ---- P1-1: 场景过滤（工作日/周末）----
        const std::string ruleScenario = field(rule, "scenario", "ALL");
        if (!scenarioMatches(ruleScenario, todayScenario)) continue;

        const std::string ruleId = field(rule, "id");
        const std::string metric = field(rule, "metric");
        const std::string op = field(rule, "operator");
        const double threshold = toDouble(field(rule, "threshold"));
        const std::string level = field(rule, "level", "WARNING");
        const std::string action = field(rule, "action");
        const std::string targetSpot = field(rule, "spot_id", "*");
        const std::string ruleSpotType = field(rule, "applicable_spot_type", "ALL");
        const std::string disposalPlan = field(rule, "disposal_plan");

        if (metric == "parking_free") {
            if (minFree < 0) continue;
            const bool hit = compare(minFree, op, threshold);
            std::string desc = minFreeName + "剩余车位 " + std::to_string(minFree) +
                               " 个，" + op + " 阈值 " + fmt1(threshold);
            if (!disposalPlan.empty()) desc += "｜处置预案：" + disposalPlan;
            evaluateCondition(ruleId, "parking", hit, "PARKING_SHORTAGE", level, "",
                              desc, disposalPlan.empty() ? "建议引导车辆前往剩余车位最多的停车场" : disposalPlan,
                              debounce);
            if (hit) hitKeys.insert("PARKING_SHORTAGE:");
            continue;
        }

        if (metric == "shuttle_load") {
            if (maxShuttleLoad < 0) continue;
            const bool hit = compare(maxShuttleLoad, op, threshold);
            std::string desc = maxShuttleName + "满载率 " + fmt1(maxShuttleLoad * 100) +
                               "%，" + op + " 阈值 " + fmt1(threshold * 100) + "%";
            if (!disposalPlan.empty()) desc += "｜处置预案：" + disposalPlan;
            evaluateCondition(ruleId, "shuttle", hit, "CAPACITY_SHORTAGE", level, "",
                              desc, disposalPlan.empty() ? "建议调派最近空闲摆渡车前往支援" : disposalPlan,
                              debounce);
            if (hit) hitKeys.insert("CAPACITY_SHORTAGE:");
            continue;
        }

        for (const auto& s : spots) {
            if (targetSpot != "*" && targetSpot != s.spotId) continue;
            // ---- P1-1: spot_type 过滤 ----
            if (!spotTypeMatches(ruleSpotType, s.spotType)) continue;

            double value = 0.0;
            std::string desc;
            if (metric == "visitor_ratio") {
                value = s.ratio();
                desc = s.name + "当前在园 " + std::to_string(s.currentVisitors) + " 人，拥挤度 " +
                       fmt1(value * 100) + "%，" + op + " 阈值 " + fmt1(threshold * 100) + "%";
            } else if (metric == "arrival_rate") {
                value = s.arrivalRate;
                desc = s.name + "到达速率 " + fmt1(value) + " 人/分钟，" + op + " 阈值 " +
                       fmt1(threshold);
            } else {
                continue;
            }

            const bool hit = compare(value, op, threshold);
            const std::string type = eventTypeForMetric(metric, value);
            std::string suggestion = action;
            if (type == "CONGESTION" || type == "OVERLOAD") {
                suggestion = "建议将部分游客分流至容量充足的景点";
            }
            if (!disposalPlan.empty()) {
                desc += "｜处置预案：" + disposalPlan;
                suggestion = disposalPlan;
            }
            evaluateCondition(ruleId, s.spotId, hit, type, level, s.spotId, desc, suggestion,
                              debounce);
            if (hit) hitKeys.insert(type + ":" + s.spotId);

            // ---- P0-2 + P0-3: 预测提前预警 ----
            // 读取 flow_forecast（TFT 输出），如果预测拥挤度超过阈值则提前触发
            auto [forecastRatio, forecastUw] = latestForecastRatio(s.spotId, s.capacity);
            if (forecastRatio > 0 && forecastRatio >= threshold && forecastRatio > s.ratio()) {
                const bool highUw = forecastUw >= uwThreshold;
                const std::string predType = "PREDICTED_" + type; // PREDICTED_CONGESTION / PREDICTED_OVERLOAD
                std::string predDesc = s.name + " TFT 预测拥挤度 " + fmt1(forecastRatio * 100) +
                                       "%（高于当前 " + fmt1(s.ratio() * 100) + "%），"
                                       + op + " 阈值 " + fmt1(threshold * 100) + "%";
                if (highUw) {
                    predDesc += "｜⚠️ 不确定度较高（width_ratio=" + fmt1(forecastUw) + "），"
                                "建议人工确认后再执行处置";
                } else {
                    if (!disposalPlan.empty()) predDesc += "｜处置预案：" + disposalPlan;
                }
                const char* predLevel = highUw ? "INFO" : level.c_str();
                const std::string predSuggestion = highUw
                    ? "高不确定度预测，暂不自动生成调度建议，请人工确认"
                    : (disposalPlan.empty() ? "建议提前部署分流" : disposalPlan);
                evaluateCondition(ruleId + "_pred", s.spotId, true, predType, predLevel,
                                  s.spotId, predDesc, predSuggestion, debounce);
                hitKeys.insert(predType + ":" + s.spotId);
            }
        }
    }

    // Auto-resolve only when no rule still holds the same event key.
    std::vector<std::string> toResolve;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& entry : m_activeEvents) {
        const std::string& key = entry.first;
        if (hitKeys.count(key)) {
            m_falseSince.erase(key);
            continue;
        }
        auto it = m_falseSince.find(key);
        if (it == m_falseSince.end()) {
            m_falseSince[key] = now;
            continue;
        }
        if (std::chrono::duration<double>(now - it->second).count() >= debounce) {
            toResolve.push_back(key);
        }
    }
    for (const auto& key : toResolve) {
        const auto pos = key.find(':');
        const std::string type = pos == std::string::npos ? key : key.substr(0, pos);
        const std::string spotId = pos == std::string::npos ? "" : key.substr(pos + 1);
        resolveEvent(key, type, spotId);
        m_falseSince.erase(key);
    }
}

bool RuleEngine::evaluateCondition(const std::string& ruleId, const std::string& subject,
                                   bool condition, const std::string& type,
                                   const std::string& level, const std::string& spotId,
                                   const std::string& description,
                                   const std::string& suggestion, int debounceSeconds) {
    const std::string key = ruleId + ":" + subject;
    RuleRuntime& rt = m_runtime[key];
    const auto now = std::chrono::steady_clock::now();

    if (!condition) {
        rt.trueSince.reset();
        return false;
    }

    if (!rt.trueSince) rt.trueSince = now;
    const double seconds = std::chrono::duration<double>(now - *rt.trueSince).count();
    if (seconds >= debounceSeconds) {
        raiseEvent(type + ":" + spotId, type, level, spotId, description, suggestion);
    }
    return true;
}

void RuleEngine::raiseEvent(const std::string& key, const std::string& type,
                            const std::string& level, const std::string& spotId,
                            const std::string& description, const std::string& suggestion) {
    if (m_activeEvents.count(key)) {
        m_falseSince.erase(key);
        return;
    }

    // Survive restarts: reuse an ACTIVE event already present in the DB.
    auto existing = Database::instance().queryParams(
        "SELECT id FROM event WHERE event_type=? AND IFNULL(spot_id,'')=? AND status='ACTIVE'"
        " ORDER BY id DESC LIMIT 1",
        {type, spotId});
    if (!existing.empty()) {
        m_activeEvents[key] = toInt(existing[0]["id"]);
        m_falseSince.erase(key);
        return;
    }

    if (!Database::instance().execParams(
            "INSERT INTO event(event_type,occur_time,spot_id,level,description,suggestion,status)"
            " VALUES(?,?,?,?,?,?,'ACTIVE')",
            {type, nowString(), spotId, level, description, suggestion})) {
        logError("failed to insert event: " + Database::instance().lastError());
        return;
    }

    auto rows = Database::instance().queryParams(
        "SELECT id FROM event WHERE event_type=? AND IFNULL(spot_id,'')=? AND status='ACTIVE'"
        " ORDER BY id DESC LIMIT 1",
        {type, spotId});
    if (!rows.empty()) m_activeEvents[key] = toInt(rows[0]["id"]);
    m_falseSince.erase(key);
    logWarn("event raised: " + type + (spotId.empty() ? "" : " @" + spotId) + " - " + description);
}

void RuleEngine::resolveEvent(const std::string& key, const std::string& type,
                              const std::string& spotId) {
    Database::instance().execParams(
        "UPDATE event SET status='RESOLVED', updated_at=? "
        "WHERE event_type=? AND IFNULL(spot_id,'')=? AND status='ACTIVE'",
        {nowString(), type, spotId});
    m_activeEvents.erase(key);
    logInfo("event auto-resolved: " + type + (spotId.empty() ? "" : " @" + spotId));
}

} // namespace scenicflow
