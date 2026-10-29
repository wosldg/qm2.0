#include "network/AdminControllers.h"

#include "core/Logger.h"
#include "core/TimeUtil.h"
#include "network/ApiResult.h"
#include "network/Mappers.h"
#include "network/RequestUtil.h"
#include "security/AuthService.h"
#include "storage/Database.h"

#include <set>

namespace scenicflow {

namespace {

bool isValidEventStatus(const std::string& s) {
    static const std::set<std::string> kValid = {
        "ACTIVE", "ACKNOWLEDGED", "RESOLVED", "CLOSED", "IGNORED"};
    return kValid.count(s) > 0;
}

bool isValidSuggestionStatus(const std::string& s) {
    static const std::set<std::string> kValid = {"PENDING", "EXECUTED", "REJECTED"};
    return kValid.count(s) > 0;
}

bool isValidEventLevel(const std::string& s) {
    static const std::set<std::string> kValid = {"INFO", "WARNING", "HIGH", "CRITICAL"};
    return kValid.count(s) > 0;
}

bool isValidMetric(const std::string& s) {
    static const std::set<std::string> kValid = {
        "visitor_ratio", "parking_free", "shuttle_load", "arrival_rate"};
    return kValid.count(s) > 0;
}

bool isValidOperator(const std::string& s) {
    static const std::set<std::string> kValid = {">", ">=", "<", "<="};
    return kValid.count(s) > 0;
}

bool isValidRole(const std::string& s) {
    static const std::set<std::string> kValid = {"admin", "operator", "viewer"};
    return kValid.count(s) > 0;
}

const char* kEventSelect =
    "SELECT e.*, s.name AS spot_name FROM event e "
    "LEFT JOIN scenic_spot s ON s.spot_id = e.spot_id WHERE 1=1";

} // namespace

// ==================== 事件 ====================

void EventController::list(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    std::string sql = kEventSelect;
    std::vector<std::string> params;
    const std::string type = req->getParameter("type");
    const std::string level = req->getParameter("level");
    const std::string status = req->getParameter("status");
    if (!type.empty())   { sql += " AND e.event_type=?"; params.push_back(type); }
    if (!level.empty())  { sql += " AND e.level=?";      params.push_back(level); }
    if (!status.empty()) { sql += " AND e.status=?";     params.push_back(status); }
    sql += " ORDER BY e.occur_time DESC, e.id DESC LIMIT 200";

    auto rows = Database::instance().queryParams(sql, params);
    Json::Value arr(Json::arrayValue);
    for (const auto& r : rows) arr.append(eventToJson(r));
    callback(ok(arr));
}

void EventController::updateStatus(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const int eventId = toInt(pathParam(req));
    auto body = req->getJsonObject();
    if (!body || !(*body).isMember("status")) {
        callback(failed(400, "缺少 status 字段"));
        return;
    }
    const std::string status = (*body)["status"].asString();
    if (!isValidEventStatus(status)) {
        callback(failed(400, "非法的事件状态"));
        return;
    }

    auto rows = Database::instance().queryParams("SELECT id FROM event WHERE id=?",
                                                 {std::to_string(eventId)});
    if (rows.empty()) {
        callback(failed(404, "事件不存在"));
        return;
    }

    Database::instance().execParams("UPDATE event SET status=?, updated_at=? WHERE id=?",
                                    {status, nowString(), std::to_string(eventId)});
    logInfo("event " + std::to_string(eventId) + " status -> " + status);
    callback(ok(Json::nullValue));
}

// ==================== 调度建议 ====================

void SchedulingController::list(const drogon::HttpRequestPtr&,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto rows = Database::instance().query(
        "SELECT * FROM scheduling_record WHERE is_deleted=0 ORDER BY create_time DESC, id DESC LIMIT 200");
    Json::Value arr(Json::arrayValue);
    for (const auto& r : rows) arr.append(schedulingToJson(r));
    callback(ok(arr));
}

void SchedulingController::detail(const drogon::HttpRequestPtr& req,
                                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto rows = Database::instance().queryParams(
        "SELECT * FROM scheduling_record WHERE id=? AND is_deleted=0",
        {pathParam(req)});
    if (rows.empty()) {
        callback(failed(404, "调度建议不存在"));
        return;
    }
    callback(ok(schedulingToJson(rows[0])));
}

void SchedulingController::updateStatus(const drogon::HttpRequestPtr& req,
                                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto body = req->getJsonObject();
    if (!body || !(*body).isMember("status")) {
        callback(failed(400, "缺少 status 字段"));
        return;
    }
    const std::string status = (*body)["status"].asString();
    if (!isValidSuggestionStatus(status)) {
        callback(failed(400, "非法的建议状态"));
        return;
    }

    const std::string id = pathParam(req);
    auto rows = Database::instance().queryParams(
        "SELECT id FROM scheduling_record WHERE id=? AND is_deleted=0", {id});
    if (rows.empty()) {
        callback(failed(404, "调度建议不存在"));
        return;
    }

    Database::instance().execParams("UPDATE scheduling_record SET suggestion_status=? WHERE id=?",
                                    {status, id});
    logInfo("suggestion " + id + " status -> " + status);
    callback(ok(Json::nullValue));
}

// ==================== 规则 ====================

void RuleController::list(const drogon::HttpRequestPtr&,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto rows = Database::instance().query("SELECT * FROM rule ORDER BY priority, id");
    Json::Value arr(Json::arrayValue);
    for (const auto& r : rows) arr.append(ruleToJson(r));
    callback(ok(arr));
}

void RuleController::create(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto body = req->getJsonObject();
    if (!body) {
        callback(failed(400, "请求体不是合法 JSON"));
        return;
    }

    const std::string spotId = (*body).get("spotId", "*").asString();
    const std::string metric = (*body).get("metric", "").asString();
    const std::string op = (*body).get("operator", "").asString();
    const std::string action = (*body).get("action", "").asString();
    const std::string level = (*body).get("level", "WARNING").asString();

    if (!isValidMetric(metric))   { callback(failed(400, "非法 metric"));   return; }
    if (!isValidOperator(op))     { callback(failed(400, "非法 operator")); return; }
    if (!isValidEventLevel(level)){ callback(failed(400, "非法 level"));    return; }
    if (!(*body).isMember("threshold")) {
        callback(failed(400, "缺少 threshold"));
        return;
    }

    const double threshold = (*body)["threshold"].asDouble();
    const int priority = (*body).get("priority", 50).asInt();
    const bool enabled = (*body).get("enabled", true).asBool();
    const std::string scenario = (*body).get("scenario", "ALL").asString();
    const std::string disposalPlan = (*body).get("disposalPlan", "").asString();
    if (scenario != "ALL" && scenario != "WEEKDAY" && scenario != "WEEKEND") {
        callback(failed(400, "非法 scenario"));
        return;
    }

    if (!Database::instance().execParams(
            "INSERT INTO rule(spot_id,metric,operator,threshold,action,level,priority,status,"
            "scenario,disposal_plan) VALUES(?,?,?,?,?,?,?,?,?,?)",
            {spotId, metric, op, std::to_string(threshold), action, level,
             std::to_string(priority), enabled ? "1" : "0", scenario, disposalPlan})) {
        callback(failed(500, "规则创建失败"));
        return;
    }

    auto rows = Database::instance().queryParams("SELECT * FROM rule WHERE id=?",
                                                 {std::to_string(Database::instance().lastInsertId())});
    callback(ok(rows.empty() ? Json::Value(Json::objectValue) : ruleToJson(rows[0])));
}

void RuleController::update(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string id = pathParam(req);
    auto body = req->getJsonObject();
    if (!body) {
        callback(failed(400, "请求体不是合法 JSON"));
        return;
    }

    auto existing = Database::instance().queryParams("SELECT * FROM rule WHERE id=?", {id});
    if (existing.empty()) {
        callback(failed(404, "规则不存在"));
        return;
    }

    // Partial update: only overwrite fields present in the request body.
    const Row& cur = existing[0];
    const std::string spotId = (*body).get("spotId", field(cur, "spot_id", "*")).asString();
    const std::string metric = (*body).get("metric", field(cur, "metric")).asString();
    const std::string op = (*body).get("operator", field(cur, "operator")).asString();
    const std::string action = (*body).get("action", field(cur, "action")).asString();
    const std::string level = (*body).get("level", field(cur, "level", "WARNING")).asString();
    const double threshold = (*body).get("threshold", toDouble(field(cur, "threshold"))).asDouble();
    const int priority = (*body).get("priority", toInt(field(cur, "priority"), 50)).asInt();
    const bool enabled = (*body).get("enabled", field(cur, "status", "1") == "1").asBool();
    const std::string scenario = (*body).get("scenario", field(cur, "scenario", "ALL")).asString();
    const std::string disposalPlan =
        (*body).get("disposalPlan", field(cur, "disposal_plan")).asString();

    if (!isValidMetric(metric) || !isValidOperator(op) || !isValidEventLevel(level)) {
        callback(failed(400, "规则字段非法"));
        return;
    }
    if (scenario != "ALL" && scenario != "WEEKDAY" && scenario != "WEEKEND") {
        callback(failed(400, "非法 scenario"));
        return;
    }

    Database::instance().execParams(
        "UPDATE rule SET spot_id=?, metric=?, operator=?, threshold=?, action=?, level=?,"
        " priority=?, status=?, scenario=?, disposal_plan=? WHERE id=?",
        {spotId, metric, op, std::to_string(threshold), action, level,
         std::to_string(priority), enabled ? "1" : "0", scenario, disposalPlan, id});

    auto rows = Database::instance().queryParams("SELECT * FROM rule WHERE id=?", {id});
    callback(ok(ruleToJson(rows[0])));
}

void RuleController::remove(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string id = pathParam(req);
    auto rows = Database::instance().queryParams("SELECT id FROM rule WHERE id=?", {id});
    if (rows.empty()) {
        callback(failed(404, "规则不存在"));
        return;
    }
    Database::instance().execParams("DELETE FROM rule WHERE id=?", {id});
    logInfo("rule " + id + " deleted");
    callback(ok(Json::nullValue));
}

// ==================== 用户 ====================

void UserController::list(const drogon::HttpRequestPtr&,
                          std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto rows = Database::instance().query("SELECT * FROM user ORDER BY id");
    Json::Value arr(Json::arrayValue);
    for (const auto& r : rows) arr.append(userToJson(r));
    callback(ok(arr));
}

void UserController::create(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    auto body = req->getJsonObject();
    if (!body) {
        callback(failed(400, "请求体不是合法 JSON"));
        return;
    }
    const std::string username = (*body).get("username", "").asString();
    const std::string password = (*body).get("password", "").asString();
    const std::string role = (*body).get("role", "").asString();

    if (username.empty() || password.empty()) {
        callback(failed(400, "用户名和密码不能为空"));
        return;
    }
    if (!isValidRole(role)) {
        callback(failed(400, "非法角色"));
        return;
    }

    auto dup = Database::instance().queryParams("SELECT id FROM user WHERE username=?", {username});
    if (!dup.empty()) {
        callback(failed(409, "用户名已存在"));
        return;
    }

    Database::instance().execParams(
        "INSERT INTO user(username,password_hash,role,created_at) VALUES(?,?,?,?)",
        {username, AuthService::hashPassword(password), role, nowString()});

    auto rows = Database::instance().queryParams(
        "SELECT * FROM user WHERE username=?", {username});
    logInfo("user created: " + username + " (" + role + ")");
    callback(ok(rows.empty() ? Json::Value(Json::objectValue) : userToJson(rows[0])));
}

void UserController::remove(const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string id = pathParam(req);
    auto rows = Database::instance().queryParams("SELECT * FROM user WHERE id=?", {id});
    if (rows.empty()) {
        callback(failed(404, "用户不存在"));
        return;
    }
    if (rows[0]["username"] == "admin") {
        callback(failed(403, "内置管理员不可删除"));
        return;
    }

    Database::instance().execParams("DELETE FROM user WHERE id=?", {id});
    Database::instance().execParams("DELETE FROM auth_token WHERE username=?",
                                    {rows[0]["username"]});
    logInfo("user deleted: " + rows[0]["username"]);
    callback(ok(Json::nullValue));
}

} // namespace scenicflow
