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
├── src/                      # VuePress 源文件
│   ├── README.md             # 首页
│   ├── .vuepress/            # VuePress 配置
│   │   ├── config.ts         # 主配置
│   │   ├── theme.ts          # 主题配置
│   │   ├── navbar.ts         # 导航栏
│   │   └── sidebar.ts        # 侧边栏
│   ├── guide/                # 用户指南
│   ├── protocols/            # 协议文档
│   ├── developer/            # 开发者文档
│   ├── api/                  # API 参考
│   └── public/               # 静态资源
└── README.md                 # 本文件
```

## 添加新文档

### 添加指南文档

在 `src/guide/` 下创建 Markdown 文件，然后在 `src/.vuepress/sidebar.ts` 中添加链接。

### 添加协议文档

在 `src/protocols/` 下创建协议文档。

### 添加 API 文档

在 `src/api/` 下创建 API 文档。

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

## 旧文档

旧的 Markdown 文档保留在 `docs/` 根目录下，可以随时查阅。

## 更多信息

- [VuePress 官方文档](https://v2.vuepress.vuejs.org/)
- [默认主题文档](https://v2.vuepress.vuejs.org/reference/default-theme/)
