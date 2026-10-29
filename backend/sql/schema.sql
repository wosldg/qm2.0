-- ScenicFlow database schema (SQLite)
-- Field names are snake_case in DB; controllers map them to the camelCase JSON the frontend expects.

CREATE TABLE IF NOT EXISTS scenic_spot (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    spot_id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    spot_type TEXT NOT NULL DEFAULT 'GENERAL',      -- ENTRANCE/PLAZA/TRAIL/VIEWPOINT/SUMMIT
    instant_capacity INTEGER NOT NULL,
    daily_capacity INTEGER NOT NULL,
    current_visitors INTEGER NOT NULL DEFAULT 0,
    cumulative_visitors INTEGER NOT NULL DEFAULT 0,
    queue_minutes INTEGER NOT NULL DEFAULT 0,      -- user-side estimated queue time
    open_time TEXT NOT NULL DEFAULT '08:00',       -- user-side spot detail
    close_time TEXT NOT NULL DEFAULT '18:00',
    location_x REAL NOT NULL DEFAULT 0,
    location_y REAL NOT NULL DEFAULT 0,
    latitude REAL NOT NULL DEFAULT 0,              -- real-world lat for map rendering
    longitude REAL NOT NULL DEFAULT 0,             -- real-world lng for map rendering
    arrival_rate REAL NOT NULL DEFAULT 0,
    departure_rate REAL NOT NULL DEFAULT 0,
    status INTEGER NOT NULL DEFAULT 1,
    created_at TEXT DEFAULT (datetime('now','localtime')),
    updated_at TEXT DEFAULT (datetime('now','localtime'))
);

