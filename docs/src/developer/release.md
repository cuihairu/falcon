# 发布流程

本文档介绍如何发布 Falcon 的新版本。

## 版本号规范

Falcon 遵循 [语义化版本](https://semver.org/lang/zh-CN/)：

```
MAJOR.MINOR.PATCH

1.0.0 - 初始稳定版本
1.1.0 - 添加新功能（向后兼容）
1.1.1 - Bug 修复
2.0.0 - 破坏性更改
```

### 版本类型

| 类型 | 说明 | 示例 |
|------|------|------|
| **Major** | 破坏性 API 变更 | 1.0.0 → 2.0.0 |
| **Minor** | 向后兼容的新功能 | 1.0.0 → 1.1.0 |
| **Patch** | 向后兼容的 Bug 修复 | 1.0.0 → 1.0.1 |

## 发布前检查清单

### 代码质量

- [ ] 所有测试通过
- [ ] 代码覆盖率达标
- [ ] 无编译警告
- [ ] 代码格式检查通过
- [ ] 静态分析通过

### 文档

- [ ] CHANGELOG 更新
- [ ] README 更新（如需要）
- [ ] API 文档更新
- [ ] 迁移指南（Major 版本）

### 功能

- [ ] 所有计划功能已实现
- [ ] 已知 bug 已修复
- [ ] 性能测试通过

## 发布步骤

### 1. 更新版本号

```cpp
// include/falcon/version.hpp
#define FALCON_VERSION_MAJOR 1
#define FALCON_VERSION_MINOR 1
#define FALCON_VERSION_PATCH 0
#define FALCON_VERSION "1.1.0"
```

### 2. 更新 CHANGELOG

```markdown
## [1.1.0] - 2025-01-06

### Added
- 添加对 DASH 协议的支持
- 添加下载速度限制功能
- 添加配置文件支持

### Changed
- 改进 HTTP 连接复用机制
- 更新最低 C++ 标准为 C++17

### Fixed
- 修复断点续传的文件大小计算问题
- 修复 FTP 主动模式连接失败的问题

### Security
- 修复潜在的 SSL 证书验证问题
```

### 3. 创建发布分支

```bash
# 从 main 创建 release 分支
git checkout -b release/1.1.0

# 更新版本号和 CHANGELOG
git add .
git commit -m "chore: 准备发布 1.1.0"
```

### 4. 测试发布版本

```bash
# 构建 Release 版本
cmake -B build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --config Release

# 运行测试
ctest --test-dir build-release --output-on-failure

# 测试 CLI 工具
./build-release/bin/falcon-cli --version
./build-release/bin/falcon-cli https://example.com/test.zip
```

### 5. 创建 Git 标签

```bash
# 创建 annotated 标签
git tag -a v1.1.0 -m "Release version 1.1.0"

# 推送标签
git push origin v1.1.0
```

### 6. 合并到 main

```bash
# 合并 release 分支到 main
git checkout main
git merge release/1.1.0

# 推送
git push origin main
```

### 7. 创建 GitHub Release

1. 访问 [GitHub Releases](https://github.com/yourusername/falcon/releases)
2. 点击 "Draft a new release"
3. 选择标签 `v1.1.0`
4. 填写发布标题：`Falcon 1.1.0`
5. 填写发布说明（从 CHANGELOG 复制）
6. 上传构建的发布包
7. 点击 "Publish release"

## 构建发布包

### Linux

```bash
# 使用 CPack
cmake --build build-release --target package

# 或手动打包
cd build-release
make package
```

### macOS

```bash
# 创建 DMG
cmake --build build-release --target package

# 或使用 macports
sudo port install falcon
```

### Windows

```bash
# 使用 CPack
cmake --build build-release --config Release --target package

# 或使用 WiX
candle falcon.wxs
light -out falcon.msi falcon.wixobj
```

## 持续集成

### GitHub Actions

```yaml
name: Release

on:
  push:
    tags:
      - 'v*'

jobs:
  release:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
        include:
          - os: ubuntu-latest
            artifact_name: falcon-linux-x64.tar.gz
          - os: macos-latest
            artifact_name: falcon-macos-x64.tar.gz
          - os: windows-latest
            artifact_name: falcon-windows-x64.zip

    steps:
    - uses: actions/checkout@v3

    - name: Install dependencies
      run: |
        ./vcpkg install spdlog nlohmann-json curl openssl

    - name: Configure CMake
      run: |
        cmake -B build -S . \
          -DCMAKE_BUILD_TYPE=Release \
          -DFALCON_BUILD_TESTS=ON \
          -DCMAKE_TOOLCHAIN_FILE=${{ github.workspace }}/vcpkg/scripts/buildsystems/vcpkg.cmake

    - name: Build
      run: cmake --build build --config Release

    - name: Run tests
      run: ctest --test-dir build --output-on-failure

    - name: Package
      run: cmake --build build --target package

    - name: Upload artifacts
      uses: actions/upload-artifact@v3
      with:
        name: ${{ matrix.artifact_name }}
        path: build/falcon-*
```

## 发布后

### 通知用户

- [ ] GitHub Release 发布
- [ ] 更新网站首页
- [ ] 发送邮件通知（如果有邮件列表）
- [ ] 在社交媒体发布

### 文档更新

- [ ] 更新网站文档
- [ ] 添加迁移指南（如果需要）
- [ ] 更新 API 文档

### 监控

- [ ] 监控 issue 和 PR
- [ ] 收集用户反馈
- [ ] 跟踪下载统计

## 回滚计划

如果发现严重问题：

```bash
# 回滚标签
git tag -d v1.1.0
git push origin :refs/tags/v1.1.0

# 删除 GitHub Release
# （在 GitHub 网页上操作）

# 发布修复版本
# 1.1.1 → 修复补丁
```

::: tip 发布建议
- 在非工作时间发布，以便有时间处理紧急问题
- 提前通知用户即将发布的版本
- 准备好快速修复计划
:::
