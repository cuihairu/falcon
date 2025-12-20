# Falcon 文档

本目录包含项目的完整文档。

---

## 目录结构

```
docs/
├── api/                # API 文档（Doxygen 生成）
├── user_guide.md       # 用户指南
├── developer_guide.md  # 开发者指南
└── decisions/          # 架构决策记录 (ADR)
    └── ADR-001-async-model.md
```

---

## 文档生成（Doxygen）

### 安装 Doxygen

```bash
# macOS
brew install doxygen graphviz

# Ubuntu/Debian
sudo apt install doxygen graphviz

# Windows
# 从 https://www.doxygen.nl/download.html 下载安装
```

### 生成 API 文档

```bash
# 在项目根目录运行
doxygen Doxyfile

# 生成的文档位于 docs/api/html/index.html
```

---

## 用户指南

参见 [user_guide.md](./user_guide.md)，包含：
- 安装与编译
- CLI 使用教程
- Daemon 部署指南
- 常见问题排查

---

## 开发者指南

参见 [developer_guide.md](./developer_guide.md)，包含：
- 项目架构说明
- 代码规范
- 插件开发指南
- 测试与调试

---

## 架构决策记录 (ADR)

重要的技术决策记录在 `decisions/` 目录下，格式参考：

```markdown
# ADR-001: 选择异步模型为 std::async

## 状态
已接受

## 背景
需要支持大规模并发下载任务...

## 决策
选择 std::async + std::future 作为异步基础...

## 后果
优点：标准库支持，无需第三方依赖
缺点：相比协程性能略低
```

---

**维护要求**：每次新增重要决策、修改接口、更新架构时，同步更新相关文档。
