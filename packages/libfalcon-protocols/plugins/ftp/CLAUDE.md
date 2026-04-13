[根目录](../../../../CLAUDE.md) > [packages](../../../) > [libfalcon](../../CLAUDE.md) > [plugins](../) > **ftp**

---

# FTP Plugin

## 变更记录 (Changelog)

### 2025-12-21 - 初始化插件架构
- 创建 FTP 插件目录结构

---

## 插件职责

FTP 协议插件，基于 **libcurl** 实现，支持：

- FTP/FTPS 基础下载
- 断点续传（FTP REST 命令）
- 被动模式（PASV）
- 用户认证

---

## 依赖

- **libcurl** 7.68+（需启用 FTP 支持）

---

## 下一步

1. 实现 `FTPPlugin` 类
2. 封装 libcurl FTP API
3. 支持 REST 命令（断点续传）
4. 编写单元测试

---

**文档维护**：实现完成后更新本文档。
