#include "storage/Schema.h"

#include "core/Logger.h"
#include "core/TimeUtil.h"
#include "model/Enums.h"
#include "network/Mappers.h"
#include "storage/Database.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace scenicflow {

namespace {

bool isEmptyTable(const std::string& table) {
    auto rows = Database::instance().query("SELECT COUNT(*) AS c FROM " + table);
    if (rows.empty()) return true;
    return rows[0]["c"] == "0";
}

// Internal map grid (0..100) maps onto the real Wulingyuan / Wujiayu area
// (lng 110.4473..110.4802, lat 29.3326..29.3539, ~3 km across).
inline double mapToLat(double y) { return 29.3326 + y / 100.0 * 0.0213; }
inline double mapToLng(double x) { return 110.4473 + x / 100.0 * 0.0329; }
inline int mapDistance(double x1, double y1, double x2, double y2) {
    const double dx = x1 - x2, dy = y1 - y2;
    return static_cast<int>(std::lround(std::sqrt(dx * dx + dy * dy) * 20.0));
}

inline double toDoubleLocal(const std::string& s, double def = 0) {
    try { return std::stod(s); } catch (...) { return def; }
}

void seedSpots() {
    if (!isEmptyTable("scenic_spot")) return;
    // Real-world points around the Wulingyuan / Wujiayu entrance (Zhangjiajie).
    struct SpotSeed { const char* id; const char* name; const char* type;
                      int cap; int daily; int cur; int cum;
                      double x; double y; double lat; double lng;
                      const char* open; const char* close; };
    const SpotSeed seeds[] = {
        {"spot_01", "大氧吧广场",   "PLAZA",    15000, 60000, 3000, 8200,  67, 100, 29.3539, 110.4695, "07:00", "18:00"},
        {"spot_02", "金鞭溪入口",  "TRAIL",     8000, 40000, 5600, 9600,  57,  83, 29.3502, 110.4661, "07:00", "18:00"},
        {"spot_03", "千里相会",    "TRAIL",     5000, 20000, 1500, 4300,  43,  60, 29.3454, 110.4613, "07:00", "18:00"},
        {"spot_04", "闺门迎宾",    "ENTRANCE",  6000, 16000, 4800, 5900,  31,  43, 29.3418, 110.4576, "07:00", "18:00"},
        {"spot_05", "黄石寨入口",  "SUMMIT",    8000, 30000, 2400, 5100,  16,  21, 29.3371, 110.4527, "07:00", "18:30"},
        {"spot_06", "鹞子寨入口",  "VIEWPOINT", 3000, 12000,  300, 1500,   0,   0, 29.3326, 110.4473, "07:30", "17:30"},
    };
    for (const auto& s : seeds) {
        Database::instance().execParams(
            "INSERT INTO scenic_spot(spot_id,name,spot_type,instant_capacity,daily_capacity,"
            "current_visitors,cumulative_visitors,open_time,close_time,"
            "location_x,location_y,latitude,longitude,status)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,1)",
            {s.id, s.name, s.type, std::to_string(s.cap), std::to_string(s.daily),
             std::to_string(s.cur), std::to_string(s.cum),
             s.open, s.close, std::to_string(s.x), std::to_string(s.y),
             std::to_string(s.lat), std::to_string(s.lng)});
    }
    logInfo("seeded scenic_spot (6 rows with spot_type)");
}

void seedParkings() {
    if (!isEmptyTable("parking_lot")) return;
    // 近端 + 两个远端停车场。distance_m 是从主入口的步行距离，按此排序做分级引导。
    const struct { const char* id; const char* name; int total; int occ; int dist; double lat; double lng; }
    seeds[] = {
        {"park_01", "东门外部停车场",  800, 520, 180, 29.3395, 110.4789},
        {"park_02", "南门临时停车场", 1200, 340, 850, 29.3420, 110.4470},
        {"park_03", "北线生态停车场",  600, 110, 1500, 29.3610, 110.4620},
    };
    for (const auto& p : seeds) {
        Database::instance().execParams(
            "INSERT INTO parking_lot(parking_id,name,total_spaces,occupied_spaces,distance_m,latitude,longitude,status)"
            " VALUES(?,?,?,?,?,?,?,1)",
            {p.id, p.name, std::to_string(p.total), std::to_string(p.occ),
             std::to_string(p.dist), std::to_string(p.lat), std::to_string(p.lng)});
    }
    logInfo("seeded parking_lot (3 rows, graded by distance)");
}

void seedShuttles() {
    if (!isEmptyTable("shuttle_bus")) return;
    const char* stations[] = {"吴家峪门票站", "大氧吧广场", "金鞭溪入口", "千里相会", "闺门迎宾", "黄石寨入口", "鹞子寨入口"};
    const char* statuses[] = {"RUNNING", "RUNNING", "IDLE", "RUNNING", "MAINTENANCE"};
    for (int i = 1; i <= 15; ++i) {
        char id[16];
        char name[32];
        std::snprintf(id, sizeof(id), "shuttle_%02d", i);
        std::snprintf(name, sizeof(name), "环保车%02d号", i);
        const char* status = statuses[i % 5];
        const char* station = stations[i % 7];
        int passengers = (std::string(status) == "RUNNING") ? (i * 3) % 21 : 0;
        Database::instance().execParams(
            "INSERT INTO shuttle_bus(shuttle_id,name,capacity,current_station,current_passengers,status)"
            " VALUES(?,?,20,?,?,?)",
            {id, name, station, std::to_string(passengers), status});
    }
    logInfo("seeded shuttle_bus (15 rows)");
}

void seedRules() {
    if (!isEmptyTable("rule")) return;
    // scenario: ALL / WEEKDAY / WEEKEND；applicable_spot_type: ALL / ENTRANCE / PLAZA / TRAIL / VIEWPOINT / SUMMIT
    struct R { const char* spot; const char* metric; const char* op; double threshold;
               const char* action; const char* level; int priority; int enabled;
               const char* scenario; const char* spotType; const char* disposal; };
    const R seeds[] = {
        // ---- 通用规则（工作日+周末都适用）----
        {"*",       "visitor_ratio", ">=", 0.9,  "触发拥挤告警",             "HIGH",     10, 1, "ALL",   "ALL",       "[\"通知相邻景点分流准备\",\"安排志愿者在分流节点\",\"开启景区广播提示\"]"},
        {"*",       "visitor_ratio", ">=", 1.0,  "触发超载告警并生成分流建议","CRITICAL",  1, 1, "ALL",   "ALL",       "[\"立即启动入口限流\",\"调派空闲摆渡车增援\",\"通知应急小组\"]"},
        {"*",       "arrival_rate",  ">",  40,   "关注到达速率异常",         "INFO",     50, 1, "ALL",   "ALL",       ""},
        {"*",       "parking_free",  "<",  30,   "触发停车引导建议",         "WARNING",  30, 1, "ALL",   "ALL",       "[\"近端停车场引导满员后切换远端\",\"在各入口设置停车指示牌\"]"},
        {"*",       "shuttle_load",  ">=", 0.95, "触发运力不足告警",         "WARNING",  35, 1, "ALL",   "ALL",       "[\"调派最近空闲环保车前往支援\",\"通知调度中心增开班次\"]"},
        {"*",       "arrival_rate",  "<",  5,    "数据异常排查",             "INFO",     60, 0, "ALL",   "ALL",       ""},
        // ---- 周末差异化规则（更严格的阈值）----
        {"spot_01", "visitor_ratio", ">=", 0.85, "大氧吧广场提前预警（通用）","WARNING",  20, 1, "ALL",   "PLAZA",     "[\"在入口增派引导人员\",\"启动远端停车引导\",\"实时监控到达率\"]"},
        {"*",       "visitor_ratio", ">=", 0.85, "周末提前拥挤预警",         "WARNING",  25, 1, "WEEKEND","ALL",      "[\"提前 30 分钟通知周边景点\",\"启动景区广播引导\"]"},
        {"spot_05", "visitor_ratio", ">=", 0.95, "黄石寨入口限制进入",       "CRITICAL",  5, 1, "ALL",   "SUMMIT",    "[\"在入口设置限流栅栏\",\"引导游客改走鹞子寨入口\"]"},
        {"spot_04", "visitor_ratio", ">=", 0.8,  "入口类景点提前预警",       "WARNING",  22, 1, "ALL",   "ENTRANCE",  "[\"通知相邻景点分流准备\",\"安排志愿者引导\"]"},
    };
    for (const auto& r : seeds) {
        Database::instance().execParams(
            "INSERT INTO rule(spot_id,metric,operator,threshold,action,level,priority,"
            "scenario,applicable_spot_type,disposal_plan,status)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            {r.spot, r.metric, r.op, std::to_string(r.threshold), r.action, r.level,
             std::to_string(r.priority), r.scenario, r.spotType, r.disposal,
             std::to_string(r.enabled)});
    }
    logInfo("seeded rule (10 rows with scenario/disposal_plan)");
}

