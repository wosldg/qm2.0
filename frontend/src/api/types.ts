export interface ApiResult<T = any> {
  code: number
  message: string
  data: T
}

export type CongestionLevel = 'EMPTY' | 'NORMAL' | 'MODERATE' | 'CROWDED' | 'OVERLOADED'

export interface Spot {
  spotId: string
  name: string
  locationX: number
  locationY: number
  capacity: number
  dailyCapacity: number
  currentVisitors: number
  totalIn: number
  arrivalRate: number
  departureRate: number
  level: CongestionLevel
}

export interface ParkingLot {
  parkingId: string
  name: string
  totalSpaces: number
  occupiedSpaces: number
  freeSpaces: number
}

export interface Shuttle {
  shuttleId: string
  name: string
  capacity: number
  currentStation: string
  passengerCount: number
  status: 'RUNNING' | 'IDLE' | 'MAINTENANCE'
}

export interface SystemStatus {
  totalCapacity: number
  currentVisitors: number
  crowdedSpotCount: number
  freeSpaces: number
  shuttleOnline: number
  shuttleTotal: number
}

export interface LoginRequest {
  username: string
  password: string
}

export interface LoginResponse {
  token: string
  role: 'admin' | 'operator' | 'viewer'
  username: string
}

export interface FlowPoint {
  timestamp: number
  visitors: number
}

// ==================== 事件 / 告警 ====================

export type EventType =
  | 'CONGESTION'
  | 'OVERLOAD'
  | 'PARKING_SHORTAGE'
  | 'CAPACITY_SHORTAGE'
  | 'DATA_ANOMALY'
  | 'SYSTEM_ERROR'

export type EventLevel = 'INFO' | 'WARNING' | 'HIGH' | 'CRITICAL'

export type EventStatus = 'ACTIVE' | 'ACKNOWLEDGED' | 'RESOLVED' | 'CLOSED' | 'IGNORED'

export interface EventItem {
  eventId: number
  type: EventType
  level: EventLevel
  spotId?: string
  spotName?: string
  description: string
  status: EventStatus
  suggestion?: string
  createdAt: string
  updatedAt?: string
}

// ==================== 调度建议 ====================

export type SuggestionType =
  | 'DIVERSION'
  | 'SHUTTLE_DISPATCH'
  | 'PARKING_GUIDE'
  | 'ENTRANCE_LIMIT'
  | 'RESOURCE_WARNING'

export type SuggestionStatus = 'PENDING' | 'EXECUTED' | 'REJECTED'

export interface Suggestion {
  suggestionId: number
  type: SuggestionType
  content: string
  detail?: string
  spotId?: string
  status: SuggestionStatus
  createdAt: string
}

// ==================== 规则配置 ====================

export type RuleMetric = 'visitor_ratio' | 'parking_free' | 'shuttle_load' | 'arrival_rate'

export type RuleOperator = '>' | '>=' | '<' | '<='

export type RuleScenario = 'ALL' | 'WEEKDAY' | 'WEEKEND'

export interface Rule {
  ruleId: number
  spotId: string // '*' 表示适用于全部景点
  metric: RuleMetric
  operator: RuleOperator
  threshold: number
  action: string
  level: EventLevel
  priority: number
  enabled: boolean
  /** 适用场景：全天 / 仅工作日 / 仅周末 */
  scenario?: RuleScenario
  /** 适用景点类型（ALL 表示不限） */
  applicableSpotType?: string
  /** 处置预案（JSON 数组字符串，如 '["步骤一","步骤二"]'） */
  disposalPlan?: string
}

// ==================== 用户管理 ====================

export interface User {
  userId: number
  username: string
  role: 'admin' | 'operator' | 'viewer'
  createdAt: string
}

export interface UserCreateRequest {
  username: string
  password: string
  role: 'admin' | 'operator' | 'viewer'
}

// ==================== 数据统计 ====================

export interface FlowStatPoint {
  date: string
  visitors: number
}

export interface CongestionStat {
  spotName: string
  crowdedMinutes: number
}

export interface ResourceStatPoint {
  time: string
  utilization: number
}

export interface StatisticsData {
  flow: FlowStatPoint[]
  congestion: CongestionStat[]
  resource: ResourceStatPoint[]
  suggestionTypeCount: { type: SuggestionType; count: number }[]
}

