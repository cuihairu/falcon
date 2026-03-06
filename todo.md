# TODO

## 已完成（本轮）

### 编译问题修复
- ✅ 修复 OpenSSL 定义不匹配问题（`FALCON_HAS_OPENSSL` vs `FALCON_USE_OPENSSL`）
  - `packages/libfalcon/src/file_hash.cpp`
  - `packages/libfalcon/src/incremental_download.cpp`
- ✅ 修复 Windows WSAPOLLFD 类型定义问题
  - `packages/libfalcon/include/falcon/net/event_poll.hpp`

### CLI 测试
- ✅ 创建 CLI 单元测试框架
  - `packages/falcon-cli/tests/CMakeLists.txt`
  - `packages/falcon-cli/tests/unit/arg_parser_test.cpp`
- ✅ 测试覆盖：parse_size_bytes, parse_bool, read_urls_from_content

### Daemon 任务持久化
- ✅ 实现 SQLite 任务持久化
  - `packages/falcon-daemon/src/storage/task_storage.hpp`
  - `packages/falcon-daemon/src/storage/task_storage.cpp`
- ✅ 创建持久化单元测试
  - `packages/falcon-daemon/tests/task_storage_test.cpp`
- ✅ 支持 CRUD 操作、批量查询、进度更新、状态更新
- ✅ 支持 JSON 序列化 DownloadOptions

---

## 待继续

### 编译验证
1. 配置 C++ 编译器（MSVC/MinGW/Clang）
2. 编译通过后执行测试：
   - `falcon_core_tests`
   - `falcon_cli_tests`
   - `falcon_daemon_rpc_tests`
   - `falcon_daemon_storage_tests`
3. 生成覆盖率报告并确认覆盖率 >= 80%

### 功能完善
1. Daemon 守护进程模式实现
   - Unix: `fork()` + `setsid()`
   - Windows: Service API
   - PID 文件管理
   - 信号处理

2. CLI 配置文件支持
   - `~/.config/falcon/config.json`
   - 合并命令行和配置文件选项

3. 桌面应用完善
   - 真实下载功能集成
   - 设置持久化

---

## 当前状态

- ✅ 已完成编译问题修复
- ✅ 已添加 CLI 单元测试
- ✅ 已实现 Daemon 任务持久化
- ⏳ 等待编译器环境配置进行验证

## 下一步优先级

1. **高优先级**：配置编译环境，验证编译和测试
2. **中优先级**：实现守护进程模式
3. **低优先级**：文档和 CI/CD 优化
