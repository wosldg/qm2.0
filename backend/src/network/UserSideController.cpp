#include "network/UserSideController.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "core/Logger.h"
#include "core/Settings.h"
#include "core/TimeUtil.h"
#include "network/ApiResult.h"
#include "network/Mappers.h"
#include "network/RequestUtil.h"
#include "storage/Database.h"

using namespace scenicflow;

namespace {

struct SpotBrief {
    std::string spotId;
    std::string name;
    double x = 0, y = 0;
    double lat = 0, lng = 0;
    int capacity = 0;
    int current = 0;
    double ratio = 0;
    std::string level;
    std::string openTime, closeTime;
};

SpotBrief toBrief(const Row& r) {
    SpotBrief b;
    b.spotId = field(r, "spot_id");
    b.name = field(r, "name");
    b.x = toDouble(field(r, "location_x"));
    b.y = toDouble(field(r, "location_y"));
    b.lat = toDouble(field(r, "latitude"));
    b.lng = toDouble(field(r, "longitude"));
    b.capacity = toInt(field(r, "instant_capacity"));
    b.current = toInt(field(r, "current_visitors"));
    b.ratio = b.capacity > 0 ? static_cast<double>(b.current) / b.capacity : 0.0;
    b.level = toString(levelFromRatio(b.ratio));
    b.openTime = field(r, "open_time", "08:00");
    b.closeTime = field(r, "close_time", "18:00");
    return b;
}

Json::Value briefToJson(const SpotBrief& b, bool withLocation) {
    Json::Value j;
    j["spotId"] = b.spotId;
    j["name"] = b.name;
    j["level"] = b.level;
    j["levelText"] = levelTextOf(b.level);
    j["queueMinutes"] = queueMinutesOf(b.ratio);
    j["openTime"] = b.openTime;
    j["closeTime"] = b.closeTime;
    j["currentVisitors"] = b.current;
    if (withLocation) {
        j["locationX"] = b.x;
        j["locationY"] = b.y;
        j["latitude"] = b.lat;
        j["longitude"] = b.lng;
    }
    return j;
}

const char* categoryTextOf(const std::string& category) {
    if (category == "TOILET") return "卫生间";
    if (category == "PARKING") return "停车场";
    if (category == "STATION") return "车站";
    if (category == "SERVICE") return "服务点";
    if (category == "MEDICAL") return "医疗点";
    if (category == "GATE") return "闸机";
    return "点位";
}

double mapDistance(double x1, double y1, double x2, double y2) {
    const double dx = x1 - x2, dy = y1 - y2;
    return std::sqrt(dx * dx + dy * dy);
}

// Recomputes today's hourly forecast at most once per hour.
// P1-5: 优先使用 TFT 模型的 per-spot 5min 预测聚合为全园小时级；
//       如果没有 TFT 数据（模型未 ready / 历史不足），降级为 7 天均值方案。
void refreshForecastIfNeeded() {
    const std::string today = dateStringDaysAgo(0);
    auto rows = Database::instance().queryParams(
        "SELECT created_at FROM flow_forecast WHERE forecast_date=? AND spot_id IS NULL"
        " ORDER BY id DESC LIMIT 1", {today});
    if (!rows.empty()) {
        const long long ageMin = (nowMillis() - parseMillis(field(rows[0], "created_at"))) / 60000;
        if (ageMin < 60) return;
    }

    // ---- 优先尝试 TFT 数据 ----
    // 取最新的 granularity='5MIN' + spot_id 非空的 per-spot 预测，
    // 聚合为全园小时级（sum 所有 spot 在该小时的 q50）
    auto tftRows = Database::instance().queryParams(
        "SELECT hour, tick_offset, q50 FROM flow_forecast"
        " WHERE forecast_date=? AND spot_id IS NOT NULL AND spot_id != ''"
        " AND granularity='5MIN' AND q50 IS NOT NULL"
        " ORDER BY created_at DESC",
        {today});

    bool usedTft = false;
    if (!tftRows.empty()) {
        // 用 std::map<hour, sum> 聚合
        std::map<int, int> hourlySum;
        for (const auto& r : tftRows) {
            const int h = toInt(field(r, "hour"), -1);
            if (h < 0 || h > 23) continue;
            hourlySum[h] += static_cast<int>(std::lround(toDouble(field(r, "q50"))));
        }
        if (!hourlySum.empty()) {
            const int totalCapacity = std::max(1, currentSettings().totalCapacity);
            Database::instance().exec("BEGIN");
            Database::instance().execParams(
                "DELETE FROM flow_forecast WHERE forecast_date=? AND spot_id IS NULL"
                " AND granularity='HOURLY'", {today});
            for (int h = 0; h < 24; ++h) {
                int predicted = hourlySum.count(h) ? hourlySum[h] : 0;
                if (predicted <= 0) {
                    // TFT 没有覆盖到的小时，用最近有数据的小时值
                    // 找前后最近的非零值
                    int fallback = 0;
                    for (int delta = 1; delta < 24; ++delta) {
                        if (hourlySum.count(h - delta)) { fallback = hourlySum[h - delta]; break; }
                        if (hourlySum.count(h + delta)) { fallback = hourlySum[h + delta]; break; }
                    }
                    predicted = fallback;
                }
                const double ratio = totalCapacity > 0 ? static_cast<double>(predicted) / totalCapacity : 0;
                Database::instance().execParams(
                    "INSERT OR REPLACE INTO flow_forecast(forecast_date,hour,predicted_visitors,predicted_level,created_at)"
                    " VALUES(?,?,?,?,?)",
                    {today, std::to_string(h), std::to_string(std::max(0, predicted)),
                     toString(levelFromRatio(ratio)), nowString()});
            }
            Database::instance().exec("COMMIT");
            usedTft = true;
            logInfo("refreshForecastIfNeeded: used TFT aggregated forecast (" +
                    std::to_string(hourlySum.size()) + " hours with data)");
        }
    }

    if (usedTft) return;

    // ---- 降级：7 天均值方案 ----
    auto daily = Database::instance().query(
        "SELECT substr(record_time,1,13) AS dh, SUM(current_visitors) AS tot FROM flow_record"
        " WHERE is_deleted=0 AND record_time>=datetime('now','-7 days','localtime') GROUP BY dh");
    std::vector<double> sum(24, 0.0);
    std::vector<int> count(24, 0);
    for (const auto& r : daily) {
        const std::string dh = field(r, "dh");
        if (dh.size() < 13) continue;
        const int h = toInt(dh.substr(11, 2), -1);
        if (h < 0 || h > 23) continue;
        sum[h] += toDouble(field(r, "tot"));
        count[h] += 1;
    }

    const int curHour = toLocalTm(std::time(nullptr)).tm_hour;
    double correction = 1.0;
    auto latest = Database::instance().query(
        "SELECT SUM(current_visitors) AS tot FROM flow_record WHERE is_deleted=0 AND record_time=("
        "SELECT MAX(record_time) FROM flow_record WHERE is_deleted=0)");
    if (!latest.empty() && count[curHour] > 0 && sum[curHour] > 0) {
        const int todayTotal = toInt(field(latest[0], "tot"));
        const double avg7 = sum[curHour] / count[curHour];
        correction = std::min(1.3, std::max(0.7, todayTotal / avg7));
    }

    const int totalCapacity = std::max(1, currentSettings().totalCapacity);
    Database::instance().exec("BEGIN");
    Database::instance().execParams(
        "DELETE FROM flow_forecast WHERE forecast_date=? AND spot_id IS NULL"
        " AND granularity='HOURLY'", {today});
    for (int h = 0; h < 24; ++h) {
        const double avg7 = count[h] > 0 ? sum[h] / count[h] : 0.0;
        const int predicted = std::max(0, static_cast<int>(std::lround(avg7 * correction)));
        Database::instance().execParams(
            "INSERT OR REPLACE INTO flow_forecast(forecast_date,hour,predicted_visitors,predicted_level,created_at)"
            " VALUES(?,?,?,?,?)",
            {today, std::to_string(h), std::to_string(predicted),
             toString(levelFromRatio(static_cast<double>(predicted) / totalCapacity)), nowString()});
    }
    Database::instance().exec("COMMIT");
}

// Peak window = consecutive hours whose forecast is >= 80% of the daily max.
struct PeakWindow {
    int start = -1, end = -1, maxHour = -1;
    int maxValue = 0;
};

PeakWindow loadPeakWindow(const std::string& today) {
    auto fc = Database::instance().queryParams(
        "SELECT hour,predicted_visitors FROM flow_forecast WHERE forecast_date=? ORDER BY hour", {today});
    std::vector<std::pair<int, int>> pts;  // (hour, predicted)
    PeakWindow w;
    for (const auto& r : fc) {
        const int h = toInt(field(r, "hour"), -1);
        const int v = toInt(field(r, "predicted_visitors"));
        if (h < 0) continue;
        pts.emplace_back(h, v);
        if (v > w.maxValue) {
            w.maxValue = v;
            w.maxHour = h;
        }
    }
    auto valAt = [&](int hour) -> int {
        for (const auto& p : pts) if (p.first == hour) return p.second;
        return 0;
    };
    // Peak window = the continuous stretch around the daily maximum whose
    // forecast stays at or above 80% of that maximum.
    const int threshold = static_cast<int>(w.maxValue * 0.8);
    int s = w.maxHour, e = w.maxHour;
    if (threshold > 0 && w.maxHour >= 0) {
        while (s - 1 >= 0 && valAt(s - 1) >= threshold) --s;
        while (e + 1 <= 23 && valAt(e + 1) >= threshold) ++e;
    }
    w.start = s;
    w.end = e;
    return w;
}

} // namespace