// ==================== 模拟配置 ====================

export type SimulationPattern = 'STEADY' | 'PEAK' | 'RANDOM'

export interface SimulationConfig {
  arrivalRate: number
  pattern: SimulationPattern
  durationMinutes: number
}

export interface SimulationState {
  running: boolean
  config: SimulationConfig
}

// ==================== 系统监控 ====================

export interface PipelineStatus {
  name: string
  running: boolean
  queueDepth: number
}

export interface MonitorLog {
  time: string
  level: 'INFO' | 'WARN' | 'ERROR'
  message: string
}

export interface SystemMonitor {
  pipelines: PipelineStatus[]
  sources: { name: string; status: string }[]
  logs: MonitorLog[]
}

// ==================== 系统设置 ====================

export interface SystemSettings {
  totalCapacity: number
  debounceSeconds: number
  retentionDays: number
  batchIntervalMs: number
}

// ==================== 数据录入 ====================

export interface VisitorEntryRequest {
  spotId: string
  currentVisitors: number
}

export interface ParkingEntryRequest {
  parkingId: string
  occupiedSpaces: number
}

// ==================== 用户端（/api/user/*） ====================

/** 用户端景点简报：只给结论，不给运营数字 */
export interface UserSpotBrief {
  spotId: string
  name: string
  level: CongestionLevel
  levelText: string
  queueMinutes: number
  openTime: string
  closeTime: string
  locationX?: number
  locationY?: number
  latitude?: number
  longitude?: number
  currentVisitors?: number
}

export interface OverviewConclusion {
  level: CongestionLevel
  text: string
  currentVisitors: number
  totalCapacity: number
  ratio: number
  crowdedSpotCount: number
}

export interface PeakAdvice {
  isPeak: boolean
  peakStart?: number
  peakEnd?: number
  message: string
}

export interface OverviewData {
  conclusion: OverviewConclusion
  peakAdvice: PeakAdvice
  spots: UserSpotBrief[]
  updatedAt: string
}

export interface RankingsData {
  type: 'avoid' | 'crowded'
  list: UserSpotBrief[]
}

export interface ForecastPoint {
  hour: number
  predictedVisitors: number
  level: CongestionLevel
  levelText: string
}

export interface ForecastData {
  date: string
  points: ForecastPoint[]
  peakStart: number
  peakEnd: number
}

/** 地图标记点（用户端底图 / 路线页共用，坐标直接用于高德地图） */
export interface MapMarker {
  id: string
  name: string
  type: 'gate' | 'parking' | 'spot'
  longitude: number
  latitude: number
  visitors?: number
  level?: CongestionLevel
  levelText?: string
  freeSpaces?: number
}

export interface UserParking {
  parkingId: string
  name: string
  freeSpaces: number
  distanceM: number
  latitude: number
  longitude: number
  statusText: string
}

export interface ShuttleRouteGroup {
  routeName: string
  stations: string[]
  waitingMinutes: number
  totalDistanceM: number
}

export type PoiCategory = 'TOILET' | 'PARKING' | 'STATION' | 'SERVICE' | 'MEDICAL' | 'GATE'

export interface Poi {
  poiId: string
  name: string
  category: PoiCategory
  categoryText: string
  locationX: number
  locationY: number
  latitude: number
  longitude: number
  description?: string
}

export interface RouteStep extends UserSpotBrief {
  order: number
}

export interface RecommendData {
  type: 'avoid' | 'classic'
  name: string
  steps: RouteStep[]
  totalDistanceM: number
  estimatedMinutes: number
  message?: string
}

export interface SpotTrendPoint {
  hour: number
  visitors: number
}

export interface SpotDetailPoi {
  poiId: string
  name: string
  category: PoiCategory
  categoryText: string
  distanceM: number
  latitude: number
  longitude: number
}

export interface SpotDetailData extends UserSpotBrief {
  currentVisitors: number
  capacity: number
  trend: SpotTrendPoint[]
  pois: SpotDetailPoi[]
}

/** 底图点位选中事件（PoiMap 组件 → 页面） */
export interface PoiMapSelect {
  type: 'spot' | 'poi'
  id: string
  name: string
  latitude?: number
  longitude?: number
  distanceM?: number
}

