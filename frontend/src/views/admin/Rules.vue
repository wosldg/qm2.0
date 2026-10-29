<template>
  <div class="rules-page">
    <!-- 三级预警线说明 -->
    <el-card shadow="never" class="guide-card">
      <div class="guide-row">
        <span class="guide-title">三级预警线</span>
        <el-tag type="warning" size="small">黄色 · 警告</el-tag>
        <span class="guide-text">拥挤度达阈值 → 广播提示 + 人员前置引导</span>
        <el-tag type="danger" size="small" effect="plain">橙色 · 严重</el-tag>
        <span class="guide-text">启动分流 + 摆渡车调配</span>
        <el-tag type="danger" size="small">红色 · 紧急</el-tag>
        <span class="guide-text">入口限流 + 暂停入园建议</span>
      </div>
    </el-card>

    <!-- 拥挤度预警线 -->
    <el-card shadow="never" class="table-card">
      <template #header>
        <div class="card-header">
          <span>拥挤度预警线</span>
          <el-button v-if="authStore.isAdmin" type="primary" size="small" @click="openCreate">
            新增预警线
          </el-button>
        </div>
      </template>
      <el-table :data="congestionRules" stripe>
        <el-table-column label="适用景点" min-width="140">
          <template #default="{ row }">{{ spotLabel(row.spotId) }}</template>
        </el-table-column>
        <el-table-column label="预警等级" width="110">
          <template #default="{ row }">
            <el-tag :type="EVENT_LEVEL_TAG[row.level]" size="small">
              {{ warningLevelLabel(row.level) }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="触发条件" width="150">
          <template #default="{ row }">拥挤度 ≥ {{ Math.round(row.threshold * 100) }}%</template>
        </el-table-column>
        <el-table-column label="适用场景" width="110">
          <template #default="{ row }">{{ SCENARIO_LABELS[row.scenario || 'ALL'] }}</template>
        </el-table-column>
        <el-table-column label="处置预案" min-width="200" show-overflow-tooltip>
          <template #default="{ row }">{{ disposalSummary(row.disposalPlan) }}</template>
        </el-table-column>
        <el-table-column label="启用" width="90">
          <template #default="{ row }">
            <el-switch
              :model-value="row.enabled"
              :disabled="!authStore.isAdmin"
              @change="(val) => toggleEnabled(row, val)"
            />
          </template>
        </el-table-column>
        <el-table-column v-if="authStore.isAdmin" label="操作" width="130">
          <template #default="{ row }">
            <el-button link type="primary" size="small" @click="openEdit(row)">编辑</el-button>
            <el-button link type="danger" size="small" @click="handleDelete(row)">删除</el-button>
          </template>
        </el-table-column>
      </el-table>
    </el-card>

    <!-- 资源类规则（只读展示 + 启停） -->
    <el-collapse v-if="resourceRules.length">
      <el-collapse-item title="资源类规则（停车 / 摆渡车 / 到达速率）" name="resource">
        <el-table :data="resourceRules" stripe>
          <el-table-column label="适用景点" min-width="130">
            <template #default="{ row }">{{ spotLabel(row.spotId) }}</template>
          </el-table-column>
          <el-table-column label="指标" min-width="150">
            <template #default="{ row }">{{ RULE_METRIC_LABELS[row.metric] }}</template>
          </el-table-column>
          <el-table-column label="条件" width="130">
            <template #default="{ row }">{{ row.operator }} {{ row.threshold }}</template>
          </el-table-column>
          <el-table-column label="等级" width="100">
            <template #default="{ row }">
              <el-tag :type="EVENT_LEVEL_TAG[row.level]" size="small">
                {{ EVENT_LEVEL_LABELS[row.level] }}
              </el-tag>
            </template>
          </el-table-column>
          <el-table-column prop="action" label="动作" min-width="180" show-overflow-tooltip />
          <el-table-column label="启用" width="90">
            <template #default="{ row }">
              <el-switch
                :model-value="row.enabled"
                :disabled="!authStore.isAdmin"
                @change="(val) => toggleEnabled(row, val)"
              />
            </template>
          </el-table-column>
        </el-table>
      </el-collapse-item>
    </el-collapse>

    <!-- 新增 / 编辑预警线 -->
    <el-dialog
      v-model="dialogVisible"
      :title="editId === null ? '新增预警线' : '编辑预警线'"
      width="520px"
    >
      <el-form :model="form" label-width="110px">
        <el-form-item label="适用景点">
          <el-select v-model="form.spotId" style="width: 100%">
            <el-option label="全部景点" value="*" />
            <el-option v-for="s in spots" :key="s.spotId" :label="s.name" :value="s.spotId" />
          </el-select>
        </el-form-item>
        <el-form-item label="预警等级">
          <el-select v-model="form.level" style="width: 100%">
            <el-option label="黄色（警告）" value="WARNING" />
            <el-option label="橙色（严重）" value="HIGH" />
            <el-option label="红色（紧急）" value="CRITICAL" />
          </el-select>
        </el-form-item>
        <el-form-item label="触发阈值">
          <el-slider v-model="form.thresholdPct" :min="50" :max="100" show-input />
          <div class="hint">拥挤度（在园人数 / 瞬时容量）达到该百分比时触发</div>
        </el-form-item>
        <el-form-item label="适用场景">
          <el-select v-model="form.scenario" style="width: 100%">
            <el-option
              v-for="(label, key) in SCENARIO_LABELS"
              :key="key"
              :label="label"
              :value="key"
            />
          </el-select>
        </el-form-item>
        <el-form-item label="处置预案">
          <el-input
            v-model="form.disposalText"
            type="textarea"
            :rows="4"
            placeholder="每行一条处置步骤，触发预警后将在预警详情中展示"
          />
        </el-form-item>
        <el-form-item label="启用">
          <el-switch v-model="form.enabled" />
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dialogVisible = false">取消</el-button>
        <el-button type="primary" :loading="submitting" @click="handleSubmit">确定</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, computed } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { getRules, getSpots, createRule, updateRule, deleteRule } from '@/api'
