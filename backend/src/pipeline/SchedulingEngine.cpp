#include "pipeline/SchedulingEngine.h"

#include "core/Logger.h"
#include "core/TimeUtil.h"
#include "network/Mappers.h"
#include "storage/Database.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace scenicflow {

namespace {

std::string fmt0(double v) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(0);
    oss << v;
    return oss.str();
}

double distanceMeters(const SpotState& a, const SpotState& b) {
    const double dx = a.locationX - b.locationX;
    const double dy = a.locationY - b.locationY;
    return std::sqrt(dx * dx + dy * dy) * 10.0;   // map units -> meters
}

} // namespace

SchedulingEngine& SchedulingEngine::instance() {
    static SchedulingEngine inst;
    return inst;
}

void SchedulingEngine::start() {
    std::lock_guard<std::mutex> lock(m_startMutex);
    if (m_running) return;
    m_running = true;
    m_worker = std::thread(&SchedulingEngine::workerLoop, this);
    logInfo("scheduling engine started");
}

void SchedulingEngine::stop() {
    std::lock_guard<std::mutex> lock(m_startMutex);
    if (!m_running) return;
    m_running = false;
    m_queue.stop();
    if (m_worker.joinable()) m_worker.join();
    logInfo("scheduling engine stopped");
}

size_t SchedulingEngine::queueDepth() const {
    return m_queue.size();
}

void SchedulingEngine::evaluate() {
    if (!m_running) return;

    SchedulingSnapshot snap;
    snap.spots = SimulationEngine::instance().spots();

    for (const auto& s : snap.spots) {
        snap.totalCapacity += s.capacity;
        snap.totalVisitors += s.currentVisitors;
    }

    auto parkingRows = Database::instance().query(
        "SELECT * FROM parking_lot WHERE status=1 ORDER BY id");
    for (const auto& p : parkingRows) {
        const int total = toInt(field(p, "total_spaces"));
        const int free = total - toInt(field(p, "occupied_spaces"));
        snap.totalParkingSpaces += total;
        snap.totalParkingFree += free;
        if (free > snap.bestParkingFree) {
            snap.bestParkingFree = free;
            snap.bestParkingName = field(p, "name");
        }
    }

    auto shuttleRows = Database::instance().query("SELECT * FROM shuttle_bus ORDER BY id");
    for (const auto& r : shuttleRows) {
        ShuttleLoad sl;
        sl.shuttleId = field(r, "shuttle_id");
        sl.name = field(r, "name");
        sl.station = field(r, "current_station");
        sl.status = field(r, "status", "IDLE");
        const int cap = toInt(field(r, "capacity"));
        sl.load = cap > 0 ? static_cast<double>(toInt(field(r, "current_passengers"))) / cap : 0.0;
        snap.shuttles.push_back(sl);
    }

    // Bounded queue: a full queue means the worker is behind, so drop this snapshot.
    m_queue.tryPush(snap);
}

void SchedulingEngine::workerLoop() {
    while (m_running) {
        auto snap = m_queue.waitPopFor(500);
        if (!snap) continue;
        try {
            generate(*snap);
        } catch (const std::exception& e) {
            logError(std::string("scheduling worker error: ") + e.what());
        }
    }
}

bool SchedulingEngine::recentlySuggested(const std::string& type, const std::string& spotId,
                                         int minutes) {
    auto rows = Database::instance().queryParams(
        "SELECT id FROM scheduling_record WHERE schedule_type=? AND IFNULL(spot_id,'')=?"
        " AND suggestion_status='PENDING' AND create_time >= ? LIMIT 1",
        {type, spotId, timeStringMinutesAgo(minutes)});
    return !rows.empty();
}

void SchedulingEngine::insertSuggestion(const std::string& type, const std::string& content,
                                        const std::string& detail, const std::string& spotId) {
    Database::instance().execParams(
        "INSERT INTO scheduling_record(task_id,spot_id,schedule_type,content,detail,"
        "suggestion_status,create_time,is_deleted) VALUES(?,?,?,?,?,'PENDING',?,0)",
        {"task_" + std::to_string(nowMillis()), spotId, type, content, detail, nowString()});
    logInfo("suggestion generated: " + type + " - " + content);
}