// 7 days of 10-minute flow history so charts and statistics have data before
// the simulation is started. Each record represents one sampling point.
void seedFlowHistory() {
    if (!isEmptyTable("flow_record")) return;

    struct Curve { const char* id; double peakRatio; };
    const Curve curves[] = {
        {"spot_01", 0.55}, {"spot_02", 0.85}, {"spot_03", 0.45}, {"spot_04", 0.90},
        {"spot_05", 0.50}, {"spot_06", 0.30},
    };

    auto spots = Database::instance().query("SELECT * FROM scenic_spot WHERE status=1 ORDER BY id");
    int rowsInserted = 0;

    Database::instance().exec("BEGIN");
    for (const auto& spot : spots) {
        const std::string spotId = spot.count("spot_id") ? spot.at("spot_id") : "";
        const int capacity = spot.count("instant_capacity") ? std::stoi(spot.at("instant_capacity")) : 0;
        const int cumulative = spot.count("cumulative_visitors") ? std::stoi(spot.at("cumulative_visitors")) : 0;

        double peak = 0.5;
        for (const auto& c : curves) {
            if (spotId == c.id) { peak = c.peakRatio; break; }
        }

        constexpr int kPoints = 7 * 24 * 6;      // 7 days at 10-minute intervals
        constexpr int kStepSeconds = 600;
        const int startCumulative = static_cast<int>(cumulative * 0.15);
        const int span = std::max(0, cumulative - startCumulative);

        for (int i = 0; i < kPoints; ++i) {
            const std::time_t t = std::time(nullptr) - static_cast<std::time_t>(kPoints - 1 - i) * kStepSeconds;
            const std::string ts = formatTime(t, "%Y-%m-%d %H:%M:%S");
            const std::tm tm = toLocalTm(t);
            const double hour = tm.tm_hour + tm.tm_min / 60.0;
            // Daily curve: low at night, peaking around 14:00.
            const double dayCurve = 0.55 + 0.45 * std::sin((hour - 6.0) / 24.0 * 2.0 * 3.14159265358979);
            const int current = std::max(0, static_cast<int>(std::lround(capacity * peak * dayCurve)));
            const int cum = startCumulative + static_cast<int>(span * (i + 1) / kPoints);
            const double ratio = capacity > 0 ? static_cast<double>(current) / capacity : 0.0;

            if (!Database::instance().execParams(
                    "INSERT INTO flow_record(record_time,spot_id,current_visitors,cumulative_visitors,"
                    "congestion_level,arrival_rate,departure_rate,is_deleted) VALUES(?,?,?,?,?,0,0,0)",
                    {ts, spotId, std::to_string(current), std::to_string(cum),
                     toString(levelFromRatio(ratio))})) {
                break;
            }
            ++rowsInserted;
        }
    }
    Database::instance().exec("COMMIT");
    logInfo("seeded flow_record (" + std::to_string(rowsInserted) + " rows)");
}

