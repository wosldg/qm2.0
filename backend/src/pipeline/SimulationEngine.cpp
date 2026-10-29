#include "pipeline/SimulationEngine.h"

#include "core/Logger.h"
#include "core/Settings.h"
#include "core/TimeUtil.h"
#include "model/Enums.h"
#include "storage/Database.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace scenicflow {

namespace {

double readConfigDouble(const std::string& key, double def) {
    auto rows = Database::instance().queryParams("SELECT value FROM system_config WHERE key=?", {key});
    if (rows.empty()) return def;
    try { return std::stod(rows[0]["value"]); } catch (...) { return def; }
}

std::string readConfigString(const std::string& key, const std::string& def) {
    auto rows = Database::instance().queryParams("SELECT value FROM system_config WHERE key=?", {key});
    if (rows.empty()) return def;
    return rows[0]["value"];
}

void writeConfig(const std::string& key, const std::string& value) {
    Database::instance().execParams(
        "INSERT INTO system_config(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        {key, value});
}

} // namespace

SimulationEngine& SimulationEngine::instance() {
    static SimulationEngine inst;
    return inst;
}

void SimulationEngine::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.arrivalRate = readConfigDouble("simArrivalRate", 45.0);
    m_config.pattern = readConfigString("simPattern", "PEAK");
    m_config.durationMinutes = static_cast<int>(readConfigDouble("simDurationMinutes", 120.0));
    // P1-4: 团客模式配置从 system_config 读取（如果有的话）
    m_config.group.targetSpotId = readConfigString("simGroupTargetSpot", "");
    m_config.group.burstArrivalRate = readConfigDouble("simGroupBurstRate", 200.0);
    m_config.group.burstDurationMinutes = static_cast<int>(readConfigDouble("simGroupBurstMin", 10.0));
    m_config.group.burstDelayMinutes = static_cast<int>(readConfigDouble("simGroupDelayMin", 15.0));
    m_running = false;
    logInfo("simulation engine ready (arrivalRate=" + std::to_string(m_config.arrivalRate) +
            ", pattern=" + m_config.pattern + ")");
}

bool SimulationEngine::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_running = true;
    m_startedAt = std::chrono::steady_clock::now();
    m_lastStep = m_startedAt;
    m_recordCounter = 0;
    logInfo("simulation started");
    return true;
}

void SimulationEngine::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running) logInfo("simulation stopped");
    m_running = false;
}

bool SimulationEngine::isRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running;
}

SimulationConfig SimulationEngine::config() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

SimulationConfig SimulationEngine::updateConfig(const SimulationConfig& cfg) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = cfg;
    }
    persistConfig();
    logInfo("simulation config updated");
    return config();
}

void SimulationEngine::persistConfig() {
    SimulationConfig cfg = config();
    writeConfig("simArrivalRate", std::to_string(cfg.arrivalRate));
    writeConfig("simPattern", cfg.pattern);
    writeConfig("simDurationMinutes", std::to_string(cfg.durationMinutes));
    // P1-4: 团客配置持久化
    writeConfig("simGroupTargetSpot", cfg.group.targetSpotId);
    writeConfig("simGroupBurstRate", std::to_string(cfg.group.burstArrivalRate));
    writeConfig("simGroupBurstMin", std::to_string(cfg.group.burstDurationMinutes));
    writeConfig("simGroupDelayMin", std::to_string(cfg.group.burstDelayMinutes));
}

double SimulationEngine::patternFactor(double progress) const {
    // progress in [0,1] across the configured duration.
    if (m_config.pattern == "STEADY") return 1.0;
    if (m_config.pattern == "PEAK") {
        // Bell curve: low at the edges, peaking in the middle of the window.
        return 0.5 + 1.1 * std::sin(progress * 3.14159265358979);
    }
    // RANDOM
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> dist(0.5, 1.5);
    return dist(rng);
}

std::vector<SpotState> SimulationEngine::spots() const {
    auto rows = Database::instance().query(
        "SELECT * FROM scenic_spot WHERE status=1 ORDER BY id");
    std::vector<SpotState> out;
    out.reserve(rows.size());
    for (const auto& r : rows) {
        SpotState s;
        s.spotId = r.count("spot_id") ? r.at("spot_id") : "";
        s.name = r.count("name") ? r.at("name") : "";
        s.spotType = r.count("spot_type") ? r.at("spot_type") : "GENERAL";
        s.capacity = r.count("instant_capacity") ? std::stoi(r.at("instant_capacity")) : 0;
        s.currentVisitors = r.count("current_visitors") ? std::stoi(r.at("current_visitors")) : 0;
        s.cumulativeVisitors = r.count("cumulative_visitors") ? std::stoi(r.at("cumulative_visitors")) : 0;
        s.arrivalRate = r.count("arrival_rate") ? std::stod(r.at("arrival_rate")) : 0.0;
        s.departureRate = r.count("departure_rate") ? std::stod(r.at("departure_rate")) : 0.0;
        s.locationX = r.count("location_x") ? std::stod(r.at("location_x")) : 0.0;
        s.locationY = r.count("location_y") ? std::stod(r.at("location_y")) : 0.0;
        out.push_back(s);
    }
    return out;
}