import type { Rule, RuleScenario, EventLevel, Spot } from '@/api'
import { RULE_METRIC_LABELS, EVENT_LEVEL_LABELS, EVENT_LEVEL_TAG } from '@/utils/status'
import { useAuthStore } from '@/stores/auth'
import { usePolling } from '@/composables/usePolling'

const authStore = useAuthStore()

const rules = ref<Rule[]>([])
const spots = ref<Spot[]>([])

const SCENARIO_LABELS: Record<RuleScenario, string> = {
  ALL: '全天',
  WEEKDAY: '仅工作日',
  WEEKEND: '仅周末',
}

/** 预警等级 → 预警线名称（黄/橙/红） */
const WARNING_LEVEL_NAMES: Partial<Record<EventLevel, string>> = {
  WARNING: '黄色预警',
  HIGH: '橙色预警',
  CRITICAL: '红色预警',
}

/** 等级 → 默认优先级（数值越小优先级越高，红 > 橙 > 黄） */
const LEVEL_PRIORITY: Record<string, number> = { CRITICAL: 10, HIGH: 20, WARNING: 30 }

function warningLevelLabel(level: EventLevel): string {
  return WARNING_LEVEL_NAMES[level] || EVENT_LEVEL_LABELS[level]
}

/** 本页只维护拥挤度（visitor_ratio）预警线；其余指标规则只读展示 */
const congestionRules = computed(() => rules.value.filter((r) => r.metric === 'visitor_ratio'))
const resourceRules = computed(() => rules.value.filter((r) => r.metric !== 'visitor_ratio'))

const spotNameMap = computed(() => {
  const map: Record<string, string> = {}
  for (const s of spots.value) map[s.spotId] = s.name
  return map
})

function spotLabel(spotId: string): string {
  if (spotId === '*') return '全部景点'
  return spotNameMap.value[spotId] || spotId
}

function parsePlan(plan?: string): string[] {
  if (!plan) return []
  try {
    const parsed = JSON.parse(plan)
    if (Array.isArray(parsed)) return parsed.map(String).filter(Boolean)
  } catch {
    // 非 JSON，按纯文本处理
  }
  return [plan]
}