CREATE TABLE IF NOT EXISTS parking_lot (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    parking_id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    total_spaces INTEGER NOT NULL,
    occupied_spaces INTEGER NOT NULL DEFAULT 0,
    distance_m INTEGER NOT NULL DEFAULT 0,         -- walking distance from main gate (user side)
    latitude REAL NOT NULL DEFAULT 0,              -- for "navigate" deep links
    longitude REAL NOT NULL DEFAULT 0,
    status INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS shuttle_bus (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    shuttle_id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    capacity INTEGER NOT NULL,
    current_station TEXT,
    current_passengers INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'IDLE'   -- RUNNING / IDLE / MAINTENANCE
);

CREATE TABLE IF NOT EXISTS shuttle_route (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    route_name TEXT NOT NULL DEFAULT '',           -- user side groups rows by route
    from_station TEXT NOT NULL,
    to_station TEXT NOT NULL,
    distance INTEGER NOT NULL,                     -- meters
    waiting_minutes INTEGER NOT NULL DEFAULT 0,    -- average wait per route (user side)
    status INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS idx_route_name ON shuttle_route(route_name);

CREATE TABLE IF NOT EXISTS flow_record (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    record_time TEXT NOT NULL,
    spot_id TEXT NOT NULL,
    current_visitors INTEGER NOT NULL,
    cumulative_visitors INTEGER NOT NULL,
    congestion_level TEXT NOT NULL,
    arrival_rate REAL DEFAULT 0,
    departure_rate REAL DEFAULT 0,
    is_deleted INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_flow_spot_time ON flow_record(spot_id, record_time);

CREATE TABLE IF NOT EXISTS event (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_type TEXT NOT NULL,
    occur_time TEXT NOT NULL,
    spot_id TEXT,
    level TEXT NOT NULL,
    description TEXT,
    suggestion TEXT,
    status TEXT NOT NULL DEFAULT 'ACTIVE',
    updated_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_event_type_time ON event(event_type, occur_time);

CREATE TABLE IF NOT EXISTS scheduling_record (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT NOT NULL,
    spot_id TEXT,
    schedule_type TEXT NOT NULL,
    content TEXT,
    detail TEXT,
    suggestion_status TEXT NOT NULL DEFAULT 'PENDING',  -- PENDING / EXECUTED / REJECTED
    create_time TEXT NOT NULL,
    is_deleted INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_scheduling_time ON scheduling_record(create_time);
CREATE INDEX IF NOT EXISTS idx_scheduling_type ON scheduling_record(schedule_type, suggestion_status);

CREATE TABLE IF NOT EXISTS rule (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    spot_id TEXT NOT NULL DEFAULT '*',
    metric TEXT NOT NULL,
    operator TEXT NOT NULL,
    threshold REAL NOT NULL,
    action TEXT NOT NULL,
    level TEXT NOT NULL DEFAULT 'WARNING',
    priority INTEGER NOT NULL DEFAULT 50,
    scenario TEXT NOT NULL DEFAULT 'ALL',            -- ALL / WEEKDAY / WEEKEND
    applicable_spot_type TEXT NOT NULL DEFAULT 'ALL', -- ALL / ENTRANCE / PLAZA / TRAIL / VIEWPOINT / SUMMIT
    disposal_plan TEXT,                              -- 处置预案操作步骤（JSON 或文本）
    status INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS user (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    role TEXT NOT NULL,                  -- admin / operator / viewer
    created_at TEXT DEFAULT (datetime('now','localtime'))
);

CREATE TABLE IF NOT EXISTS system_config (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS auth_token (
    token TEXT PRIMARY KEY,
    username TEXT NOT NULL,
    role TEXT NOT NULL,
    expire_time TEXT NOT NULL
);

-- User-side POI navigation (toilets / parking lots / stations).
CREATE TABLE IF NOT EXISTS poi (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    poi_id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    category TEXT NOT NULL,               -- TOILET / PARKING / STATION / SERVICE / MEDICAL
    location_x REAL NOT NULL DEFAULT 0,   -- scenic-area map coordinates
    location_y REAL NOT NULL DEFAULT 0,
    latitude REAL NOT NULL DEFAULT 0,     -- for "navigate" deep links
    longitude REAL NOT NULL DEFAULT 0,
    description TEXT,
    status INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS idx_poi_category ON poi(category);

-- Per-spot nearby facilities with walking distance (spot detail page).
CREATE TABLE IF NOT EXISTS spot_poi (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    spot_id TEXT NOT NULL,
    poi_id TEXT NOT NULL,
    distance_m INTEGER NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_spot_poi ON spot_poi(spot_id, poi_id);

-- 预测结果缓存表。旧字段（forecast_date, hour, predicted_visitors, predicted_level）
-- 保留给 UserSideController.refreshForecastIfNeeded() 的全园小时级预测使用；
-- TFT 新链路写入 spot_id 非空 + granularity=5MIN 的行，引擎统一从此表读取。
CREATE TABLE IF NOT EXISTS flow_forecast (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    forecast_date TEXT NOT NULL,          -- YYYY-MM-DD
    hour INTEGER NOT NULL,                -- 0..23（旧链路用）
    predicted_visitors INTEGER NOT NULL,
    predicted_level TEXT NOT NULL,        -- congestion level of the whole park
    spot_id TEXT,                          -- NULL = 全园聚合；非空 = per-spot
    granularity TEXT NOT NULL DEFAULT 'HOURLY',  -- HOURLY / 5MIN
    tick_offset INTEGER,                   -- 5MIN 粒度下在 hour 内的 offset (0..11)
    q10 REAL,
    q50 REAL,
    q90 REAL,
    uncertainty_width REAL,                -- (q90-q10)/q50，高不确定预测应降权
    created_at TEXT DEFAULT (datetime('now','localtime'))
);
-- 注意：不要在这里建 (forecast_date, hour) 的唯一索引。TFT 的 5MIN 行会按
-- (date, hour) 冲突互相覆盖，导致每景点 12 个 tick 只剩 1 行。唯一性改由
-- idx_forecast_tick(forecast_date, hour, tick_offset, spot_id, granularity) 保证，
-- 且老库上的历史唯一索引需由 Schema.cpp 的 migrateLegacySchema() 主动 DROP。
CREATE INDEX IF NOT EXISTS idx_forecast_date_hour ON flow_forecast(forecast_date, hour);
-- 注意：idx_forecast_spot_time(spot_id, ...) 不能写在这里。
-- 老库中 flow_forecast 已存在但没有 spot_id 列，CREATE TABLE IF NOT EXISTS 不会补列，
-- 此处建索引会直接报 "no such column: spot_id"。该索引统一在 Schema.cpp 的
-- migrateLegacySchema() 里 ALTER TABLE 加列之后再创建。

-- 预测日志：记录每次 TFT 推理的结果与后续真实值，用于偏差追踪与效果评估。
CREATE TABLE IF NOT EXISTS prediction_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    spot_id TEXT NOT NULL,
    predict_time TEXT NOT NULL,             -- 预测时间点（被预测的那个 tick 的时间）
    base_time TEXT,                         -- 本次预测所用的最后一条 flow_record 时间
    predicted_value REAL NOT NULL,          -- q50 中位数预测
    q10 REAL,
    q50 REAL,
    q90 REAL,
    actual_value REAL,                      -- 真实值（模拟模式下是 SimulationEngine 输出）
    deviation_rate REAL,                    -- |actual - predicted_value| / max(predicted_value, 1)
    uncertainty_width REAL,                 -- (q90-q10)/max(q50, 1)
    created_at TEXT DEFAULT (datetime('now','localtime'))
);
CREATE INDEX IF NOT EXISTS idx_predlog_spot_time ON prediction_log(spot_id, predict_time);