void seedEvents() {
    if (!isEmptyTable("event")) return;

    struct E { const char* type; const char* level; int minsAgo; int spotIdx; const char* desc; const char* suggestion; const char* status; };
    const E seeds[] = {
        {"OVERLOAD",          "CRITICAL",   3, 0, "当前在园 15600 人，超过瞬时容量 15000 人，已触发超载告警", "建议将部分游客分流至鹞子寨入口", "ACTIVE"},
        {"CONGESTION",        "HIGH",       8, 4, "当前在园 5600 人，拥挤度 70%，建议启动分流",           "建议将部分游客分流至闺门迎宾", "ACKNOWLEDGED"},
        {"PARKING_SHORTAGE",  "WARNING",   15, 0, "东门外部停车场剩余车位 12 个，低于阈值 30",             "建议引导车辆前往周边临时停车区", "ACTIVE"},
        {"CONGESTION",        "HIGH",      25, 0, "当前在园 10500 人，拥挤度 70%，持续 12 分钟",          "建议将部分游客分流至千里相会", "RESOLVED"},
        {"CAPACITY_SHORTAGE", "WARNING",   40, 5, "鹞子寨入口候车人数 180，环保车运力不足",                "建议调派最近空闲环保车前往支援", "ACTIVE"},
        {"CONGESTION",        "WARNING",   62, 1, "金鞭溪入口拥挤度 58%，接近阈值",                        "建议将部分游客分流至闺门迎宾", "CLOSED"},
        {"OVERLOAD",          "CRITICAL",  95, 0, "大氧吧广场在园 16200 人，超过瞬时容量 15000 人",         "建议将部分游客分流至金鞭溪入口", "RESOLVED"},
        {"PARKING_SHORTAGE",  "WARNING",  130, 1, "东门外部停车场剩余车位 22 个，低于阈值 30",             "建议引导车辆前往周边临时停车区", "IGNORED"},
        {"DATA_ANOMALY",      "INFO",     180, 0, "大氧吧广场流量数据 3 分钟未更新，已自动补位",           "", "RESOLVED"},
        {"CONGESTION",        "HIGH",     240, 2, "千里相会拥挤度 74%，持续 15 分钟",                      "建议将部分游客分流至闺门迎宾", "RESOLVED"},
        {"CAPACITY_SHORTAGE", "WARNING",  300, 4, "黄石寨入口站点环保车满载率 100%，等待时间上升",        "建议调派最近空闲环保车前往支援", "RESOLVED"},
        {"SYSTEM_ERROR",      "CRITICAL", 360, -1,"数据写库线程异常重启，已自动恢复",                    "", "CLOSED"},
        {"CONGESTION",        "WARNING",  420, 3, "闺门迎宾拥挤度 61%",                                    "建议将部分游客分流至金鞭溪入口", "CLOSED"},
        {"PARKING_SHORTAGE",  "INFO",     480, 2, "东门外部停车场剩余车位 120 个，恢复正常",               "", "CLOSED"},
        {"OVERLOAD",          "HIGH",     540, 0, "大氧吧广场在园 14400 人，持续 8 分钟后回落",            "建议将部分游客分流至黄石寨入口", "CLOSED"},
        {"CONGESTION",        "WARNING",  640, 1, "金鞭溪入口拥挤度 55%，趋势上升",                       "建议将部分游客分流至千里相会", "CLOSED"},
        {"DATA_ANOMALY",      "INFO",     720, 2, "检测到停车场上报数据跳变，已记录",                     "", "CLOSED"},
        {"CONGESTION",        "HIGH",     800, 4, "黄石寨入口拥挤度 78%，建议分流至金鞭溪入口",             "建议将部分游客分流至金鞭溪入口", "CLOSED"},
        {"CAPACITY_SHORTAGE", "WARNING",  900, 1, "环保车 07 号故障，在线运力下降",                        "建议调派最近空闲环保车前往支援", "CLOSED"},
        {"CONGESTION",        "INFO",    1000, 0, "吴家峪门票站到达速率 45 人/分钟，高于日常均值",          "", "CLOSED"},
    };

    for (const auto& e : seeds) {
        char spotId[16] = {0};
        if (e.spotIdx >= 0) std::snprintf(spotId, sizeof(spotId), "spot_%02d", e.spotIdx + 1);
        const std::string occur = timeStringMinutesAgo(e.minsAgo);
        const std::string updated =
            std::string(e.status) == "ACTIVE" ? "" : timeStringMinutesAgo(std::max(1, e.minsAgo - 5));
        Database::instance().execParams(
            "INSERT INTO event(event_type,occur_time,spot_id,level,description,suggestion,status,updated_at)"
            " VALUES(?,?,?,?,?,?,?,?)",
            {e.type, occur, spotId, e.level, e.desc, e.suggestion, e.status, updated});
    }
    logInfo("seeded event (20 rows)");
}

