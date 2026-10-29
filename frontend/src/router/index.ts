import { createRouter, createWebHistory } from 'vue-router'
import Login from '@/views/Login.vue'
import UserLayout from '@/layouts/UserLayout.vue'
import AdminLayout from '@/layouts/AdminLayout.vue'
import UserHome from '@/views/user/UserHome.vue'
import SpotList from '@/views/user/SpotList.vue'
import SpotDetail from '@/views/user/SpotDetail.vue'
import PoiMap from '@/views/user/PoiMap.vue'
import RouteGuide from '@/views/user/RouteGuide.vue'
import ParkingGuide from '@/views/user/ParkingGuide.vue'
import ShuttleGuide from '@/views/user/ShuttleGuide.vue'
import Overview from '@/views/Overview.vue'
import Spots from '@/views/Spots.vue'
import Alerts from '@/views/admin/Alerts.vue'
import Scheduling from '@/views/admin/Scheduling.vue'
import Statistics from '@/views/admin/Statistics.vue'
import SpotManage from '@/views/admin/SpotManage.vue'
import ParkingManage from '@/views/admin/ParkingManage.vue'
import ShuttleManage from '@/views/admin/ShuttleManage.vue'
import DataEntry from '@/views/admin/DataEntry.vue'
import Simulation from '@/views/admin/Simulation.vue'
import Rules from '@/views/admin/Rules.vue'
import Users from '@/views/admin/Users.vue'
import Settings from '@/views/admin/Settings.vue'
import SysMonitor from '@/views/admin/SysMonitor.vue'

const ADMIN_ROLES = ['admin', 'operator']

const router = createRouter({
  history: createWebHistory(),
  routes: [
    { path: '/', redirect: '/home' },
    { path: '/login', component: Login },
    {
      path: '/',
      component: UserLayout,
      children: [
        { path: 'home', component: UserHome },
        { path: 'spots', component: SpotList },
        { path: 'spots/:id', component: SpotDetail },
        { path: 'map', component: PoiMap },
        { path: 'routes', component: RouteGuide },
        { path: 'parking', component: ParkingGuide },
        { path: 'shuttles', component: ShuttleGuide },
      ],
    },
    {
      path: '/admin',
      component: AdminLayout,
      redirect: '/admin/overview',
      children: [
        { path: 'overview', component: Overview, meta: { title: '景区概览', roles: ADMIN_ROLES } },
        { path: 'flow', component: Spots, meta: { title: '景点流量看板', roles: ADMIN_ROLES } },
        { path: 'alerts', component: Alerts, meta: { title: '预警中心', roles: ADMIN_ROLES } },
        { path: 'scheduling', component: Scheduling, meta: { title: '调度建议', roles: ADMIN_ROLES } },
        { path: 'statistics', component: Statistics, meta: { title: '数据统计', roles: ADMIN_ROLES } },
        { path: 'spot-manage', component: SpotManage, meta: { title: '景点管理', roles: ADMIN_ROLES } },
        {
          path: 'resources/parking',
          component: ParkingManage,
          meta: { title: '停车场管理', roles: ADMIN_ROLES },
        },
        {
          path: 'resources/shuttles',
          component: ShuttleManage,
          meta: { title: '摆渡车管理', roles: ADMIN_ROLES },
        },
        { path: 'entry', component: DataEntry, meta: { title: '数据录入', roles: ['admin'] } },
        { path: 'rules', component: Rules, meta: { title: '预警阈值配置', roles: ['admin'] } },
        { path: 'users', component: Users, meta: { title: '用户管理', roles: ['admin'] } },
        { path: 'settings', component: Settings, meta: { title: '系统设置', roles: ['admin'] } },
        // 演示/运维工具页：不进菜单，仅 URL 直达
        { path: 'simulation', component: Simulation, meta: { title: '模拟配置', roles: ['admin'] } },
        { path: 'monitor', component: SysMonitor, meta: { title: '系统监控', roles: ['admin'] } },
        // 兼容旧链接
        { path: 'events', redirect: '/admin/alerts' },
        { path: 'resources/routes', redirect: '/admin/resources/shuttles' },
      ],
    },
    { path: '/:pathMatch(.*)*', redirect: '/home' },
  ],
})

function homeByRole(role: string): string {
  return role === 'admin' || role === 'operator' ? '/admin/overview' : '/home'
}

router.beforeEach((to) => {
  const token = localStorage.getItem('token')
  const role = localStorage.getItem('role') || ''

  if (to.path === '/login') {
    return token ? homeByRole(role) : true
  }

  // 管理端页面需要登录且角色匹配
  if (to.path.startsWith('/admin')) {
    if (!token) return '/login'
    const roles = to.meta.roles as string[] | undefined
    if (roles && !roles.includes(role)) {
      return homeByRole(role)
    }
  }

  return true
})

export default router
