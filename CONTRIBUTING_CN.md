# 参与 Falcon 贡献

感谢你有兴趣为本项目做贡献！本文档覆盖基础流程。

## 快速上手

```bash
git clone https://github.com/cuihairu/falcon.git
cd falcon

# 启用测试的调试配置
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DFALCON_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

工具链与第三方库要求参见 `README_CN.md` 的「系统要求」一节。

## 项目结构

Falcon 是 CMake Monorepo：

- `packages/libfalcon-core` — 下载引擎、任务管理、事件系统
- `packages/libfalcon-protocols` — HTTP/FTP/BT/Metalink/HLS 等协议实现
- `packages/libfalcon-storage` — S3/OSS/COS/Kodo/又拍云对象存储浏览
- `packages/libfalcon-drives` — 网盘、资源搜索、加密配置管理
- `packages/falcon-cli` — 命令行下载工具
- `packages/falcon-daemon` — RPC 服务（aria2 兼容 JSON-RPC）
- `apps/desktop` — Qt6 桌面应用

依赖方向：`protocols/storage/drives → core`，禁止反向依赖。

## 代码风格

- 遵循 Google C++ 风格指南；函数/变量 `snake_case`，类型 `PascalCase`
- 使用 `clang-format` 格式化（配置见 `.clang-format`）
- C++17；代码需在 GCC / Clang / MSVC 三平台保持零告警

## 测试

- 新功能须在所属包的 `tests/` 目录补充单元测试（Google Test）
- 网络相关代码一律使用本地可编程 mock/回环服务器测试，不依赖真实外部服务
- 提交前跑全量测试：`ctest --test-dir build --output-on-failure`

## Pull Request

1. 新建功能分支，保持改动聚焦
2. 确保你所在平台构建与测试全部通过
3. 在 PR 描述中说明改动内容与原因

## 许可证

提交贡献即表示你同意该贡献以 Apache License 2.0 许可证发布。