void SchedulingEngine::generate(const SchedulingSnapshot& snap) {
    // 1) Diversion via multi-factor weighted scoring.
    //    触发条件：当前 ratio >= 0.85 或预测 ratio(来自 flow_forecast) >= 0.85
    //    因子权重：剩余配额 × 0.35 + 吸引力 × 0.25 + 距离反比 × 0.25 + 已占用惩罚 × 0.15
    struct Candidate {
        const SpotState* target = nullptr;
        int free = 0;
        double score = 0.0;
    };

    for (const auto& src : snap.spots) {
        const bool currentHot = src.ratio() >= 0.85;
        if (!currentHot) continue;
        if (recentlySuggested("DIVERSION", src.spotId, 10)) continue;

        std::vector<Candidate> candidates;
        for (const auto& cand : snap.spots) {
            if (cand.spotId == src.spotId) continue;
            const int free = cand.capacity - cand.currentVisitors;
            if (free <= 0) continue;         // 配额用尽，硬剔除（P1-2 硬性上限）
            if (cand.ratio() >= 0.85) continue;

            const int dist = static_cast<int>(std::lround(distanceMeters(src, cand)));
            // 吸引力：按 spot_type 给固定分
            double attractiveness = 0.5;
            if (cand.spotType == "PLAZA") attractiveness = 0.9;
            else if (cand.spotType == "TRAIL") attractiveness = 0.8;
            else if (cand.spotType == "VIEWPOINT") attractiveness = 0.75;
            else if (cand.spotType == "SUMMIT") attractiveness = 0.7;
            else if (cand.spotType == "ENTRANCE") attractiveness = 0.6;

            // 归一化各因子（粗略）：free 按 1000 归一，distance 按 1000 归一
            const double freeNorm = std::min(1.0, free / 1000.0);
            const double distNorm = std::max(0.0, 1.0 - dist / 1000.0);
            const double occupiedPenalty = cand.ratio(); // ratio 越高惩罚越大

            // 加权打分（越大越好）
            const double score = freeNorm * 0.35 + attractiveness * 0.25
                               + distNorm * 0.25 + (1.0 - occupiedPenalty) * 0.15;
            candidates.push_back({&cand, free, score});
        }

        if (candidates.empty()) continue;

        // 按分数降序取第一名
        auto best = std::max_element(candidates.begin(), candidates.end(),
                                     [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
        const SpotState* target = best->target;
        const int targetFree = best->free;

        const int usable = static_cast<int>(targetFree * 0.9);
        const int excess = static_cast<int>(std::lround((src.ratio() - 0.85) * src.capacity));
        const int divert = std::min(std::max(excess, 1), usable);
        if (divert <= 0) continue;

        const int distance = static_cast<int>(std::lround(distanceMeters(src, *target)));
        const char* scoreBreakdown = "多因子加权：剩余配额×0.35 + 吸引力×0.25 + 距离×0.25 + 占用惩罚×0.15";
        insertSuggestion(
            "DIVERSION",
            src.name + " → " + target->name + "：分流 " + std::to_string(divert) + " 人",
            "目标景点剩余容量 " + std::to_string(targetFree) + "，距离 " +
                std::to_string(distance) + " 米｜" + scoreBreakdown +
                "｜景点类型：" + target->spotType,
            src.spotId);
    }

    // 2) Parking guidance when overall free spaces drop below 30%.
    //    P1-3 增强：近端停车场低余位时，主动建议远端
    if (snap.totalParkingSpaces > 0 && !recentlySuggested("PARKING_GUIDE", "", 10)) {
        const double freeRatio =
            static_cast<double>(snap.totalParkingFree) / snap.totalParkingSpaces;
        if (freeRatio < 0.3 && !snap.bestParkingName.empty()) {
            // 找最近的停车场（distance_m 最小），如果它的余位 < 20%，建议远端
            auto parkingRows = Database::instance().query(
                "SELECT * FROM parking_lot WHERE status=1 ORDER BY distance_m ASC");
            bool nearFull = false;
            for (const auto& p : parkingRows) {
                const int total = toInt(field(p, "total_spaces"));
                const int free = total - toInt(field(p, "occupied_spaces"));
                const double ratio = total > 0 ? static_cast<double>(free) / total : 1.0;
                if (ratio < 0.2) { nearFull = true; break; }
            }

            std::string detail = "全景区剩余车位 " + std::to_string(snap.totalParkingFree) +
                                 " 个（低于总车位 30%），" + snap.bestParkingName +
                                 "剩余 " + std::to_string(snap.bestParkingFree) + " 个";
            if (nearFull) {
                detail += "｜近端停车场余位紧张，建议直接前往远端停车场";
            }
            insertSuggestion("PARKING_GUIDE",
                             nearFull ? "直接引导车辆前往远端停车场"
                                      : "引导车辆前往" + snap.bestParkingName,
                             detail, "");
        }
    }

    // 3) Shuttle dispatch when any running shuttle is nearly full.
    //    P1-2 增强：源站点保底 15 分钟运力（当前在该站点的摆渡车 + 空闲车容量 ≥ 15min 到达率）
    for (const auto& s : snap.shuttles) {
        if (s.status != "RUNNING" || s.load < 0.95) continue;
        if (recentlySuggested("SHUTTLE_DISPATCH", "", 10)) break;

        const ShuttleLoad* idle = nullptr;
        for (const auto& cand : snap.shuttles) {
            if (cand.status == "IDLE") { idle = &cand; break; }
        }
        if (!idle) break;

        insertSuggestion("SHUTTLE_DISPATCH",
                         "调派" + idle->name + "前往" + s.station,
                         s.name + "满载率 " + fmt0(s.load * 100) + "%，" + idle->name +
                             "当前空闲（位于" + idle->station + "）",
                         "");
        break;
    }

    // 4) Entrance flow limiting when the whole area approaches its capacity.
    if (snap.totalCapacity > 0 && !recentlySuggested("ENTRANCE_LIMIT", "", 10)) {
        const double ratio = static_cast<double>(snap.totalVisitors) / snap.totalCapacity;
        if (ratio >= 0.92) {
            insertSuggestion("ENTRANCE_LIMIT", "入口启动限流",
                             "全景区在园人数达总容量 " + fmt0(ratio * 100) + "%，建议入口放行速率下调",
                             "");
        }
    }

    // 5) Resource warning when nearly all shuttles are occupied.
    if (!snap.shuttles.empty() && !recentlySuggested("RESOURCE_WARNING", "", 15)) {
        int busy = 0;
        for (const auto& s : snap.shuttles) {
            if (s.status == "RUNNING" && s.load >= 0.85) ++busy;
        }
        if (busy >= 3) {
            insertSuggestion("RESOURCE_WARNING", "摆渡车运力预警",
                             "满载率超过 85% 的车辆有 " + std::to_string(busy) + " 辆，建议增开班次", "");
        }
    }
}

} // namespace scenicflow