function disposalSummary(plan?: string): string {
  const steps = parsePlan(plan)
  if (!steps.length) return '—'
  return steps.length > 1 ? `${steps[0]}（共 ${steps.length} 步）` : steps[0]
}

async function fetch() {
  try {
    const [ruleRes, spotRes] = await Promise.all([getRules(), getSpots()])
    rules.value = ruleRes.data
    spots.value = spotRes.data
  } catch {
    // silent in polling
  }
}

usePolling(fetch, 10000)

// ==================== 新增 / 编辑 ====================

const dialogVisible = ref(false)
const submitting = ref(false)
const editId = ref<number | null>(null)

interface ThresholdForm {
  spotId: string
  level: EventLevel
  thresholdPct: number
  scenario: RuleScenario
  disposalText: string
  enabled: boolean
}

const emptyForm = (): ThresholdForm => ({
  spotId: '*',
  level: 'WARNING',
  thresholdPct: 85,
  scenario: 'ALL',
  disposalText: '',
  enabled: true,
})

const form = ref<ThresholdForm>(emptyForm())

function openCreate() {
  editId.value = null
  form.value = emptyForm()
  dialogVisible.value = true
}

function openEdit(row: Rule) {
  editId.value = row.ruleId
  form.value = {
    spotId: row.spotId,
    level: row.level,
    thresholdPct: Math.round(row.threshold * 100),
    scenario: row.scenario || 'ALL',
    disposalText: parsePlan(row.disposalPlan).join('\n'),
    enabled: row.enabled,
  }
  dialogVisible.value = true
}

async function handleSubmit() {
  const f = form.value
  const steps = f.disposalText
    .split('\n')
    .map((s) => s.trim())
    .filter(Boolean)
  const levelName = WARNING_LEVEL_NAMES[f.level] || '预警'
  const payload = {
    spotId: f.spotId,
    metric: 'visitor_ratio' as const,
    operator: '>=' as const,
    threshold: f.thresholdPct / 100,
    action: `触发${levelName}：拥挤度达到 ${f.thresholdPct}%`,
    level: f.level,
    priority: LEVEL_PRIORITY[f.level] ?? 30,
    enabled: f.enabled,
    scenario: f.scenario,
    disposalPlan: steps.length ? JSON.stringify(steps) : '',
  }
  submitting.value = true
  try {
    const res =
      editId.value === null ? await createRule(payload) : await updateRule(editId.value, payload)
    if (res.code !== 200) {
      ElMessage.error(res.message)
      return
    }
    ElMessage.success(editId.value === null ? '新增成功' : '修改成功')
    dialogVisible.value = false
    fetch()
  } catch {
    // silent
  } finally {
    submitting.value = false
  }
}

async function toggleEnabled(row: Rule, val: string | number | boolean) {
  try {
    const res = await updateRule(row.ruleId, { enabled: Boolean(val) })
    if (res.code !== 200) {
      ElMessage.error(res.message)
      return
    }
    ElMessage.success('已更新')
    fetch()
  } catch {
    // silent
  }
}

async function handleDelete(row: Rule) {
  try {
    await ElMessageBox.confirm(
      `确认删除「${spotLabel(row.spotId)}」的${warningLevelLabel(row.level)}线？`,
      '提示',
      { type: 'warning', confirmButtonText: '确认删除', cancelButtonText: '取消' },
    )
  } catch {
    return
  }
  try {
    const res = await deleteRule(row.ruleId)
    if (res.code !== 200) {
      ElMessage.error(res.message)
      return
    }
    ElMessage.success('删除成功')
    fetch()
  } catch {
    // silent
  }
}
</script>

<style scoped>
.rules-page {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.guide-card {
  border-radius: 6px;
}

.guide-row {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}

.guide-title {
  font-size: 14px;
  font-weight: 600;
  color: #303133;
}

.guide-text {
  font-size: 13px;
  color: #606266;
  margin-right: 8px;
}

.table-card {
  border-radius: 6px;
}

.card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 15px;
  font-weight: 600;
  color: #303133;
}

.hint {
  font-size: 12px;
  color: #909399;
  line-height: 1.6;
}
</style>
