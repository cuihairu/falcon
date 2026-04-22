# Desktop 应用开发指南

## 概述

Falcon Desktop 是基于 Qt6 的跨平台桌面下载管理器，采用迅雷风格的 UI 设计。

## 技术栈

- **Qt 6.2+**: UI 框架
- **C++17**: 编程语言
- **CMake**: 构建系统
- **libfalcon**: 核心下载库

## 架构设计

```
┌─────────────────────────────────────────────────────┐
│                    MainWindow                       │
├──────────┬──────────────────────────────────────────┤
│ TopBar   │                                          │
├──┬───────┴──────────────────────────────────────────┤
│  │      ┌──────────────┐    ┌───────────────────┐   │
│  │      │              │    │                   │   │
│  │      │  SideBar     │    │   ContentStack    │   │
│  │      │              │    │                   │   │
│  │      │ - 下载中     │    │ - DownloadPage    │   │
│  │      │ - 已完成     │    │ - CloudPage      │   │
│  │      │ - 云添加     │    │ - DiscoveryPage  │   │
│  │      │ - 云盘       │    │ - SettingsPage   │   │
│  │      │ - 发现       │    │                   │   │
│  │      │ - 设置       │    │                   │   │
│  │      └──────────────┘    └───────────────────┘   │
│  │                                              ┌───┴───┐
│  │                                              │StatusBar│
├──┴──────────────────────────────────────────────┴──────┤
│              System Tray (QSystemTrayIcon)              │
└─────────────────────────────────────────────────────────┘
```

## 组件说明

### MainWindow

主窗口类，负责：
- 管理所有页面和组件
- 处理窗口关闭（最小化到托盘）
- 集成 DownloadEngine
- 连接信号/槽

### SideBar

侧边导航栏，提供：
- 下载中/已完成/云添加切换
- 云盘、发现、设置导航

### DownloadPage

下载管理页面，支持：
- 表格视图（传统列表）
- 网格视图（卡片布局）
- 任务过滤（下载中/已完成/云添加）
- 任务操作（暂停/继续/删除）

### CloudPage

云盘管理页面：
- S3/OSS/COS 等对象存储连接
- 远程资源浏览
- 文件上传/下载

### DiscoveryPage

资源发现页面：
- 多资源类型搜索（磁力/HTTP/网盘/FTP）
- 搜索结果展示
- 一键下载

### SettingsPage

设置页面：
- 剪切板监听配置
- 下载设置（并发数、保存目录）
- 连接设置（超时、重试）
- 通知设置
- 主题切换

## 服务层

### SearchService

搜索服务，支持：
- 磁力链接搜索
- HTTP 资源搜索
- 网盘资源搜索
- 后台线程搜索

### StorageService

存储服务桥接层：
- 连接/断开云存储
- 列出远程文件
- 下载远程文件

### ThemeManager

主题管理器：
- 亮色/暗色主题
- 完整 QSS 样式表
- 运行时切换
- 配置持久化

## 工具类

### UrlDetector

URL 检测器，支持：
- 标准协议（HTTP/HTTPS/FTP）
- 磁力链接（Magnet）
- 私有协议（Thunder/Flashget/ED2K）
- 云盘链接（百度/阿里云/夸克/天翼/蓝奏云）

### ClipboardMonitor

剪切板监听器：
- 自动检测下载链接
- 可配置检测延迟
- 可启用/禁用

## 样式指南

### QSS 样式

使用 QSS 实现主题样式：

```cpp
// 亮色主题
QWidget {
    background-color: #ffffff;
    color: #333333;
}

QPushButton#primaryButton {
    background-color: #0078d4;
    color: #ffffff;
}

// 暗色主题
QWidget {
    background-color: #1e1e1e;
    color: #e0e0e0;
}
```

### 对象命名

为特定 QSS 样式设置 objectName：

```cpp
button->setObjectName("primaryButton");
table->setObjectName("taskTable");
card->setObjectName("taskCard");
```

## 信号/槽连接

### 跨页面通信

使用信号/槽机制：

```cpp
// DiscoveryPage 发出下载请求
connect(discovery_page, &DiscoveryPage::direct_download_requested,
        this, &MainWindow::on_direct_download_requested);

// MainWindow 处理下载
void MainWindow::on_direct_download_requested(const QString& url, bool start) {
    add_download_task(url, start);
}
```

## 编译与运行

### 依赖

```bash
# Ubuntu/Debian
sudo apt install qt6-base-dev qt6-tools-dev

# macOS
brew install qt@6

# Windows
# 从 qt.io 下载 Qt6 installer
```

### 构建命令

```bash
# 配置
cmake -B build -S . \
  -DFALCON_BUILD_DESKTOP=ON \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x.x/gcc_64

# 编译
cmake --build build --target falcon-desktop

# 运行
./build/bin/falcon-desktop
```

## 开发规范

### 文件命名

- 页面: `xxx_page.{hpp,cpp}`
- 组件: `xxx.{hpp,cpp}`
- 对话框: `xxx_dialog.{hpp,cpp}`
- 服务: `xxx_service.{hpp,cpp}`

### 类命名

- 页面: `XxxPage`
- 组件: `XxxBar`/`XxxWidget`
- 对话框: `XxxDialog`
- 服务: `XxxService`

### 成员变量命名

使用下划线后缀：

```cpp
class DownloadPage : public QWidget {
private:
    QTableWidget* task_table_;
    QPushButton* new_task_button_;
    DownloadViewMode view_mode_;
};
```

## 未来计划

- [ ] 任务拖拽排序
- [ ] 下载队列管理
- [ ] 下载速度限制
- [ ] 计划任务（定时下载）
- [ ] 下载完成后操作（打开文件、关机等）
- [ ] 多语言支持（i18n）
