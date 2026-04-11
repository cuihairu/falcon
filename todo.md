# TODO

## 已完成

### 第一阶段：编译问题修复 ✅
- ✅ 修复 OpenSSL 定义不匹配（`FALCON_HAS_OPENSSL` vs `FALCON_USE_OPENSSL`）
- ✅ 修复 Windows WSAPOLLFD 类型定义兼容性

### 第二阶段：CLI 测试 ✅
- ✅ 创建 CLI 单元测试框架
- ✅ arg_parser_test.cpp - 参数解析测试

### 第三阶段：Daemon 任务持久化 ✅
- ✅ SQLite 任务存储（task_storage.hpp/cpp）
- ✅ CRUD、查询、批量更新操作
- ✅ task_storage_test.cpp 单元测试

### 第四阶段：Daemon 守护进程模式 ✅
- ✅ daemon.hpp/cpp - 跨平台守护进程实现
  - Unix: fork() + setsid() 守护化
  - Windows: Service API 支持
  - PID 文件管理
  - 信号处理
- ✅ 集成到 main.cpp
- ✅ 命令行选项：-d/--daemon, --pid-file, --working-dir, --log-file
- ✅ Windows 服务管理：--install-service, --uninstall-service

### 第五阶段：CLI 配置文件支持 ✅
- ✅ config_loader.hpp/cpp - JSON 配置加载器
- ✅ 配置搜索路径（当前目录 → 用户配置 → 系统配置）
- ✅ 环境变量支持（FALCON_*）
- ✅ 命令行与配置文件合并（命令行优先）
- ✅ 命令行选项：--config, --show-config-path, --create-default-config
- ✅ config_loader_test.cpp 单元测试

### 第六阶段：编译验证与测试收口 ✅
- ✅ 本机编译器环境验证完成（Apple Clang 17.0.0 / CMake 4.2.3 / Ninja 1.13.2）
- ✅ 编译通过：
  - `falcon`
  - `falcon-cli`
  - `falcon-daemon`
  - `falcon-desktop`
- ✅ 测试通过：
  - `falcon_core_tests`（793 tests）
  - `falcon_cli_tests`（37 tests）
  - `falcon_daemon_rpc_tests`（2 tests）
  - `falcon_daemon_storage_tests`（22 tests）
- ✅ 补齐脚本能力：
  - `scripts/run_all_tests.sh` 改为按明确目标构建/执行
  - 增加 macOS 兼容的并行度探测
  - 增加 `--coverage` / `--skip-desktop`
  - 测试结果输出为 JUnit XML + HTML 汇总

---

## 当前无阻塞性 TODO

当前 `todo.md` 中原有待办已完成或已转入长期产品规划，不再作为本轮阻塞项维护。

## 后续规划

### 产品能力
- 桌面应用接入真实下载链路（核心库 / Daemon）
- 云存储页接入真实浏览、上传、下载、删除、重命名能力
- Discovery 页接入真实搜索源

### 工程化
- ADR 文档（异步模型、持久化策略）
- 用户指南与故障排除文档
- 覆盖率报告上传到 CI
- 发布工作流

---

## 当前状态

- ✅ 编译问题修复
- ✅ CLI 单元测试
- ✅ Daemon 任务持久化
- ✅ Daemon 守护进程模式
- ✅ CLI 配置文件支持
- ✅ 编译器环境验证
- ✅ 关键测试目标验证

## 新增文件

```
packages/falcon-cli/
├── src/
│   ├── config_loader.hpp      # 配置加载器头文件
│   └── config_loader.cpp      # 配置加载器实现
└── tests/
    ├── CMakeLists.txt         # 测试构建配置
    └── unit/
        ├── arg_parser_test.cpp    # 参数解析测试
        └── config_loader_test.cpp # 配置加载测试

packages/falcon-daemon/
├── src/
│   ├── daemon/
│   │   ├── daemon.hpp         # 守护进程管理头文件
│   │   └── daemon.cpp         # 守护进程管理实现
│   └── storage/
│       ├── task_storage.hpp   # 任务存储头文件
│       └── task_storage.cpp   # 任务存储实现
└── tests/
    └── task_storage_test.cpp  # 任务存储测试
```