void seedSuggestions() {
    if (!isEmptyTable("scheduling_record")) return;

    struct S { const char* type; const char* content; const char* detail; int minsAgo; int spotIdx; const char* status; };
    const S seeds[] = {
        {"DIVERSION",         "大氧吧广场 → 金鞭溪入口：分流 120 人",   "目标景点剩余容量 2400，距离约 394 米，满足 90% 安全余量约束", 3,   1, "PENDING"},
        {"SHUTTLE_DISPATCH",  "调派环保车03号前往鹞子寨入口",           "鹞子寨入口候车 180 人，03 号当前空闲且距站点最近（1.2 公里）", 12,  6, "EXECUTED"},
        {"PARKING_GUIDE",     "引导车辆前往周边临时停车区",             "东门外部停车场剩余 12 位，已接近饱和",                        18,  0, "EXECUTED"},
        {"ENTRANCE_LIMIT",    "吴家峪门票站启动限流",                   "全景区在园人数达总容量 92%，建议入口放行速率降至 30 人/分钟", 30,  0, "PENDING"},
        {"RESOURCE_WARNING",  "环保车运力预警",                         "在线 12/15，满载率超过 85% 的车辆有 4 辆",                   45,  6, "EXECUTED"},
        {"DIVERSION",         "大氧吧广场 → 千里相会：分流 200 人",     "大氧吧广场拥挤度 108%，目标景点剩余容量 3500",               60,  1, "EXECUTED"},
        {"SHUTTLE_DISPATCH",  "调派环保车09号前往黄石寨入口",           "黄石寨入口站点满载率 100%，09 号距站点 0.8 公里",            95,  5, "REJECTED"},
        {"PARKING_GUIDE",     "东门外部停车场入口引导",                 "剩余车位 22 个，建议引导至周边临时停车区",                   130, 2, "EXECUTED"},
        {"DIVERSION",         "千里相会 → 闺门迎宾：分流 80 人",        "千里相会拥挤度 74%，闺门迎宾剩余容量 1200",                  180, 3, "EXECUTED"},
        {"RESOURCE_WARNING",  "停车场整体饱和预警",                     "全景区剩余车位 280，低于总车位 35%",                         240, 1, "REJECTED"},
        {"ENTRANCE_LIMIT",    "吴家峪门票站恢复正常放行",               "全景区在园人数回落至总容量 78%",                             300, 3, "EXECUTED"},
        {"SHUTTLE_DISPATCH",  "增开环保车加班车 2 趟",                  "早高峰预测到达率上升，建议提前增开",                         360, 4, "EXECUTED"},
    };

    for (const auto& s : seeds) {
        char spotId[16] = {0};
        std::snprintf(spotId, sizeof(spotId), "spot_%02d", std::min(6, std::max(1, s.spotIdx)) );
        const std::string taskId = "task_" + std::to_string(s.minsAgo);
        Database::instance().execParams(
            "INSERT INTO scheduling_record(task_id,spot_id,schedule_type,content,detail,"
            "suggestion_status,create_time,is_deleted) VALUES(?,?,?,?,?,?,?,0)",
            {taskId, spotId, s.type, s.content, s.detail, s.status,
             timeStringMinutesAgo(s.minsAgo)});
    }
    logInfo("seeded scheduling_record (12 rows)");
}

