[根目录](../../CLAUDE.md) > [packages](../) > **falcon-cli**

---

# falcon-cli - 命令行下载工具

## 变更记录 (Changelog)

### 2025-12-21 - 初始化模块架构
- 创建 CLI 项目结构
- 定义命令行参数设计
- 规划进度显示与交互

---

## 模块职责

`falcon-cli` 是基于 `libfalcon` 核心库的命令行下载工具，提供类似 `wget`、`curl`、`aria2c` 的用户体验。

### 核心功能
- 单文件/批量下载
- 进度条与速度显示
- 断点续传
- 任务管理（暂停/恢复/取消）
- 配置文件支持
- 日志输出

---

## 入口与启动

### 编译与运行

```bash
# 编译
cmake -B build -DFALCON_BUILD_CLI=ON
cmake --build build --target falcon-cli

# 运行
./build/bin/falcon-cli --help
```

### 命令行参数设计

```bash
# 基础下载
falcon-cli <URL>
falcon-cli https://example.com/file.zip

# 指定输出路径
falcon-cli <URL> -o /path/to/file
falcon-cli <URL> --output /path/to/file

# 多线程下载
falcon-cli <URL> -c 5              # 5 个并发连接
falcon-cli <URL> --connections 5

# 速度限制
falcon-cli <URL> --limit 1M        # 限速 1 MB/s
falcon-cli <URL> -l 512K           # 限速 512 KB/s

# 批量下载（从文件读取 URL 列表）
falcon-cli -i urls.txt
falcon-cli --input-file urls.txt

# 断点续传
falcon-cli <URL> --continue        # 默认启用
falcon-cli <URL> --no-continue     # 禁用续传

# 重试与超时
falcon-cli <URL> --retry 5 --timeout 60

# 代理
falcon-cli <URL> --proxy http://proxy:8080

# 详细输出
falcon-cli <URL> -v                # 详细模式
falcon-cli <URL> --quiet           # 静默模式

# 配置文件
falcon-cli <URL> --config ~/.config/falcon/config.json
```

---

## 对外接口

### 主要命令选项

| 选项 | 简写 | 类型 | 说明 |
|------|------|------|------|
| `--help` | `-h` | 标志 | 显示帮助信息 |
| `--version` | `-V` | 标志 | 显示版本信息 |
| `--output` | `-o` | 字符串 | 输出文件路径 |
| `--directory` | `-d` | 字符串 | 输出目录 |
| `--connections` | `-c` | 整数 | 并发连接数（1-16） |
| `--limit` | `-l` | 字符串 | 速度限制（如 1M, 512K） |
| `--input-file` | `-i` | 字符串 | URL 列表文件 |
| `--continue` | | 标志 | 启用断点续传（默认） |
| `--no-continue` | | 标志 | 禁用断点续传 |
| `--retry` | | 整数 | 重试次数（0-10） |
| `--timeout` | `-t` | 整数 | 超时时间（秒） |
| `--proxy` | | 字符串 | 代理服务器 |
| `--no-verify-ssl` | | 标志 | 跳过 SSL 验证 |
| `--user-agent` | `-U` | 字符串 | 自定义 User-Agent |
| `--header` | `-H` | 字符串 | 自定义 HTTP 头（可多次） |
| `--verbose` | `-v` | 标志 | 详细输出 |
| `--quiet` | `-q` | 标志 | 静默模式 |
| `--config` | | 字符串 | 配置文件路径 |

---

## 关键依赖与配置

### 编译依赖

| 依赖库 | 用途 | 版本 |
|--------|------|------|
| libfalcon | 核心下载库 | 0.1.0 |
| CLI11 | 命令行解析 | 2.3+ |
| spdlog | 日志 | 1.9+ |

### 配置文件格式

配置文件位置（按优先级）：
1. 命令行 `--config` 指定路径
2. `~/.config/falcon/config.json`
3. `/etc/falcon/config.json`

示例配置（`~/.config/falcon/config.json`）：

```json
{
  "default_download_dir": "~/Downloads",
  "max_connections": 5,
  "timeout_seconds": 30,
  "max_retries": 3,
  "speed_limit": "0",
  "user_agent": "Falcon/0.1",
  "verify_ssl": true,
  "auto_resume": true,
  "log_level": "info",
  "proxy": ""
}
```

---

## 数据模型

### 进度显示格式

```
[下载中] file.zip
进度: [=============================>           ] 70% (700MB / 1GB)
速度: 5.2 MB/s | 预计剩余时间: 00:01:23 | 连接数: 5/5
```

### 批量下载格式（`urls.txt`）

```
https://example.com/file1.zip
https://example.com/file2.tar.gz
ftp://ftp.example.com/data.csv
```

支持带选项的格式：

```
https://example.com/file1.zip
  out=file1_renamed.zip
  connections=8
  limit=2M

https://example.com/file2.tar.gz
  dir=/tmp/downloads
```

---

## 测试与质量

### 测试策略

#### 1. 单元测试（`tests/unit/`）

- `arg_parser_test.cpp`：命令行参数解析测试
- `config_loader_test.cpp`：配置文件加载测试
- `progress_display_test.cpp`：进度显示逻辑测试

#### 2. 集成测试（`tests/integration/`）

- `cli_download_test.cpp`：完整下载流程测试（调用真实 HTTP 服务器）
- `batch_download_test.cpp`：批量下载测试
- `resume_test.cpp`：断点续传测试

### 测试覆盖率目标

- 核心逻辑：≥ 70%
- 命令行解析：≥ 90%

---

## 常见问题 (FAQ)

### Q1：如何显示实时进度？

使用 ANSI 转义码更新同一行输出（参考 `progress_display.cpp`）。

### Q2：如何支持中文文件名？

使用 UTF-8 编码，确保终端支持 Unicode。

### Q3：如何与 libfalcon 交互？

通过 `IEventListener` 接口接收下载事件，更新进度显示。

---

## 相关文件清单

```
packages/falcon-cli/
├── CMakeLists.txt
├── CLAUDE.md
├── src/
│   ├── main.cpp              # 主入口
│   ├── arg_parser.cpp        # 命令行解析（CLI11）
│   ├── config_loader.cpp     # 配置文件加载
│   ├── progress_display.cpp  # 进度条显示
│   ├── batch_downloader.cpp  # 批量下载逻辑
│   └── event_handler.cpp     # libfalcon 事件处理器
└── tests/
    ├── unit/
    └── integration/
```

---

## 下一步开发计划

### 第一阶段（基础功能）
1. 实现命令行参数解析（CLI11）
2. 单文件下载与进度显示
3. 配置文件加载
4. 基础日志输出

### 第二阶段（高级功能）
1. 批量下载（URL 列表文件）
2. 断点续传支持
3. 速度限制与并发控制
4. 代理与自定义头支持

### 第三阶段（用户体验）
1. 交互式任务管理（暂停/恢复/取消）
2. 彩色输出与主题
3. Shell 自动补全（Bash/Zsh）
4. 国际化（i18n）

---

**文档维护**：每次新增命令选项、修改配置格式时，请更新本文档并在"变更记录"中添加条目。