namespace scenicflow {

void UserSideController::overview(const drogon::HttpRequestPtr&,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const auto spots = Database::instance().query("SELECT * FROM scenic_spot WHERE status=1 ORDER BY id");
    const int totalCapacity = std::max(1, currentSettings().totalCapacity);

    Json::Value spotArr(Json::arrayValue);
    int totalCurrent = 0;
    int crowdedCount = 0;
    for (const auto& r : spots) {
        const SpotBrief b = toBrief(r);
        totalCurrent += b.current;
        if (b.level == "CROWDED" || b.level == "OVERLOADED") ++crowdedCount;
        // Persist the derived queue minutes so the value is a stored cache too.
        Database::instance().execParams(
            "UPDATE scenic_spot SET queue_minutes=? WHERE spot_id=?",
            {std::to_string(queueMinutesOf(b.ratio)), b.spotId});
        spotArr.append(briefToJson(b, true));
    }

    const double ratio = static_cast<double>(totalCurrent) / totalCapacity;
    const std::string overallLevel = toString(levelFromRatio(ratio));

    refreshForecastIfNeeded();
    const std::string today = dateStringDaysAgo(0);
    const PeakWindow peak = loadPeakWindow(today);
    const int curHour = toLocalTm(std::time(nullptr)).tm_hour;
    const bool isPeak = peak.start >= 0 && curHour >= peak.start && curHour <= peak.end;

    Json::Value advice;
    advice["isPeak"] = isPeak;
    if (peak.start >= 0) {
        advice["peakStart"] = peak.start;
        advice["peakEnd"] = peak.end;
        char msg[96];
        if (isPeak) {
            std::snprintf(msg, sizeof(msg), "当前为入园高峰，建议 %02d:00 后前往热门景点",
                          std::min(23, peak.end + 1));
        } else {
            std::snprintf(msg, sizeof(msg), "当前非高峰时段（高峰 %02d:00~%02d:00），可放心游览热门景点",
                          peak.start, peak.end);
        }
        advice["message"] = msg;
    } else {
        advice["message"] = "今日客流预测数据生成中";
    }

    Json::Value conclusion;
    conclusion["level"] = overallLevel;
    conclusion["text"] = levelTextOf(overallLevel);
    conclusion["currentVisitors"] = totalCurrent;
    conclusion["totalCapacity"] = totalCapacity;
    conclusion["ratio"] = ratio;
    conclusion["crowdedSpotCount"] = crowdedCount;

    Json::Value data;
    data["conclusion"] = conclusion;
    data["peakAdvice"] = advice;
    data["spots"] = spotArr;
    data["updatedAt"] = nowString();
    callback(ok(data));
}

void UserSideController::rankings(const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    std::string type = req->getParameter("type");
    if (type.empty()) type = "avoid";
    if (type != "avoid" && type != "crowded") {
        callback(failed(400, "type 仅支持 avoid 或 crowded"));
        return;
    }

    const std::string expr = "CAST(current_visitors AS REAL)/instant_capacity";
    const std::string sql = type == "avoid"
        ? "SELECT * FROM scenic_spot WHERE status=1 AND instant_capacity>0 ORDER BY " + expr + " ASC LIMIT 5"
        : "SELECT * FROM scenic_spot WHERE status=1 AND instant_capacity>0 AND " + expr +
              ">=0.6 ORDER BY " + expr + " DESC LIMIT 5";

    Json::Value arr(Json::arrayValue);
    for (const auto& r : Database::instance().query(sql)) {
        arr.append(briefToJson(toBrief(r), false));
    }
    Json::Value data;
    data["type"] = type;
    data["list"] = arr;
    callback(ok(data));
}

void UserSideController::forecast(const drogon::HttpRequestPtr&,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    refreshForecastIfNeeded();
    const std::string today = dateStringDaysAgo(0);
    const PeakWindow peak = loadPeakWindow(today);

    auto fc = Database::instance().queryParams(
        "SELECT hour,predicted_visitors,predicted_level FROM flow_forecast WHERE forecast_date=? ORDER BY hour",
        {today});
    Json::Value points(Json::arrayValue);
    for (const auto& r : fc) {
        Json::Value p;
        p["hour"] = toInt(field(r, "hour"));
        p["predictedVisitors"] = toInt(field(r, "predicted_visitors"));
        p["level"] = field(r, "predicted_level");
        p["levelText"] = levelTextOf(p["level"].asString());
        points.append(p);
    }

    Json::Value data;
    data["date"] = today;
    data["points"] = points;
    data["peakStart"] = peak.start;
    data["peakEnd"] = peak.end;
    callback(ok(data));
}

void UserSideController::parking(const drogon::HttpRequestPtr&,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto rows = Database::instance().query(
        "SELECT * FROM parking_lot WHERE status=1 ORDER BY distance_m ASC");
    Json::Value arr(Json::arrayValue);
    for (const auto& r : rows) {
        const int total = toInt(field(r, "total_spaces"));
        const int occupied = toInt(field(r, "occupied_spaces"));
        const int free = std::max(0, total - occupied);
        Json::Value j;
        j["parkingId"] = field(r, "parking_id");
        j["name"] = field(r, "name");
        j["freeSpaces"] = free;
        j["distanceM"] = toInt(field(r, "distance_m"));
        j["latitude"] = toDouble(field(r, "latitude"));
        j["longitude"] = toDouble(field(r, "longitude"));
        j["statusText"] = free < 20 ? "余位紧张" : "余位充足";
        arr.append(j);
    }
    callback(ok(arr));
}

void UserSideController::shuttleRoutes(const drogon::HttpRequestPtr&,
                                       std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto rows = Database::instance().query(
        "SELECT * FROM shuttle_route WHERE status=1 ORDER BY id");
    std::vector<std::string> order;
    std::vector<Json::Value> groups;
    for (const auto& r : rows) {
        const std::string name = field(r, "route_name", "未命名线路");
        Json::Value* g = nullptr;
        for (auto& cand : groups) {
            if (cand["routeName"].asString() == name) { g = &cand; break; }
        }
        if (!g) {
            Json::Value nj;
            nj["routeName"] = name;
            nj["stations"] = Json::Value(Json::arrayValue);
            nj["stations"].append(field(r, "from_station"));
            nj["waitingMinutes"] = toInt(field(r, "waiting_minutes"));
            nj["totalDistanceM"] = 0;
            groups.push_back(nj);
            order.push_back(name);
            g = &groups.back();
        }
        (*g)["stations"].append(field(r, "to_station"));
        (*g)["totalDistanceM"] = (*g)["totalDistanceM"].asInt() + toInt(field(r, "distance"));
    }

    Json::Value arr(Json::arrayValue);
    for (const auto& name : order) {
        for (const auto& g : groups) {
            if (g["routeName"].asString() == name) { arr.append(g); break; }
        }
    }
    callback(ok(arr));
}

void UserSideController::pois(const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string category = req->getParameter("category");
    if (!category.empty() && category != "TOILET" && category != "PARKING" &&
        category != "STATION" && category != "SERVICE" && category != "MEDICAL" &&
        category != "GATE") {
        callback(failed(400, "非法的点位类别"));
        return;
    }
    const std::string sql = category.empty()
        ? "SELECT * FROM poi WHERE status=1 ORDER BY category, id"
        : "SELECT * FROM poi WHERE status=1 AND category='" + category + "' ORDER BY id";

    Json::Value arr(Json::arrayValue);
    for (const auto& r : Database::instance().query(sql)) {
        Json::Value j;
        j["poiId"] = field(r, "poi_id");
        j["name"] = field(r, "name");
        j["category"] = field(r, "category");
        j["categoryText"] = categoryTextOf(j["category"].asString());
        j["locationX"] = toDouble(field(r, "location_x"));
        j["locationY"] = toDouble(field(r, "location_y"));
        j["latitude"] = toDouble(field(r, "latitude"));
        j["longitude"] = toDouble(field(r, "longitude"));
        const std::string desc = field(r, "description");
        if (!desc.empty()) j["description"] = desc;
        arr.append(j);
    }
    callback(ok(arr));
}

void UserSideController::recommendRoutes(const drogon::HttpRequestPtr& req,
                                         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    std::string type = req->getParameter("type");
    if (type.empty()) type = "avoid";
    if (type != "avoid" && type != "classic") {
        callback(failed(400, "type 仅支持 avoid 或 classic"));
        return;
    }

    std::vector<SpotBrief> nodes;
    for (const auto& r : Database::instance().query("SELECT * FROM scenic_spot WHERE status=1")) {
        const SpotBrief b = toBrief(r);
        // Avoid-route skips overloaded/crowded spots (ratio >= 0.9).
        if (type == "avoid" && b.ratio >= 0.9) continue;
        nodes.push_back(b);
    }
    if (nodes.empty()) {
        Json::Value data;
        data["type"] = type;
        data["name"] = type == "avoid" ? "避峰路线" : "经典路线";
        data["steps"] = Json::Value(Json::arrayValue);
        data["totalDistanceM"] = 0;
        data["estimatedMinutes"] = 0;
        data["message"] = type == "avoid" ? "当前各景点均较拥挤，建议稍后再试" : "";
        callback(ok(data));
        return;
    }

    // Nearest-neighbor chain over map coordinates, starting from the node
    // closest to the scenic-area entrance side (smallest x + y).
    std::vector<bool> used(nodes.size(), false);
    size_t cur = 0;
    double best = 1e18;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const double d = nodes[i].x + nodes[i].y;
        if (d < best) { best = d; cur = i; }
    }

