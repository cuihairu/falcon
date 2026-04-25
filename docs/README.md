# Falcon 文档

本目录使用 **VitePress** 构建项目文档，源码位于 `docs/src`。

> [!WARNING]
> `docs/src/` 是当前维护中的主文档来源。
> 根目录下的 `docs/*.md` 多数属于历史设计稿、迁移记录或归档材料，不应再被视为当前 API、CLI 或构建方式的权威说明。

归档文档索引见 `docs/ARCHIVE.md`。

## 快速开始

### 安装依赖

```bash
cd docs
pnpm install
```

### 开发模式

```bash
pnpm docs:dev
```

访问终端输出中的本地地址查看文档。

### 构建文档

```bash
pnpm docs:build
```

构建后的文件位于 `docs/src/.vitepress/dist/`。

## 文档结构

```
docs/
├── package.json              # pnpm / Node 配置
├── README.md                 # 本文件
├── ARCHIVE.md                # 根目录历史文档索引
├── src/                      # VitePress 源文件
│   ├── index.md              # 首页
│   ├── .vitepress/           # VitePress 配置
│   │   └── config.mts        # 主配置
│   ├── guide/                # 用户指南
│   │   ├── index.md          # 项目介绍
│   │   ├── getting-started.md # 快速开始
│   │   ├── installation.md    # 安装说明
│   │   └── usage.md          # 使用指南
│   ├── protocols/            # 协议文档
│   │   ├── index.md          # 协议概述
│   │   ├── http.md           # HTTP/HTTPS
│   │   ├── ftp.md            # FTP
│   │   ├── bittorrent.md     # BitTorrent
│   │   ├── private.md        # 私有协议
│   │   └── streaming.md      # HLS/DASH
│   ├── developer/            # 开发者文档
│   │   ├── index.md          # 开发者指南
│   │   ├── architecture.md   # 架构设计
│   │   ├── coding-standards.md # 编码规范
│   │   ├── testing.md        # 测试指南
│   │   ├── plugins.md        # 插件开发
│   │   ├── debugging.md      # 调试技巧
│   │   └── release.md        # 发布流程
│   ├── api/                  # API 参考
│   │   ├── index.md          # API 概述
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

## VitePress 组件

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
pnpm docs:build

# 推送到 gh-pages 分支
cd src/.vitepress/dist
git init
git add .
git commit -m "Deploy docs"
git push -f git@github.com:cuihairu/falcon.git master:gh-pages
```

### Vercel/Netlify

直接连接 GitHub 仓库，设置构建命令为：

```bash
cd docs && pnpm install --frozen-lockfile && pnpm docs:build
```

设置发布目录为 `docs/src/.vitepress/dist`。

### GitHub Actions

仓库已提供 GitHub Pages 部署工作流，推荐直接使用：

```bash
.github/workflows/docs-deploy.yml
```

文档校验工作流：

```bash
.github/workflows/docs-check.yml
```

默认行为：

- 推送到 `main` 时自动构建并部署
- 使用 `pnpm` 安装依赖
- 将 `docs/src/.vitepress/dist` 上传到 GitHub Pages
- 自动根据仓库名设置 VitePress `base`
- 在 PR / push 中独立执行文档构建校验

## 更多信息

- [VitePress 官方文档](https://vitepress.dev/)