int SimulationEngine::totalVisitors() const {
    auto rows = Database::instance().query(
        "SELECT COALESCE(SUM(current_visitors),0) AS v FROM scenic_spot WHERE status=1");
    if (rows.empty()) return 0;
    try { return std::stoi(rows[0]["v"]); } catch (...) { return 0; }
}

void SimulationEngine::step() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running) return;

    const auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - m_lastStep).count();
    if (dt < 0.2) return;
    if (dt > 10.0) dt = 10.0;   // guard against long stalls
    m_lastStep = now;

    const double elapsedMinutes =
        std::chrono::duration<double>(now - m_startedAt).count() / 60.0;
    if (m_config.durationMinutes > 0 && elapsedMinutes >= m_config.durationMinutes) {
        m_running = false;
        logInfo("simulation finished (duration reached)");
        return;
    }

    const double progress = m_config.durationMinutes > 0
                                ? std::min(1.0, elapsedMinutes / m_config.durationMinutes)
                                : 0.0;
    const double factor = patternFactor(progress);

    std::vector<SpotState> all = spots();
    int totalCapacity = 0;
    for (const auto& s : all) totalCapacity += s.capacity;
    if (totalCapacity <= 0 || all.empty()) return;

    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> jitter(0.85, 1.15);

    const double avgStayMinutes = 25.0;
    ++m_recordCounter;

    // P1-4: 突发团客脉冲是否在当前时间窗口内
    const bool groupActive =
        m_config.pattern == "GROUP_PATTERN" &&
        elapsedMinutes >= m_config.group.burstDelayMinutes &&
        elapsedMinutes < m_config.group.burstDelayMinutes + m_config.group.burstDurationMinutes;

    for (const auto& s : all) {
        const double weight = static_cast<double>(s.capacity) / totalCapacity;
        double arrivalPerMin =
            m_config.arrivalRate * factor * weight * jitter(rng);

        // P1-4: 团客突发脉冲叠加
        if (groupActive) {
            bool targetMatch = m_config.group.targetSpotId.empty() ||
                               m_config.group.targetSpotId == s.spotId;
            if (targetMatch) {
                double groupWeight = m_config.group.targetSpotId.empty() ? weight : 1.0;
                arrivalPerMin += m_config.group.burstArrivalRate * groupWeight;
            }
        }

        const double departurePerMin =
            static_cast<double>(s.currentVisitors) / avgStayMinutes;

        double delta = (arrivalPerMin - departurePerMin) * (dt / 60.0);
        int next = static_cast<int>(std::lround(s.currentVisitors + delta));
        next = std::max(0, next);
        next = std::min(next, static_cast<int>(s.capacity * 1.25));

        const int arrived = std::max(0, static_cast<int>(std::lround(arrivalPerMin * (dt / 60.0))));
        const int cumulative = s.cumulativeVisitors + arrived;

        Database::instance().execParams(
            "UPDATE scenic_spot SET current_visitors=?, cumulative_visitors=?, arrival_rate=?,"
            " departure_rate=?, updated_at=? WHERE spot_id=?",
            {std::to_string(next), std::to_string(cumulative),
             std::to_string(arrivalPerMin), std::to_string(departurePerMin),
             nowString(), s.spotId});

        // Persist a flow point every 5 ticks (~5s) to keep the table compact.
        if (m_recordCounter % 5 == 0) {
            const double ratio = s.capacity > 0 ? static_cast<double>(next) / s.capacity : 0.0;
            Database::instance().execParams(
                "INSERT INTO flow_record(record_time,spot_id,current_visitors,cumulative_visitors,"
                "congestion_level,arrival_rate,departure_rate,is_deleted) VALUES(?,?,?,?,?,?,?,0)",
                {nowString(), s.spotId, std::to_string(next), std::to_string(cumulative),
                 toString(levelFromRatio(ratio)), std::to_string(arrivalPerMin),
                 std::to_string(departurePerMin)});
        }
    }
}

} // namespace scenicflow