// Shuttle routes grouped by line; the user side shows per-route wait times.
// Distances derived from the internal grid (mapDistance).
void seedShuttleRoutes() {
    if (!isEmptyTable("shuttle_route")) return;
    struct R { const char* route; const char* from; const char* to; int dist; int wait; };
    const R seeds[] = {
        {"门票站线", "吴家峪门票站", "大氧吧广场", 1370, 8},
        {"门票站线", "大氧吧广场",   "金鞭溪入口",  394, 8},
        {"门票站线", "金鞭溪入口",   "千里相会",    539, 8},
        {"黄石寨线", "吴家峪门票站", "闺门迎宾",   1384, 12},
        {"黄石寨线", "闺门迎宾",     "黄石寨入口",  532, 12},
    };
    for (const auto& r : seeds) {
        Database::instance().execParams(
            "INSERT INTO shuttle_route(route_name,from_station,to_station,distance,waiting_minutes,status)"
            " VALUES(?,?,?,?,?,1)",
            {r.route, r.from, r.to, std::to_string(r.dist), std::to_string(r.wait)});
    }
    logInfo("seeded shuttle_route (5 rows)");
}

// POI navigation points. The gate and parking lot come from the reference
// material (real coordinates); service facilities sit next to the ticket
// station / main square at estimated positions.
void seedPois() {
    if (!isEmptyTable("poi")) return;
    struct P { const char* id; const char* name; const char* cat; double x; double y; const char* desc; };
    const P seeds[] = {
        {"gate_01",    "吴家峪东门闸机口",     "GATE",    100, 38, "景区主入口检票闸机"},
        {"park_p01",   "东门外部停车场",       "PARKING",  96, 32, "小型车 / 大巴"},
        {"station_01", "环保车乘降点 · 门票站", "STATION",  99, 37, "门票站线 / 黄石寨线起点"},
        {"toilet_01",  "卫生间 · 门票站",      "TOILET",   98, 36, "门票站广场东侧"},
        {"toilet_02",  "卫生间 · 大氧吧广场",  "TOILET",   66, 97, "大氧吧广场北侧"},
        {"service_01", "游客服务中心",         "SERVICE",  98, 39, "咨询 / 寄存 / 租赁"},
    };
    for (const auto& p : seeds) {
        Database::instance().execParams(
            "INSERT INTO poi(poi_id,name,category,location_x,location_y,latitude,longitude,description,status)"
            " VALUES(?,?,?,?,?,?,?,?,1)",
            {p.id, p.name, p.cat, std::to_string(p.x), std::to_string(p.y),
             std::to_string(mapToLat(p.y)), std::to_string(mapToLng(p.x)), p.desc});
    }

    // Link every active spot to all POIs with coordinate-derived distances.
    const auto spots = Database::instance().query("SELECT spot_id,location_x,location_y FROM scenic_spot WHERE status=1");
    const auto pois = Database::instance().query("SELECT poi_id,location_x,location_y FROM poi");
    int links = 0;
    Database::instance().exec("BEGIN");
    for (const auto& s : spots) {
        const double sx = toDoubleLocal(s.count("location_x") ? s.at("location_x") : "0");
        const double sy = toDoubleLocal(s.count("location_y") ? s.at("location_y") : "0");
        const std::string spotId = s.count("spot_id") ? s.at("spot_id") : "";
        for (const auto& p : pois) {
            const double px = toDoubleLocal(p.count("location_x") ? p.at("location_x") : "0");
            const double py = toDoubleLocal(p.count("location_y") ? p.at("location_y") : "0");
            Database::instance().execParams(
                "INSERT OR IGNORE INTO spot_poi(spot_id,poi_id,distance_m) VALUES(?,?,?)",
                {spotId, p.count("poi_id") ? p.at("poi_id") : "",
                 std::to_string(mapDistance(sx, sy, px, py))});
            ++links;
        }
    }
    Database::instance().exec("COMMIT");
    logInfo("seeded poi (6 rows, " + std::to_string(links) + " spot links)");
}

