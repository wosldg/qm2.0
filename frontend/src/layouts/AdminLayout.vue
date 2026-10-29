<template>
  <div class="admin-layout">
    <aside class="sidebar" :class="{ collapsed }">
      <div class="sidebar-logo" @click="$router.push('/admin/overview')">
        <span class="logo-icon">🏞</span>
        <span v-if="!collapsed" class="logo-text">ScenicFlow 管理平台</span>
      </div>

      <el-menu
        :default-active="activeMenu"
        :collapse="collapsed"
        background-color="#001529"
        text-color="rgba(255,255,255,0.72)"
        active-text-color="#409eff"
        router
        class="sidebar-menu"
      >
        <template v-for="item in visibleMenus" :key="item.title">
          <el-sub-menu v-if="item.children" :index="item.title">
            <template #title>
              <el-icon><component :is="item.icon" /></el-icon>
              <span>{{ item.title }}</span>
            </template>
            <el-menu-item v-for="child in item.children" :key="child.path" :index="child.path!">
              <el-icon><component :is="child.icon" /></el-icon>
              <span>{{ child.title }}</span>
            </el-menu-item>
          </el-sub-menu>
          <el-menu-item v-else :index="item.path!">
            <el-icon><component :is="item.icon" /></el-icon>
            <span>{{ item.title }}</span>
          </el-menu-item>
        </template>
      </el-menu>
    </aside>

    <div class="admin-main">
      <header class="admin-header">
        <el-button text class="collapse-btn" @click="collapsed = !collapsed">
          <el-icon><component :is="collapsed ? Expand : Fold" /></el-icon>
        </el-button>
        <div class="page-title">{{ pageTitle }}</div>

        <div class="header-right">
          <el-button size="small" text @click="$router.push('/home')">用户端预览</el-button>
          <el-tag :type="authStore.role === 'admin' ? 'danger' : 'success'" size="small">
            {{ roleText }}
          </el-tag>
          <span class="username">{{ authStore.username }}</span>
          <el-button size="small" @click="handleLogout">退出</el-button>
        </div>
      </header>

      <main class="admin-content">
        <router-view />
      </main>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { useAuthStore } from '@/stores/auth'
import { ROLE_LABELS } from '@/utils/status'
import {
  Fold,
  Expand,
  Odometer,
  TrendCharts,
  Warning,
  Guide,
  PieChart,
  Location,
  Van,
  EditPen,
  SetUp,
  User,
  Setting,
} from '@element-plus/icons-vue'

const route = useRoute()
const router = useRouter()
const authStore = useAuthStore()

const collapsed = ref(false)

interface MenuItem {
  title: string
  icon: unknown
  roles: string[]
  path?: string
  children?: MenuItem[]
}

const ALL: string[] = ['admin', 'operator']

// 菜单按「监测指挥 / 预警处置 / 资源管理 / 系统配置」四个业务域分组。
// 模拟配置、系统监控属演示/运维工具，不进菜单，仅 URL 直达（见 router）。
const menus: MenuItem[] = [
  {
    title: '监测指挥',
    icon: Odometer,
    roles: ALL,
    children: [
      { title: '景区概览', path: '/admin/overview', icon: Odometer, roles: ALL },
      { title: '景点流量看板', path: '/admin/flow', icon: TrendCharts, roles: ALL },
      { title: '数据统计', path: '/admin/statistics', icon: PieChart, roles: ALL },
    ],
  },
  {
    title: '预警处置',
    icon: Warning,
    roles: ALL,
    children: [
      { title: '预警中心', path: '/admin/alerts', icon: Warning, roles: ALL },
      { title: '调度建议', path: '/admin/scheduling', icon: Guide, roles: ALL },
    ],
  },
  {
    title: '资源管理',
    icon: Van,
    roles: ALL,
    children: [
      { title: '景点管理', path: '/admin/spot-manage', icon: Location, roles: ALL },
      { title: '停车场管理', path: '/admin/resources/parking', icon: Location, roles: ALL },
      { title: '摆渡车管理', path: '/admin/resources/shuttles', icon: Van, roles: ALL },
    ],
  },
  {
    title: '系统配置',
    icon: Setting,
    roles: ['admin'],
    children: [
      { title: '预警阈值配置', path: '/admin/rules', icon: SetUp, roles: ['admin'] },
      { title: '数据录入', path: '/admin/entry', icon: EditPen, roles: ['admin'] },
      { title: '用户管理', path: '/admin/users', icon: User, roles: ['admin'] },
      { title: '系统设置', path: '/admin/settings', icon: Setting, roles: ['admin'] },
    ],
  },
]

/** 按当前角色过滤菜单（后端接口黑白名单兜底） */
const visibleMenus = computed<MenuItem[]>(() =>
  menus
    .filter((m) => m.roles.includes(authStore.role))
    .map((m) =>
      m.children
        ? {
            ...m,
            children: m.children.filter((c) => c.roles.includes(authStore.role)),
          }
        : m,
    ),
)

const activeMenu = computed(() => route.path)

const pageTitle = computed(() => (route.meta.title as string) || '管理平台')

const roleText = computed(() => ROLE_LABELS[authStore.role] || authStore.role)

function handleLogout() {
  authStore.logout()
  ElMessage.success('已退出登录')
  router.push('/login')
}
</script>

<style scoped>
.admin-layout {
  display: flex;
  min-height: 100vh;
  background: #f5f7fa;
}

.sidebar {
  width: 208px;
  background: #001529;
  flex-shrink: 0;
  transition: width 0.25s;
  overflow: hidden;
}

.sidebar.collapsed {
  width: 64px;
}

.sidebar-logo {
  height: 56px;
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 0 16px;
  color: #fff;
  font-weight: 600;
  font-size: 15px;
  cursor: pointer;
  white-space: nowrap;
  border-bottom: 1px solid rgba(255, 255, 255, 0.08);
}

.logo-icon {
  font-size: 20px;
}

.sidebar-menu {
  border-right: none;
}

.sidebar-menu:not(.el-menu--collapse) {
  width: 208px;
}

.admin-main {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
}

.admin-header {
  height: 56px;
  background: #fff;
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 0 16px;
  box-shadow: 0 1px 4px rgba(0, 0, 0, 0.06);
  position: sticky;
  top: 0;
  z-index: 10;
}

.collapse-btn {
  font-size: 18px;
}

.page-title {
  font-size: 15px;
  font-weight: 600;
  color: #303133;
}

.header-right {
  margin-left: auto;
  display: flex;
  align-items: center;
  gap: 12px;
}

.username {
  font-size: 13px;
  color: #606266;
}

.admin-content {
  flex: 1;
  padding: 16px;
}

@media (max-width: 768px) {
  .sidebar {
    width: 64px;
  }

  .sidebar-menu:not(.el-menu--collapse) {
    width: 64px;
  }

  .username {
    display: none;
  }
}
</style>