    Json::Value steps(Json::arrayValue);
    double totalUnits = 0;
    int order = 1;
    while (true) {
        used[cur] = true;
        const SpotBrief& b = nodes[cur];
        Json::Value s = briefToJson(b, true);
        s["order"] = order++;
        steps.append(s);
        size_t next = nodes.size();
        double nextDist = 1e18;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (used[i]) continue;
            const double d = mapDistance(b.x, b.y, nodes[i].x, nodes[i].y);
            if (d < nextDist) { nextDist = d; next = i; }
        }
        if (next == nodes.size()) break;
        totalUnits += nextDist;
        cur = next;
    }

    const int totalDistance = static_cast<int>(std::lround(totalUnits * 20.0));
    Json::Value data;
    data["type"] = type;
    data["name"] = type == "avoid" ? "避峰路线" : "经典路线";
    data["steps"] = steps;
    data["totalDistanceM"] = totalDistance;
    data["estimatedMinutes"] = totalDistance / 80 + static_cast<int>(steps.size()) * 30;
    callback(ok(data));
}

void UserSideController::spotDetail(const drogon::HttpRequestPtr& req,
                                    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string spotId = pathParam(req, 0);
    auto rows = Database::instance().queryParams("SELECT * FROM scenic_spot WHERE spot_id=?", {spotId});
    if (rows.empty()) {
        callback(failed(404, "景点不存在"));
        return;
    }

    const SpotBrief b = toBrief(rows[0]);
    Database::instance().execParams(
        "UPDATE scenic_spot SET queue_minutes=? WHERE spot_id=?",
        {std::to_string(queueMinutesOf(b.ratio)), b.spotId});

    // Today's hourly trend (weakly presented on the detail page).
    const std::string todayStart = dateStringDaysAgo(0) + " 00:00:00";
    auto trendRows = Database::instance().queryParams(
        "SELECT substr(record_time,12,2) AS h, AVG(current_visitors) AS v FROM flow_record"
        " WHERE spot_id=? AND is_deleted=0 AND record_time>=? GROUP BY h ORDER BY h",
        {spotId, todayStart});
    Json::Value trend(Json::arrayValue);
    for (const auto& r : trendRows) {
        Json::Value p;
        p["hour"] = toInt(field(r, "h"));
        p["visitors"] = static_cast<int>(std::lround(toDouble(field(r, "v"))));
        trend.append(p);
    }

    // Nearby facilities ordered by walking distance.
    auto poiRows = Database::instance().queryParams(
        "SELECT p.poi_id,p.name,p.category,p.latitude,p.longitude,sp.distance_m"
        " FROM spot_poi sp JOIN poi p ON p.poi_id=sp.poi_id"
        " WHERE sp.spot_id=? ORDER BY sp.distance_m ASC",
        {spotId});
    Json::Value pois(Json::arrayValue);
    for (const auto& r : poiRows) {
        Json::Value j;
        j["poiId"] = field(r, "poi_id");
        j["name"] = field(r, "name");
        j["category"] = field(r, "category");
        j["categoryText"] = categoryTextOf(j["category"].asString());
        j["distanceM"] = toInt(field(r, "distance_m"));
        j["latitude"] = toDouble(field(r, "latitude"));
        j["longitude"] = toDouble(field(r, "longitude"));
        pois.append(j);
    }

    Json::Value data = briefToJson(b, false);
    data["currentVisitors"] = b.current;
    data["capacity"] = b.capacity;
    data["trend"] = trend;
    data["pois"] = pois;
    callback(ok(data));
}

} // namespace scenicflow
