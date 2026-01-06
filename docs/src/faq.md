# 常见问题

本文档汇总了 Falcon 的常见问题和解答。

## 安装问题

### Q: 如何安装 Falcon？

A: 有多种安装方式：

```bash
# macOS
brew install falcon

# Ubuntu/Debian
sudo apt install falcon-downloader

# 从源码编译
git clone https://github.com/yourusername/falcon.git
cd falcon
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

详见 [安装说明](./guide/installation.md)。

### Q: 编译时找不到依赖？

A: 确保已安装所有依赖：

```bash
# macOS
brew install spdlog nlohmann-json curl openssl googletest

# Ubuntu/Debian
sudo apt install libspdlog-dev nlohmann-json3-dev \
  libcurl4-openssl-dev libssl-dev libgtest-dev
```

### Q: 如何查看支持的协议？

A: 运行以下命令：

```bash
falcon-cli --list-protocols
```

## 使用问题

### Q: 如何下载文件？

A: 基本用法：

```bash
falcon-cli https://example.com/file.zip
```

### Q: 如何使用多线程下载？

A: 使用 `-c` 选项：

```bash
falcon-cli https://example.com/large.zip -c 5
```

### Q: 断点续传不工作？

A: 确保：
1. 服务器支持 Range 请求
2. 使用 `--continue` 选项
3. 文件未被修改

### Q: 如何限制下载速度？

A: 使用 `--limit` 选项：

```bash
falcon-cli https://example.com/file.zip --limit 1M
```

### Q: 如何使用代理？

A: 使用 `--proxy` 选项：

```bash
falcon-cli https://example.com/file.zip --proxy http://proxy:8080
```

## 协议问题

### Q: 支持哪些协议？

A: Falcon 支持：
- HTTP/HTTPS
- FTP/FTPS
- BitTorrent (Magnet, .torrent)
- ED2K
- 迅雷 (thunder://)
- QQ旋风 (qqlink://)
- 快车 (flashget://)
- HLS/DASH

详见 [协议支持](./protocols/README.md)。

### Q: 如何下载迅雷链接？

A: 直接使用迅雷链接：

```bash
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="
```

### Q: Magnet 链接下载很慢？

A: 可能的原因：
1. 没有 peers：添加更多 tracker
2. 防火墙阻止：开放端口 6881
3. 启用 UPnP 或手动端口映射

### Q: HLS 下载失败？

A: 确保：
1. FFmpeg 已安装
2. 播放列表 URL 有效
3. 如果是加密流，提供密钥

## 配置问题

### Q: 配置文件在哪里？

A: 配置文件位置：

- **Linux/macOS**: `~/.config/falcon/config.json`
- **Windows**: `%APPDATA%\falcon\config.json`

### Q: 如何修改默认下载目录？

A: 编辑配置文件：

```json
{
  "default_download_dir": "~/Downloads"
}
```

### Q: 如何设置全局代理？

A: 编辑配置文件：

```json
{
  "proxy": "http://proxy:8080"
}
```

## 错误排查

### Q: 下载失败怎么办？

A: 检查：
1. 网络连接是否正常
2. URL 是否有效
3. 使用 `--verbose` 查看详细日志

### Q: SSL 证书验证失败？

A: 临时解决方案（不推荐生产环境）：

```json
{
  "verify_ssl": false
}
```

### Q: 磁盘空间不足？

A: 检查磁盘空间：

```bash
df -h
```

或更改下载目录：

```bash
falcon-cli <URL> -d /path/to/larger/disk
```

### Q: 权限被拒绝？

A: 检查文件权限：

```bash
# 检查目标目录权限
ls -la ~/Downloads

# 修改权限
chmod u+w ~/Downloads
```

## 性能问题

### Q: 如何提高下载速度？

A: 尝试：
1. 增加连接数：`-c 5`
2. 使用更快的网络
3. 更换 CDN 或镜像
4. 关闭其他占用带宽的程序

### Q: Falcon 占用内存太多？

A: 检查：
1. 同时下载的任务数
2. 单个任务的最大连接数
3. 是否有内存泄漏

## 开发问题

### Q: 如何贡献代码？

A: 请查看 [开发者指南](./developer/README.md)。

### Q: 如何开发新插件？

A: 请查看 [插件开发指南](./developer/plugins.md)。

### Q: 如何运行测试？

A:

```bash
# 编译测试
cmake -B build -S . -DFALCON_BUILD_TESTS=ON
cmake --build build

# 运行测试
ctest --test-dir build --output-on-failure
```

## 其他问题

### Q: Falcon 是开源的吗？

A: 是的，基于 Apache 2.0 许可证开源。

### Q: Falcon 与 aria2 相比如何？

A: 请查看 [功能对比](./developer/README.md)。

### Q: 如何获取帮助？

A:
- 查看 [文档](./)
- 在 [GitHub Issues](https://github.com/yourusername/falcon/issues) 提问
- 加入 [Discussions](https://github.com/yourusername/falcon/discussions)

::: tip 仍有问题？
请在 [GitHub Issues](https://github.com/yourusername/falcon/issues) 中提问，我们很乐意帮助！
:::
