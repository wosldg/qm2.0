#pragma once

#include <json/json.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "model/Enums.h"
#include "storage/Database.h"

namespace scenicflow {

// DB rows use snake_case columns; the frontend expects camelCase JSON fields.
// All conversions live here so every controller stays consistent.

inline int toInt(const std::string& s, int def = 0) {
    try { return std::stoi(s); } catch (...) { return def; }
}

inline double toDouble(const std::string& s, double def = 0) {
    try { return std::stod(s); } catch (...) { return def; }
}

inline Json::Value spotToJson(const Row& r) {
    int capacity = toInt(r.count("instant_capacity") ? r.at("instant_capacity") : "0");
    int current = toInt(r.count("current_visitors") ? r.at("current_visitors") : "0");
    double ratio = capacity > 0 ? static_cast<double>(current) / capacity : 0.0;

    Json::Value j;
    j["spotId"] = r.count("spot_id") ? r.at("spot_id") : "";
    j["name"] = r.count("name") ? r.at("name") : "";
    j["locationX"] = toDouble(r.count("location_x") ? r.at("location_x") : "0");
    j["locationY"] = toDouble(r.count("location_y") ? r.at("location_y") : "0");
    j["capacity"] = capacity;
    j["dailyCapacity"] = toInt(r.count("daily_capacity") ? r.at("daily_capacity") : "0");
    j["currentVisitors"] = current;
    j["totalIn"] = toInt(r.count("cumulative_visitors") ? r.at("cumulative_visitors") : "0");
    j["arrivalRate"] = toDouble(r.count("arrival_rate") ? r.at("arrival_rate") : "0");
    j["departureRate"] = toDouble(r.count("departure_rate") ? r.at("departure_rate") : "0");
    j["level"] = toString(levelFromRatio(ratio));
    return j;
}

inline Json::Value parkingToJson(const Row& r) {
    int total = toInt(r.count("total_spaces") ? r.at("total_spaces") : "0");
    int occupied = toInt(r.count("occupied_spaces") ? r.at("occupied_spaces") : "0");
    Json::Value j;
    j["parkingId"] = r.count("parking_id") ? r.at("parking_id") : "";
    j["name"] = r.count("name") ? r.at("name") : "";
    j["totalSpaces"] = total;
    j["occupiedSpaces"] = occupied;
    j["freeSpaces"] = total - occupied;
    return j;
}

inline Json::Value shuttleToJson(const Row& r) {
    Json::Value j;
    j["shuttleId"] = r.count("shuttle_id") ? r.at("shuttle_id") : "";
    j["name"] = r.count("name") ? r.at("name") : "";
    j["capacity"] = toInt(r.count("capacity") ? r.at("capacity") : "0");
    j["currentStation"] = r.count("current_station") ? r.at("current_station") : "";
    j["passengerCount"] = toInt(r.count("current_passengers") ? r.at("current_passengers") : "0");
    j["status"] = r.count("status") ? r.at("status") : "IDLE";
    return j;
}

inline std::string field(const Row& r, const std::string& key, const std::string& def = "") {
    auto it = r.find(key);
    return it == r.end() ? def : it->second;
}

// User-side plain-language conclusion for a congestion level (requirement 13.2):
// numbers are always paired with a 拥挤/适中/空闲 text verdict.
inline const char* levelTextOf(const std::string& level) {
    if (level == "OVERLOADED" || level == "CROWDED") return "拥挤";
    if (level == "MODERATE") return "适中";
    return "空闲";
}

// Queue minutes derived from the occupancy ratio (0..60, capped).
inline int queueMinutesOf(double ratio) {
    if (ratio <= 0) return 0;
    return static_cast<int>(std::min(60.0, std::round(ratio * 60.0)));
}

inline Json::Value eventToJson(const Row& r) {
    Json::Value j;
    j["eventId"] = toInt(field(r, "id"));
    j["type"] = field(r, "event_type");
    j["level"] = field(r, "level", "WARNING");
    const std::string spotId = field(r, "spot_id");
    if (!spotId.empty()) j["spotId"] = spotId;
    const std::string spotName = field(r, "spot_name");
    if (!spotName.empty()) j["spotName"] = spotName;
    j["description"] = field(r, "description");
    j["status"] = field(r, "status", "ACTIVE");
    const std::string suggestion = field(r, "suggestion");
    if (!suggestion.empty()) j["suggestion"] = suggestion;
    j["createdAt"] = field(r, "occur_time");
    const std::string updatedAt = field(r, "updated_at");
    if (!updatedAt.empty()) j["updatedAt"] = updatedAt;
    return j;
}

inline Json::Value schedulingToJson(const Row& r) {
    Json::Value j;
    j["suggestionId"] = toInt(field(r, "id"));
    j["type"] = field(r, "schedule_type");
    j["content"] = field(r, "content");
    const std::string detail = field(r, "detail");
    if (!detail.empty()) j["detail"] = detail;
    const std::string spotId = field(r, "spot_id");
    if (!spotId.empty()) j["spotId"] = spotId;
    j["status"] = field(r, "suggestion_status", "PENDING");
    j["createdAt"] = field(r, "create_time");
    return j;
}

inline Json::Value ruleToJson(const Row& r) {
    Json::Value j;
    j["ruleId"] = toInt(field(r, "id"));
    j["spotId"] = field(r, "spot_id", "*");
    j["metric"] = field(r, "metric");
    j["operator"] = field(r, "operator");
    j["threshold"] = toDouble(field(r, "threshold"));
    j["action"] = field(r, "action");
    j["level"] = field(r, "level", "WARNING");
    j["priority"] = toInt(field(r, "priority"), 50);
    j["enabled"] = field(r, "status", "1") == "1";
    j["scenario"] = field(r, "scenario", "ALL");
    j["applicableSpotType"] = field(r, "applicable_spot_type", "ALL");
    const std::string disposalPlan = field(r, "disposal_plan");
    if (!disposalPlan.empty()) j["disposalPlan"] = disposalPlan;
    return j;
}

inline Json::Value userToJson(const Row& r) {
    Json::Value j;
    j["userId"] = toInt(field(r, "id"));
    j["username"] = field(r, "username");
    j["role"] = field(r, "role");
    j["createdAt"] = field(r, "created_at");
    return j;
}

} // namespace scenicflow
