<template>
  <div class="shuttle-manage-page">
    <!-- 只读提示 -->
    <el-alert
      v-if="!authStore.isAdmin"
      type="info"
      :closable="false"
      title="当前为只读视角，如需修改请联系管理员"
    />

    <el-card shadow="never" class="table-card">
      <el-tabs v-model="activeTab">
        <el-tab-pane label="车辆管理" name="vehicles">
          <div class="card-header">
            <span>摆渡车列表</span>
            <el-button v-if="authStore.isAdmin" type="primary" size="small" @click="openAdd">新增摆渡车</el-button>
          </div>

          <el-table :data="shuttles" stripe>
            <el-table-column prop="shuttleId" label="车辆ID" width="110" />
            <el-table-column prop="name" label="名称" min-width="110" />
            <el-table-column prop="capacity" label="载客量" width="90" />
            <el-table-column prop="currentStation" label="当前站点" min-width="110" />
            <el-table-column prop="passengerCount" label="当前载客" width="90" />
            <el-table-column label="状态" width="100">
              <template #default="{ row }">
                <el-tag :type="STATUS_TAG[row.status]">{{ STATUS_TEXT[row.status] }}</el-tag>
              </template>
            </el-table-column>
            <el-table-column v-if="authStore.isAdmin" label="操作" width="140" fixed="right">
              <template #default="{ row }">
                <el-button link type="primary" size="small" @click="openEdit(row)">编辑</el-button>
                <el-button link type="danger" size="small" @click="handleDelete(row)">删除</el-button>
              </template>
            </el-table-column>
          </el-table>
        </el-tab-pane>
        <el-tab-pane label="路线与距离矩阵" name="routes" lazy>
          <RouteManage />
        </el-tab-pane>
      </el-tabs>
    </el-card>

    <el-dialog
      v-model="dialogVisible"
      :title="dialogMode === 'add' ? '新增摆渡车' : '编辑摆渡车'"
      width="480px"
    >
      <el-form :model="form" label-width="90px">
        <el-form-item label="名称">
          <el-input v-model="form.name" placeholder="请输入车辆名称" />
        </el-form-item>
        <el-form-item label="载客量">
          <el-input-number v-model="form.capacity" :min="0" style="width: 100%" />
        </el-form-item>
        <el-form-item label="当前站点">
          <el-select v-model="form.currentStation" placeholder="请选择当前站点" style="width: 100%">
            <el-option v-for="s in STATIONS" :key="s" :label="s" :value="s" />
          </el-select>
        </el-form-item>
        <el-form-item label="状态">
          <el-select v-model="form.status" placeholder="请选择状态" style="width: 100%">
            <el-option
              v-for="s in STATUS_OPTIONS"
              :key="s.value"
              :label="s.label"
              :value="s.value"
            />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="dialogVisible = false">取消</el-button>
        <el-button type="primary" @click="handleSave">确定</el-button>
      </template>
    </el-dialog>
  </div>
</template>

<script setup lang="ts">
import { ref, reactive } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { useAuthStore } from '@/stores/auth'
import { usePolling } from '@/composables/usePolling'
import { getShuttles } from '@/api'
import type { Shuttle } from '@/api'
import RouteManage from './RouteManage.vue'

type ShuttleStatus = Shuttle['status']

const authStore = useAuthStore()
const shuttles = ref<Shuttle[]>([])
const activeTab = ref('vehicles')

/** 可用站点列表 */
const STATIONS = ['东门', '索道站', '观瀑台', '百花谷', '望湖亭', '青石古镇', '山顶广场']

/** 车辆状态中文文案与标签类型 */
const STATUS_TEXT: Record<ShuttleStatus, string> = {
  RUNNING: '运行中',
  IDLE: '空闲',
  MAINTENANCE: '维保中',
}

const STATUS_TAG: Record<ShuttleStatus, 'success' | 'info' | 'warning'> = {
  RUNNING: 'success',
  IDLE: 'info',
  MAINTENANCE: 'warning',
}

const STATUS_OPTIONS: { value: ShuttleStatus; label: string }[] = [
  { value: 'RUNNING', label: '运行中' },
  { value: 'IDLE', label: '空闲' },
  { value: 'MAINTENANCE', label: '维保中' },
]

async function fetchShuttles() {
  try {
    const res = await getShuttles()
    shuttles.value = res.data
  } catch (e) {}
}

// 定时刷新（5 秒）
usePolling(fetchShuttles, 5000)

const dialogVisible = ref(false)
const dialogMode = ref<'add' | 'edit'>('add')
const editingId = ref<string | null>(null)

const form = reactive({
  name: '',
  capacity: 0,
  currentStation: STATIONS[0],
  status: 'IDLE' as ShuttleStatus,
})

function resetForm() {
  form.name = ''
  form.capacity = 0
  form.currentStation = STATIONS[0]
  form.status = 'IDLE'
}

function openAdd() {
  dialogMode.value = 'add'
  editingId.value = null
  resetForm()
  dialogVisible.value = true
}

function openEdit(row: Shuttle) {
  dialogMode.value = 'edit'
  editingId.value = row.shuttleId
  form.name = row.name
  form.capacity = row.capacity
  form.currentStation = row.currentStation
  form.status = row.status
  dialogVisible.value = true
}

function handleSave() {
  if (!form.name) {
    ElMessage.warning('请输入车辆名称')
    return
  }
  // TODO: 后端摆渡车管理接口就绪后，替换为对应的 create / update / delete 调用
  if (dialogMode.value === 'add') {
    shuttles.value.push({
      shuttleId: `V${Date.now()}`,
      name: form.name,
      capacity: form.capacity,
      currentStation: form.currentStation,
      passengerCount: 0,
      status: form.status,
    })
  } else if (editingId.value) {
    const target = shuttles.value.find((s) => s.shuttleId === editingId.value)
    if (target) {
      target.name = form.name
      target.capacity = form.capacity
      target.currentStation = form.currentStation
      target.status = form.status
    }
  }
  ElMessage.success('操作成功')
  dialogVisible.value = false
}

async function handleDelete(row: Shuttle) {
  try {
    await ElMessageBox.confirm(`确定删除摆渡车「${row.name}」吗？`, '提示', {
      confirmButtonText: '确定',
      cancelButtonText: '取消',
      type: 'warning',
    })
  } catch (e) {
    return
  }
  // TODO: 后端摆渡车管理接口就绪后，替换为对应的 create / update / delete 调用
  shuttles.value = shuttles.value.filter((s) => s.shuttleId !== row.shuttleId)
  ElMessage.success('操作成功')
}
</script>

<style scoped>
.shuttle-manage-page {
  display: flex;
  flex-direction: column;
  gap: 16px;
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
</style>
