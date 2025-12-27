# Falcon Desktop - Qt 桌面应用

Falcon 下载器的 Qt6 图形用户界面。

## 技术栈

- **Qt 6.x**: 跨平台 GUI 框架
- **C++17**: 编程语言标准
- **CMake**: 构建系统

## 环境要求

### macOS

```bash
# 使用 Homebrew 安装 Qt6
brew install qt@6

# 设置 Qt6 路径
export Qt6_DIR=/usr/local/opt/qt@6
export CMAKE_PREFIX_PATH=/usr/local/opt/qt@6
```

### Ubuntu/Debian

```bash
# 方式 1: 使用 apt 安装（推荐，版本较旧）
sudo apt-get update
sudo apt-get install -y qt6-base-dev qt6-tools-dev cmake

# 方式 2: 使用 aqtinstall（推荐，最新版本）
pip3 install aqtinstall
aqt install-qt linux desktop linux_gcc_64_qt6
# 设置 Qt6 路径
export Qt6_DIR=~/Qt/6.x.x/gcc_64
export CMAKE_PREFIX_PATH=~/Qt/6.x.x/gcc_64
```

### Windows

```powershell
# 方式 1: 使用 vcpkg（推荐）
vcpkg install qt6-base qt6-tools
# CMake 会自动检测 vcpkg

# 方式 2: 使用在线安装器
# 从 https://www.qt.io/download-qt 下载 Qt 在线安装器
# 安装 Qt6 并添加到 PATH

# 设置环境变量（PowerShell）
$env:Qt6_DIR="C:\Qt\6.x.x\msvc2019_64"
$env:CMAKE_PREFIX_PATH="C:\Qt\6.x.x\msvc2019_64"
```

## 构建步骤

### 配置项目

```bash
# 在项目根目录
cmake -B build -DFALCON_BUILD_DESKTOP=ON
```

### 编译

```bash
cmake --build build --target falcon-desktop
```

### 运行

```bash
# macOS/Linux
./build/bin/falcon-desktop

# Windows
.\build\bin\Release\falcon-desktop.exe
```

## 功能模块

### 1. 下载管理页面

- **正在下载**: 实时显示下载进度、速度、状态
- **已完成**: 已完成下载的文件列表
- **回收站**: 已删除的下载记录
- **下载记录**: 所有下载历史

### 2. 云盘页面

支持多种对象存储服务：
- Amazon S3
- 阿里云 OSS
- 腾讯云 COS
- 七牛云 Kodo
- 又拍云 Upyun

功能：
- 文件浏览
- 上传/下载
- 新建文件夹
- 删除操作

### 3. 发现页面

资源搜索功能：
- 磁力链接搜索
- HTTP/HTTPS 资源搜索
- 云盘资源搜索
- FTP 资源搜索

## 开发指南

### 项目结构

```
apps/desktop/
├── CMakeLists.txt          # 构建配置
├── README.md               # 本文档
└── src/
    ├── main.cpp            # 程序入口
    ├── main_window.hpp/cpp # 主窗口
    ├── navigation/
    │   └── sidebar.hpp/cpp # 侧边导航栏
    └── pages/
        ├── download_page.hpp/cpp   # 下载管理
        ├── cloud_page.hpp/cpp      # 云盘浏览
        └── discovery_page.hpp/cpp  # 资源发现
```

### 添加新页面

1. 在 `src/pages/` 创建页面头文件和源文件
2. 继承 `QWidget` 并实现界面
3. 在 `main_window.cpp` 中注册页面
4. 在 `sidebar.cpp` 添加导航按钮

### 信号与槽

使用 Qt 的信号槽机制进行组件通信：

```cpp
// 连接按钮点击信号
connect(button, &QPushButton::clicked, this, &MyPage::on_button_clicked);

// 自定义信号
signals:
    void taskCompleted(const QString& taskId);

// 槽函数
private slots:
    void on_button_clicked();
```

## 与 libfalcon 集成

桌面应用通过调用 libfalcon 核心库实现下载功能：

```cpp
#include <falcon/download_engine.hpp>

// 创建下载引擎
falcon::DownloadEngine engine;

// 配置下载选项
falcon::DownloadOptions options;
options.max_connections = 5;
options.output_directory = "/path/to/downloads";

// 启动下载
auto task = engine.start_download(url, options);

// 监听进度
task->wait();
```

## 已知限制

当前版本为 UI 框架，以下功能待实现：

- [ ] 实际的下载引擎集成
- [ ] 云存储 API 集成
- [ ] 资源搜索 API 集成
- [ ] 系统托盘图标
- [ ] 下载完成通知
- [ ] 主题切换
- [ ] 配置持久化
- [ ] 多语言支持

## 故障排除

### Qt6 找不到

```bash
# macOS
brew info qt@6

# Linux
dpkg -L qt6-base-dev

# 手动指定路径
cmake -B build -DQt6_DIR=/path/to/qt6
```

### 编译错误

确保使用 C++17 或更高版本：

```bash
cmake -B build -DCMAKE_CXX_STANDARD=17
```

### 运行时库找不到

```bash
# macOS
install_name_tool -change @rpath/QtWidgets.framework/libQtWidgets.dylib \
  /usr/local/opt/qt@6/lib/QtWidgets.framework/libQtWidgets.dylib \
  build/bin/falcon-desktop

# Linux
export LD_LIBRARY_PATH=/usr/local/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

# Windows
# 将 Qt 的 bin 目录添加到 PATH
```

## 许可证

Apache License 2.0

## 贡献

欢迎提交 Issue 和 Pull Request！
