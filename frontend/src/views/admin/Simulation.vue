<template>
  <div class="simulation-page">
    <el-alert
      type="warning"
      :closable="false"
      show-icon
      title="演示数据源工具"
      description="本页为联调/演示用途，不在正式菜单中。接入真实闸机数据后请勿在此启动模拟。"
    />

    <el-card shadow="never" class="status-card">
      <template #header>
        <div class="card-header">模拟运行状态</div>
      </template>
      <div class="status-body">
        <el-tag :type="running ? 'success' : 'info'" size="large">
          {{ running ? '运行中' : '已停止' }}
        </el-tag>
        <span class="status-summary">{{ configSummary }}</span>
      </div>
    </el-card>

    <el-card shadow="never" class="config-card">
      <template #header>
        <div class="card-header">模拟参数配置</div>
      </template>

      <el-alert
        v-if="running"
        type="warning"
        :closable="false"
        show-icon
        title="模拟运行中，部分参数需重新启动后生效"
        style="margin-bottom: 16px"
      />

      <el-form :model="form" label-width="120px">
        <el-form-item label="到达率">
          <el-input-number v-model="form.arrivalRate" :min="1" :max="500" />
          <span class="unit">人/分钟</span>
        </el-form-item>
        <el-form-item label="波动模式">
          <el-select v-model="form.pattern" style="width: 200px">
            <el-option
              v-for="k in patternOptions"
              :key="k"
              :label="SIMULATION_PATTERN_LABELS[k]"
              :value="k"
            />
          </el-select>
        </el-form-item>
        <el-form-item label="运行时长">
          <el-input-number v-model="form.durationMinutes" :min="1" :max="1440" />
          <span class="unit">分钟</span>
        </el-form-item>
        <el-form-item>
          <el-button type="primary" :loading="saving" @click="handleSave">保存配置</el-button>
          <el-button
            v-if="running"
            type="danger"
            :loading="operating"
            @click="handleStop"
          >
            停止模拟
          </el-button>
          <el-button v-else type="success" :loading="operating" @click="handleStart">
            启动模拟
          </el-button>
        </el-form-item>
      </el-form>
    </el-card>
  </div>
</template>

<script setup lang="ts">
import { ref, computed } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import {
  getSimulationState,
  startSimulation,
  stopSimulation,
  updateSimulationConfig,
} from '@/api'
import type { SimulationConfig, SimulationPattern } from '@/api'
import { SIMULATION_PATTERN_LABELS } from '@/utils/status'
import { usePolling } from '@/composables/usePolling'

const running = ref(false)
const initialized = ref(false)
const saving = ref(false)
const operating = ref(false)

const form = ref<SimulationConfig>({ arrivalRate: 45, pattern: 'PEAK', durationMinutes: 120 })

const patternOptions = Object.keys(SIMULATION_PATTERN_LABELS) as SimulationPattern[]

const configSummary = computed(
  () =>
    `到达率 ${form.value.arrivalRate} 人/分钟 · 模式 ${
      SIMULATION_PATTERN_LABELS[form.value.pattern]
    } · 时长 ${form.value.durationMinutes} 分钟`,
)

async function fetch() {
  try {
    const res = await getSimulationState()
    running.value = res.data.running
    if (!initialized.value) {
      form.value = { ...res.data.config }
      initialized.value = true
    }
  } catch {
    // silent in polling
  }
}

usePolling(fetch, 5000)

async function handleSave() {
  saving.value = true
  try {
    const res = await updateSimulationConfig({ ...form.value })
    if (res.code !== 200) {
      ElMessage.error(res.message)
      return
    }
    ElMessage.success('配置已保存')
    if (running.value) {
      ElMessage.info('部分参数需重新启动后生效')
    }
    fetch()
  } catch {
    // silent
  } finally {
    saving.value = false
  }
}

async function handleStart() {
  operating.value = true
  try {
    const res = await startSimulation()
    if (res.code !== 200) {
      ElMessage.error(res.message)
      return
    }
    ElMessage.success('模拟已启动')
    await fetch()
  } catch {
    // silent
  } finally {
    operating.value = false
  }
}

async function handleStop() {
  try {
    await ElMessageBox.confirm('停止后模拟数据将不再增长，确认停止？', '提示', {
      type: 'warning',
      confirmButtonText: '确认停止',
      cancelButtonText: '取消',
    })
  } catch {
    return
  }
  operating.value = true
  try {
    const res = await stopSimulation()
    if (res.code !== 200) {
      ElMessage.error(res.message)
      return
    }
    ElMessage.success('模拟已停止')
    await fetch()
  } catch {
    // silent
  } finally {
    operating.value = false
  }
}
</script>

<style scoped>
.simulation-page {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.status-card,
.config-card {
  border-radius: 6px;
}

.card-header {
  font-size: 15px;
  font-weight: 600;
  color: #303133;
}

.status-body {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
}

.status-summary {
  font-size: 14px;
  color: #606266;
}

.unit {
  margin-left: 8px;
  font-size: 13px;
  color: #909399;
}
</style>