// Hourly whole-park forecast for today. The runtime refresh in
// UserSideController recomputes this from the last 7 days of flow_record
// once per hour; this seed only guarantees non-empty data on first boot.
void seedForecast() {
    const std::string today = dateStringDaysAgo(0);
    auto rows = Database::instance().queryParams(
        "SELECT COUNT(*) AS c FROM flow_forecast WHERE forecast_date=?", {today});
    if (!rows.empty() && rows[0]["c"] != "0") return;

    Database::instance().execParams("DELETE FROM flow_forecast WHERE forecast_date<?", {today});
    // Whole-park capacity = sum of all spot instant capacities (43000 with the
    // current six Zhangjiajie spots).
    int totalCapacity = 0;
    auto capRows = Database::instance().query("SELECT COALESCE(SUM(instant_capacity),0) AS c FROM scenic_spot WHERE status=1");
    if (!capRows.empty()) totalCapacity = std::stoi(capRows[0]["c"]);
    if (totalCapacity <= 0) totalCapacity = 20000;
    Database::instance().exec("BEGIN");
    for (int hour = 0; hour < 24; ++hour) {
        const double dayCurve = 0.55 + 0.45 * std::sin((hour - 6.0) / 24.0 * 2.0 * 3.14159265358979);
        const int predicted = std::max(0, static_cast<int>(std::lround(totalCapacity * 0.42 * dayCurve)));
        const double ratio = static_cast<double>(predicted) / totalCapacity;
        Database::instance().execParams(
            "INSERT OR REPLACE INTO flow_forecast(forecast_date,hour,predicted_visitors,predicted_level)"
            " VALUES(?,?,?,?)",
            {today, std::to_string(hour), std::to_string(predicted), toString(levelFromRatio(ratio))});
    }
    Database::instance().exec("COMMIT");
    logInfo("seeded flow_forecast (24 rows for " + today + ")");
}

