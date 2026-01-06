# Falcon 文档

本目录使用 **VuePress 2** 构建项目文档。

## 快速开始

### 安装依赖

```bash
cd docs
npm install
```

### 开发模式

```bash
npm run docs:dev
```

访问 http://localhost:8080 查看文档。

### 构建文档

```bash
npm run docs:build
```

构建后的文件位于 `src/.vuepress/dist/`。

## 文档结构

```
docs/
├── package.json              # npm 配置
├── README.md                 # 本文件
├── src/                      # VuePress 源文件
│   ├── README.md             # 首页
│   ├── .vuepress/            # VuePress 配置
│   │   ├── config.ts         # 主配置
│   │   ├── theme.ts          # 主题配置
│   │   ├── navbar.ts         # 导航栏
│   │   └── sidebar.ts        # 侧边栏
│   ├── guide/                # 用户指南
│   │   ├── README.md         # 项目介绍
│   │   ├── getting-started.md # 快速开始
│   │   ├── installation.md    # 安装说明
│   │   └── usage.md          # 使用指南
│   ├── protocols/            # 协议文档
│   │   ├── README.md         # 协议概述
│   │   ├── http.md           # HTTP/HTTPS
│   │   ├── ftp.md            # FTP
│   │   ├── bittorrent.md     # BitTorrent
│   │   ├── private.md        # 私有协议
│   │   └── streaming.md      # HLS/DASH
│   ├── developer/            # 开发者文档
│   │   ├── README.md         # 开发者指南
│   │   ├── architecture.md   # 架构设计
│   │   ├── coding-standards.md # 编码规范
│   │   ├── testing.md        # 测试指南
│   │   ├── plugins.md        # 插件开发
│   │   ├── debugging.md      # 调试技巧
│   │   └── release.md        # 发布流程
│   ├── api/                  # API 参考
│   │   ├── README.md         # API 概述
│   │   ├── core.md           # 核心 API
│   │   ├── plugins.md        # 插件 API
│   │   └── events.md         # 事件 API
│   └── faq.md                # 常见问题
└── [旧文档保留在根目录]
    ├── api_guide.md
    ├── developer_guide.md
    ├── protocols_guide.md
    └── ...
```

## VuePress 组件

### 提示框

```markdown
::: tip 提示
这是一个提示
:::

::: warning 注意
这是一个警告
:::

::: danger 危险
这是一个危险提示
:::
```

### 代码块

\`\`\`cpp
#include <iostream>
int main() {
    std::cout << "Hello, World!" << std::endl;
}
\`\`\`

### 自定义容器

\`\`\`md
::: details 点击查看详情
隐藏的内容
:::
\`\`\`

## 部署

### GitHub Pages

```bash
# 构建
npm run docs:build

# 推送到 gh-pages 分支
cd src/.vuepress/dist
git init
git add .
git commit -m "Deploy docs"
git push -f git@github.com:username/falcon.git master:gh-pages
```

### Vercel/Netlify

直接连接 GitHub 仓库，设置构建命令为：

```bash
cd docs && npm install && npm run docs:build
```

设置发布目录为 `docs/src/.vuepress/dist`。

## 更多信息

- [VuePress 官方文档](https://v2.vuepress.vuejs.org/)
- [默认主题文档](https://v2.vuepress.vuejs.org/reference/default-theme/)
