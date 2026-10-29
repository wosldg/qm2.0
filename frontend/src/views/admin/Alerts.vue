<template>
  <div class="alerts-page">
    <!-- 等级统计 -->
    <div class="stat-row">
      <div class="stat-col">
        <StatCard :title="EVENT_LEVEL_LABELS.CRITICAL" :value="levelCounts.CRITICAL" />
      </div>
      <div class="stat-col">
        <StatCard :title="EVENT_LEVEL_LABELS.HIGH" :value="levelCounts.HIGH" />
      </div>
      <div class="stat-col">
        <StatCard :title="EVENT_LEVEL_LABELS.WARNING" :value="levelCounts.WARNING" />
      </div>
      <div class="stat-col">
        <StatCard :title="EVENT_LEVEL_LABELS.INFO" :value="levelCounts.INFO" />
      </div>
    </div>

    <!-- 筛选 -->
    <div class="toolbar">
      <el-radio-group v-model="activeTab">
        <el-radio-button value="pending">待处理</el-radio-button>
        <el-radio-button value="all">全部事件</el-radio-button>
      </el-radio-group>
      <el-select v-model="filterLevel" placeholder="等级筛选" style="width: 140px">
        <el-option label="全部" value="" />
        <el-option
          v-for="level in LEVEL_OPTIONS"
          :key="level"
          :label="EVENT_LEVEL_LABELS[level]"
          :value="level"
        />
      </el-select>
      <el-select v-model="filterType" placeholder="类型筛选" style="width: 150px">
        <el-option label="全部" value="" />
        <el-option
          v-for="type in TYPE_OPTIONS"
          :key="type"
          :label="EVENT_TYPE_LABELS[type]"
          :value="type"
        />
      </el-select>
      <el-select
        v-if="activeTab === 'all'"
        v-model="filterStatus"
        placeholder="状态筛选"
        style="width: 140px"
      >
        <el-option label="全部" value="" />
        <el-option
          v-for="status in STATUS_OPTIONS"
          :key="status"
          :label="EVENT_STATUS_LABELS[status]"
          :value="status"
        />
      </el-select>
    </div>

    <!-- 预警表格 -->
    <el-card shadow="never" class="table-card">
      <template #header>
        <div class="card-header">
          {{ activeTab === 'pending' ? '待处理预警' : '事件列表' }}
        </div>
      </template>
      <el-table :data="pagedEvents" :row-class-name="rowClassName" style="width: 100%">
        <el-table-column prop="createdAt" label="时间" width="165" />
        <el-table-column prop="spotName" label="景点" width="120">
          <template #default="{ row }">{{ row.spotName || '—' }}</template>
        </el-table-column>
        <el-table-column label="类型" width="140">
          <template #default="{ row }">
            <el-tag size="small">{{ typeLabel(row.type) }}</el-tag>
            <el-tag v-if="isPredicted(row.type)" size="small" type="warning" class="pred-tag">
              预测
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="等级" width="90">
          <template #default="{ row }">
            <el-tag size="small" :type="EVENT_LEVEL_TAG[row.level]">
              {{ EVENT_LEVEL_LABELS[row.level] }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column prop="description" label="描述" min-width="230" show-overflow-tooltip />
        <el-table-column label="状态" width="95">
          <template #default="{ row }">
            <el-tag size="small" :type="EVENT_STATUS_TAG[row.status]">
              {{ EVENT_STATUS_LABELS[row.status] }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="操作" width="210" fixed="right">
          <template #default="{ row }">
            <el-button size="small" text type="primary" @click="openDetail(row)">详情</el-button>
            <template v-if="authStore.isAdmin">
              <el-button
                v-if="row.status === 'ACTIVE'"
                size="small"
                text
                type="primary"
                @click="changeStatus(row, 'ACKNOWLEDGED')"
              >
                确认
              </el-button>
              <el-button
                v-if="row.status === 'ACTIVE' || row.status === 'ACKNOWLEDGED'"
                size="small"
                text
                type="success"
                @click="handleWithConfirm(row, 'RESOLVED', '确认恢复该事件？')"
              >
                恢复
              </el-button>
              <el-button
                v-if="row.status === 'ACTIVE' || row.status === 'ACKNOWLEDGED'"
                size="small"
                text
                type="danger"
                @click="handleWithConfirm(row, 'CLOSED', '确认关闭该事件？')"
              >
                关闭
              </el-button>
            </template>
          </template>
        </el-table-column>
      </el-table>

      <div class="pagination-wrap">
        <el-pagination
          v-model:current-page="page"
          :page-size="PAGE_SIZE"
          :total="filteredEvents.length"
          layout="prev, pager, next, total"
          background
        />
      </div>
    </el-card>

    <!-- 详情抽屉 -->
    <el-drawer v-model="drawerVisible" title="预警详情" size="480px">
      <div v-if="detail" class="detail-list">
        <el-alert
          v-if="isPredicted(detail.type) && detail.level === 'INFO'"
          type="info"
          :closable="false"
          show-icon
          title="高不确定度预测，暂不自动生成调度建议，请人工确认"
          class="detail-alert"
        />
        <el-alert
          v-else-if="isPredicted(detail.type)"
          type="warning"
          :closable="false"
          show-icon
          title="TFT 预测提前预警（未来 60 分钟内可能触发）"
          class="detail-alert"
        />
        <div class="detail-row">
          <span class="detail-label">类型</span>
          <span class="detail-value">
            {{ typeLabel(detail.type) }}
            <el-tag v-if="isPredicted(detail.type)" size="small" type="warning">预测</el-tag>
          </span>
        </div>
        <div class="detail-row">
          <span class="detail-label">等级</span>
          <span class="detail-value">
            <el-tag size="small" :type="EVENT_LEVEL_TAG[detail.level]">
              {{ EVENT_LEVEL_LABELS[detail.level] }}
            </el-tag>
          </span>
        </div>
        <div class="detail-row">
          <span class="detail-label">状态</span>
          <span class="detail-value">
            <el-tag size="small" :type="EVENT_STATUS_TAG[detail.status]">
              {{ EVENT_STATUS_LABELS[detail.status] }}
            </el-tag>
          </span>
        </div>
        <div class="detail-row">
          <span class="detail-label">景点</span>
          <span class="detail-value">{{ detail.spotName || '—' }}</span>
        </div>
        <div class="detail-row">
          <span class="detail-label">描述</span>
          <span class="detail-value">{{ cleanDescription(detail.description) }}</span>
        </div>
        <div v-if="disposalSteps(detail).length" class="detail-row">
          <span class="detail-label">处置预案</span>
          <div class="detail-value">
            <div
              v-for="(step, i) in disposalSteps(detail)"
              :key="i"
              class="disposal-step"
            >
              {{ i + 1 }}. {{ step }}
            </div>
          </div>
        </div>
        <div v-else-if="detail.suggestion" class="detail-row">
          <span class="detail-label">调度建议</span>
          <span class="detail-value">{{ detail.suggestion }}</span>
        </div>
        <div class="detail-row">
          <span class="detail-label">生成时间</span>
          <span class="detail-value">{{ detail.createdAt }}</span>
        </div>
        <div class="detail-row">
          <span class="detail-label">更新时间</span>
          <span class="detail-value">{{ detail.updatedAt || '—' }}</span>
        </div>
      </div>
    </el-drawer>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, watch } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import StatCard from '@/components/StatCard.vue'
import { useAuthStore } from '@/stores/auth'
import { usePolling } from '@/composables/usePolling'
import { getEvents, updateEventStatus } from '@/api'
import type { EventItem, EventLevel, EventType, EventStatus } from '@/api'
import {
  EVENT_LEVEL_LABELS,
  EVENT_LEVEL_TAG,
  EVENT_TYPE_LABELS,
  EVENT_STATUS_LABELS,
  EVENT_STATUS_TAG,
} from '@/utils/status'

const authStore = useAuthStore()

const TYPE_OPTIONS: EventType[] = [
  'CONGESTION',
  'OVERLOAD',
  'PARKING_SHORTAGE',
  'CAPACITY_SHORTAGE',
  'DATA_ANOMALY',
  'SYSTEM_ERROR',
]
const LEVEL_OPTIONS: EventLevel[] = ['CRITICAL', 'HIGH', 'WARNING', 'INFO']
const STATUS_OPTIONS: EventStatus[] = ['ACTIVE', 'ACKNOWLEDGED', 'RESOLVED', 'CLOSED', 'IGNORED']
/** 待处理口径：未闭环的状态 */
const PENDING_STATUSES: string[] = ['ACTIVE', 'ACKNOWLEDGED']

const PAGE_SIZE = 10

const events = ref<EventItem[]>([])
const activeTab = ref<'pending' | 'all'>('pending')
const filterLevel = ref('')
const filterType = ref('')
const filterStatus = ref('')
const page = ref(1)
const drawerVisible = ref(false)
const detail = ref<EventItem | null>(null)

async function fetch() {
  try {
    const res = await getEvents()
    events.value = res.data
  } catch (e) {}
}

usePolling(fetch, 3000)

/** 类型标签：PREDICTED_ 前缀的预测预警取基础类型名，另加「预测」标签 */
function isPredicted(type: string): boolean {
  return type.startsWith('PREDICTED_')
}

function typeLabel(type: string): string {
  const base = isPredicted(type) ? type.slice('PREDICTED_'.length) : type
  return EVENT_TYPE_LABELS[base as EventType] || type
}

/** 描述里可能拼接了「｜处置预案：...」，详情页拆出来单独展示 */
function cleanDescription(description: string): string {
  const idx = description.indexOf('｜处置预案：')
  return idx >= 0 ? description.slice(0, idx) : description
}

/** 处置预案：优先从描述里的「｜处置预案：」解析 JSON 数组；其次尝试 suggestion */
function disposalSteps(row: EventItem): string[] {
  const sources: string[] = []
  const idx = row.description.indexOf('｜处置预案：')
  if (idx >= 0) sources.push(row.description.slice(idx + '｜处置预案：'.length))
  if (row.suggestion) sources.push(row.suggestion)
  for (const src of sources) {
    try {
      const parsed = JSON.parse(src)
      if (Array.isArray(parsed) && parsed.length) return parsed.map(String)
    } catch {
      // 非 JSON，继续尝试下一个来源
    }
  }
  return []
}

const filteredEvents = computed(() =>
  events.value.filter(
    (e) =>
      (activeTab.value === 'all' || PENDING_STATUSES.includes(e.status)) &&
      (!filterLevel.value || e.level === filterLevel.value) &&
      (!filterType.value || e.type === filterType.value) &&
      (!filterStatus.value || e.status === filterStatus.value),
  ),
)

const pagedEvents = computed(() =>
  filteredEvents.value.slice((page.value - 1) * PAGE_SIZE, page.value * PAGE_SIZE),
)

watch([activeTab, filterType, filterLevel, filterStatus], () => {
  page.value = 1
})

/** 待处理口径下的等级统计 */
const levelCounts = computed(() => {
  const counts: Record<EventLevel, number> = { CRITICAL: 0, HIGH: 0, WARNING: 0, INFO: 0 }
  for (const e of events.value) {
    if (PENDING_STATUSES.includes(e.status)) counts[e.level]++
  }
  return counts
})

/** 紧急告警整行淡红底 */
function rowClassName({ row }: { row: EventItem }) {
  return row.level === 'CRITICAL' ? 'critical-row' : ''
}

function openDetail(row: EventItem) {
  detail.value = row
  drawerVisible.value = true
}

async function changeStatus(row: EventItem, status: EventStatus) {
  try {
    const res = await updateEventStatus(row.eventId, status)
    if (res.code !== 200) {
      ElMessage.error(res.message)
      return
    }
    ElMessage.success('操作成功')
    await fetch()
  } catch (e) {}
}

async function handleWithConfirm(row: EventItem, status: EventStatus, text: string) {
  try {
    await ElMessageBox.confirm(text, '提示', { type: 'warning' })
  } catch (e) {
    return
  }
  await changeStatus(row, status)
}
</script>

<style scoped>
.alerts-page {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.stat-row {
  display: flex;
  gap: 16px;
  flex-wrap: wrap;
}

.stat-col {
  flex: 1 1 calc(25% - 12px);
  min-width: 180px;
}

.toolbar {
  display: flex;
  gap: 12px;
  flex-wrap: wrap;
  align-items: center;
}

.table-card {
  border-radius: 6px;
}

.card-header {
  font-size: 15px;
  font-weight: 600;
  color: #303133;
}

.pagination-wrap {
  display: flex;
  justify-content: flex-end;
  margin-top: 16px;
}

.pred-tag {
  margin-left: 6px;
}

.detail-list {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.detail-alert {
  margin-bottom: 4px;
}

.detail-row {
  display: flex;
  align-items: flex-start;
  gap: 12px;
  font-size: 13px;
}

.detail-label {
  width: 72px;
  flex-shrink: 0;
  color: #909399;
}

.detail-value {
  flex: 1;
  color: #303133;
  word-break: break-all;
}

.disposal-step {
  padding: 4px 0;
  border-bottom: 1px dashed #ebeef5;
}

.disposal-step:last-child {
  border-bottom: none;
}

:deep(.critical-row) {
  background: #fef0f0 !important;
}

@media (max-width: 768px) {
  .stat-col {
    flex: 1 1 100%;
  }
}
</style>