// 对已存在的老数据库做增量迁移（ALTER TABLE ADD COLUMN）。
// SQLite 的 CREATE TABLE IF NOT EXISTS 不会给已有表加新列，必须显式迁移。
void migrateLegacySchema() {
    auto hasColumn = [](const std::string& table, const std::string& col) -> bool {
        auto rows = Database::instance().query("PRAGMA table_info(" + table + ")");
        for (const auto& r : rows) {
            if (field(r, "name") == col) return true;
        }
        return false;
    };

    auto addColumn = [&](const std::string& table, const std::string& colDef) {
        const auto space = colDef.find(' ');
        const std::string colName = colDef.substr(0, space);
        if (hasColumn(table, colName)) return;
        const std::string sql = "ALTER TABLE " + table + " ADD COLUMN " + colDef;
        if (Database::instance().exec(sql)) {
            logInfo("migrate: added " + table + "." + colName);
        } else {
            logWarn("migrate: " + Database::instance().lastError());
        }
    };

    addColumn("scenic_spot", "spot_type TEXT NOT NULL DEFAULT 'GENERAL'");
    addColumn("flow_forecast", "spot_id TEXT");
    addColumn("flow_forecast", "granularity TEXT NOT NULL DEFAULT 'HOURLY'");
    addColumn("flow_forecast", "tick_offset INTEGER");
    addColumn("flow_forecast", "q10 REAL");
    addColumn("flow_forecast", "q50 REAL");
    addColumn("flow_forecast", "q90 REAL");
    addColumn("flow_forecast", "uncertainty_width REAL");
    addColumn("rule", "scenario TEXT NOT NULL DEFAULT 'ALL'");
    addColumn("rule", "applicable_spot_type TEXT NOT NULL DEFAULT 'ALL'");
    addColumn("rule", "disposal_plan TEXT");

    // 老库上的 (forecast_date, hour) 唯一索引会让 TFT 的 5MIN 行互相覆盖，
    // 必须换成包含 spot_id/granularity/tick_offset 的唯一索引。
    Database::instance().exec("DROP INDEX IF EXISTS idx_forecast_date_hour");
    Database::instance().exec(
        "CREATE INDEX IF NOT EXISTS idx_forecast_date_hour ON flow_forecast(forecast_date, hour)");
    Database::instance().exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_forecast_tick "
        "ON flow_forecast(forecast_date, hour, tick_offset, spot_id, granularity)");
    Database::instance().exec(
        "CREATE INDEX IF NOT EXISTS idx_forecast_spot_time "
        "ON flow_forecast(spot_id, forecast_date, granularity)");
    Database::instance().exec(
        "CREATE INDEX IF NOT EXISTS idx_predlog_spot_time "
        "ON prediction_log(spot_id, predict_time)");
}

