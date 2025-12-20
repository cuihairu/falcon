[根目录](../../CLAUDE.md) > [apps](../) > **web**

---

# Web - Web 管理界面（预留）

## 状态

**规划中** - 当前阶段专注于核心库与 CLI/Daemon 开发

---

## 技术选型

### 推荐方案

#### 前端框架
- **React** + Vite + TypeScript（推荐）
- **Vue 3** + Vite + TypeScript（备选）

#### UI 组件库
- Ant Design / Material-UI（React）
- Element Plus / Vuetify（Vue）

#### 状态管理
- Zustand / Jotai（React）
- Pinia（Vue）

#### API 通信
- Axios / Fetch API
- SWR / TanStack Query（数据缓存与同步）

---

## 功能规划

### 核心功能
- 任务管理（创建/暂停/恢复/取消）
- 实时进度显示（WebSocket/SSE）
- 多设备同步（连接同一个 Daemon）
- 用户认证与权限管理
- 配置管理界面

### 高级功能
- 数据可视化（下载统计、速度曲线）
- 任务调度（定时下载、优先级）
- 通知推送（浏览器通知 API）
- 多语言支持（i18n）
- 响应式设计（移动端适配）

---

## 架构设计

```
┌────────────────────────────────────┐
│         前端应用 (React/Vue)        │
│  ┌──────────────────────────────┐  │
│  │  页面组件                    │  │
│  │  - TaskList                  │  │
│  │  - TaskDetail                │  │
│  │  - Settings                  │  │
│  └──────────┬───────────────────┘  │
│             │                       │
│  ┌──────────▼───────────────────┐  │
│  │  API Client (Axios)          │  │
│  │  - REST API 调用             │  │
│  │  - WebSocket 连接            │  │
│  └──────────┬───────────────────┘  │
└─────────────┼────────────────────────┘
              │ HTTP/WS
┌─────────────▼────────────────────┐
│     falcon-daemon                │
│  - REST API                      │
│  - WebSocket 事件流              │
└──────────────────────────────────┘
```

---

## API 接口（与 Daemon 对接）

### REST API

```
GET    /api/tasks              # 获取任务列表
POST   /api/tasks              # 创建任务
GET    /api/tasks/:id          # 获取任务详情
PUT    /api/tasks/:id/pause    # 暂停任务
PUT    /api/tasks/:id/resume   # 恢复任务
DELETE /api/tasks/:id          # 取消任务
GET    /api/status             # 全局状态
PUT    /api/config             # 更新配置
```

### WebSocket/SSE 事件

```json
{
  "type": "task_progress",
  "task_id": 123,
  "progress": 0.75,
  "speed": 5242880
}

{
  "type": "task_completed",
  "task_id": 123
}
```

---

## 下一步计划

1. 完成 Daemon REST API 开发
2. 设计 UI/UX 原型
3. 实现前端基础框架（Vite + React/Vue）
4. 开发核心页面（任务列表、详情、设置）
5. WebSocket 实时通信

---

**技术讨论**：欢迎在 `docs/decisions/` 中创建 ADR 记录技术选型决策。
