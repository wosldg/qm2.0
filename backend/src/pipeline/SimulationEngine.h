#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace scenicflow {

struct SimulationConfig {
    double arrivalRate = 45.0;      // visitors per minute for the whole scenic area
    std::string pattern = "PEAK";   // STEADY / PEAK / RANDOM / GROUP_PATTERN
    int durationMinutes = 120;

    // P1-4: 团客突发模式参数（仅 pattern=GROUP_PATTERN 时生效）
    // 指定景点在指定时间窗口内的客流脉冲
    struct GroupConfig {
        std::string targetSpotId;     // 目标景点 ID（空=全景区）
        double burstArrivalRate = 200.0;  // 脉冲到达率（人/分钟）
        int burstDurationMinutes = 10;    // 脉冲持续时间
        int burstDelayMinutes = 15;       // 从 simulation start 起多少分钟后触发
    };
    GroupConfig group;
};

struct SpotState {
    std::string spotId;
    std::string name;
    std::string spotType;          // ENTRANCE / PLAZA / TRAIL / VIEWPOINT / SUMMIT / GENERAL
    int capacity = 0;
    int currentVisitors = 0;
    int cumulativeVisitors = 0;
    double arrivalRate = 0.0;
    double departureRate = 0.0;
    double locationX = 0.0;
    double locationY = 0.0;
    double ratio() const { return capacity > 0 ? static_cast<double>(currentVisitors) / capacity : 0.0; }
};

// Simulated data source. Generates visitor arrivals/departures and writes flow records.
// Only active after POST /api/simulation/start.
class SimulationEngine {
public:
    static SimulationEngine& instance();

    void init();                                  // restore config from system_config
    bool start();
    void stop();
    bool isRunning() const;

    SimulationConfig config() const;
    SimulationConfig updateConfig(const SimulationConfig& cfg);

    // Called by DataPipeline once per second.
    void step();

    std::vector<SpotState> spots() const;
    int totalVisitors() const;

private:
    mutable std::mutex m_mutex;
    bool m_running = false;
    SimulationConfig m_config;
    std::chrono::steady_clock::time_point m_lastStep;
    std::chrono::steady_clock::time_point m_startedAt;
    int m_recordCounter = 0;

    double patternFactor(double progress) const;
    void persistConfig();
};

} // namespace scenicflow