void backfillSpotTypes() {
    struct Mapping { const char* keyword; const char* type; };
    const Mapping mappings[] = {
        {"门票站", "ENTRANCE"}, {"闸机", "ENTRANCE"}, {"入口", "ENTRANCE"},
        {"广场", "PLAZA"}, {"中心", "PLAZA"},
        {"溪", "TRAIL"}, {"相会", "TRAIL"}, {"线路", "TRAIL"},
        {"观景台", "VIEWPOINT"}, {"观景", "VIEWPOINT"},
        {"寨", "SUMMIT"}, {"顶", "SUMMIT"},
    };
    auto rows = Database::instance().query("SELECT spot_id, name, spot_type FROM scenic_spot");
    for (const auto& r : rows) {
        const std::string type = field(r, "spot_type", "");
        if (!type.empty() && type != "GENERAL") continue;
        const std::string name = field(r, "name");
        const std::string spotId = field(r, "spot_id");
        const char* resolved = "GENERAL";
        for (const auto& m : mappings) {
            if (name.find(m.keyword) != std::string::npos) {
                resolved = m.type;
                break;
            }
        }
        Database::instance().execParams(
            "UPDATE scenic_spot SET spot_type=? WHERE spot_id=?",
            {resolved, spotId});
    }
}

void backfillRuleScenarios() {
    // 给 spot_01 的提前预警规则加周末差异化阈值 + 处置预案示范
    Database::instance().exec(
        "UPDATE rule SET scenario='WEEKEND', threshold=0.85, "
        "disposal_plan='[\"在入口增派引导人员\",\"启动远端停车引导\",\"实时监控到达率\"]' "
        "WHERE spot_id='spot_01' AND priority=20");
    // 给通用 visitor_ratio 规则加处置预案
    Database::instance().exec(
        "UPDATE rule SET disposal_plan='[\"通知相邻景点分流准备\",\"安排志愿者在分流节点\",\"开启景区广播提示\"]' "
        "WHERE disposal_plan IS NULL AND spot_id='*' AND metric='visitor_ratio'");
}

} // namespace

bool Schema::init(const std::string& schemaSqlPath) {
    std::ifstream in(schemaSqlPath);
    if (!in.is_open()) {
        logError("cannot open schema file: " + schemaSqlPath);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    if (!Database::instance().exec(ss.str())) {
        logError("failed to apply schema");
        return false;
    }

    // 增量迁移：对已存在的老数据库 ALTER TABLE 加新列
    migrateLegacySchema();

    seedSpots();
    seedParkings();
    seedShuttles();
    seedShuttleRoutes();
    seedRules();
    seedFlowHistory();
    seedEvents();
    seedSuggestions();
    seedPois();
    seedForecast();

    // 回填：对已有 seed 数据的新字段补值
    backfillSpotTypes();
    backfillRuleScenarios();

    return true;
}

} // namespace scenicflow
