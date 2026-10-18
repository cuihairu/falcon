[English](vcpkg_packaging_design.md) | **中文**

# vcpkg 包拆分设计（todo 第五阶段 #13）

状态：**阶段 1 已实施**（2026-10-13：四包 standalone package config，
13433cf core / 05e44fc protocols / 75752e9 storage / 6ea88ae drives；
验收双路径冒烟绿——四包独立 configure 单消费工程 + 单体
`find_package(Falcon)` 同工程形态）。阶段 2 overlay 端口排队中。
设计全貌与验收条款见文末。前置成果：#12 安装导出收口（2cd0dc2）已让
`find_package(Falcon)` 单包消费端到端可用，本设计把该能力拆到 vcpkg
包粒度。

## 1. 目标与非目标

**目标**：外部 vcpkg 工程按库粒度安装 Falcon：

```bash
vcpkg install --overlay-ports=<repo>/ports libfalcon-core
vcpkg install --overlay-ports=<repo>/ports "libfalcon-protocols[bittorrent,sftp]"
vcpkg install --overlay-ports=<repo>/ports "libfalcon-storage[s3,oss]"
```

**非目标**：
- CLI / daemon / swarmd / desktop 应用不打端口——应用随仓库源码构建；
  未来若需 `falcon` 工具端口另行立项（应用端口与库端口的版本策略、
  feature 矩阵完全不同，混在一起只会互相拖累）；
- 不改动 #12 的单体 `Falcon` 安装导出——「从源码树 install 后
  find_package(Falcon)」是既有验收过的消费路径，保持可用；
- 不向上游 vcpkg 注册表提交——首版走 overlay 端口（仓库自持），上游化
  属阶段 3 可选项（需要版本冻结与 upstream 审核节奏，不由本设计约定）。

## 2. 现状盘点

### 2.1 依赖矩阵（构建期实态）

| 包 | 公共依赖（导出闭包） | 私有依赖（插件/可选面） |
|----|--------------------|----------------------|
| core | Threads（必需）；spdlog / nlohmann-json / OpenSSL（可选，QUIET 缺失降级） | ws2_32（Windows） |
| protocols | falcon_core | curl（HTTP/FTP/WebDAV 插件）；OpenSSL（TLS/metalink 哈希）；libssh2（SFTP）；libtorrent（BT 插件）；expat + vendored libmetalink（metalink）；miniupnpc + libnatpmp（NAT 门面） |
| storage | falcon_core；curl + nlohmann-json（resource browser）；OpenSSL（签名/哈希） | — |
| drives | falcon_core；nlohmann-json；curl（搜索 crawler）；SQLite3（config_manager）；OpenSSL（加解密） | — |

### 2.2 独立 configure 的三处根上下文依赖

四包 CMakeLists 均已自带 `cmake_minimum_required` + `project()`，独立
configure 结构上可行；真正缺的是三类根注入物：

1. **`falcon_core` 目标**：protocols/storage/drives 按名链接，monorepo
   树内由根 `add_subdirectory` 提供；独立 configure 时须改从安装的
   `FalconCore` 包拿（`find_package(FalconCore CONFIG)`）；
2. **`FALCON_ENABLE_*` option**：全部定义在根 CMakeLists；包内引用的
   未必有定义（独立 configure 下 `if(FALCON_ENABLE_SFTP)` 恒假）；
3. **finder 结果变量**：三包直接消费 `CURL_FOUND` /
   `nlohmann_json_FOUND` 而不做自己的 `find_package`（根查找、子包
   读取）——独立 configure 下恒未定义。

### 2.3 既有机制复用面

- **overlay-ports 在位**：`vcpkg-configuration.json` 已挂 `ports/`
  （libnatpmp 先例），新增端口零配置面变更；
- **`FALCON_PACKAGE_NEEDS_*` CACHE INTERNAL 打标**（#12 引入）：依赖
  闭包的单一事实源在「创建包的 CMakeLists 链接点」——vcpkg 拆分后每包
  Config 的 find_dependency 闭包必须与单体 FalconConfig 同源演进，
  复用同一套标志变量是收敛机制（见 §4.3）；
- **每包 `EXPORT_NAME` 已与别名一致**（Falcon::core 等，#12 收口），
  跨包引用的导出目标名拆分后不变。

## 3. 端口与 feature 矩阵

端口名沿 todo 候选（vcpkg 小写连字符惯例）：

| 端口 | CMake 包名 | feature | → 构建开关 |
|------|-----------|---------|-----------|
| `libfalcon-core` | `FalconCore` | 无（可选依赖按 registry 实态自动降级） | — |
| `libfalcon-protocols` | `FalconProtocols` | `http` / `ftp` / `bittorrent` / `sftp` / `webdav` / `metalink` / `nat` | `FALCON_ENABLE_HTTP/FTP/BITTORRENT/SFTP/WEBDAV/METALINK/NAT` |
| `libfalcon-storage` | `FalconStorage` | `s3` / `oss` / `cos` / `kodo` / `upyun` | **需新增** per-provider option（见 §5.2） |
| `libfalcon-drives` | `FalconDrives` | 首版无 feature（baidu/aliyundrive/115/quark/pikpak 候选未实现，todo 网盘直链解析仍待用户点名目标；实现后映射） | — |

设计裁定三条：

1. **protocols 默认 feature = `http`**（最小可用下载面；bittorrent 重
   （boost+libtorrent 源码构建时长）不进默认）；
2. **drives 首版零 feature**：vcpkg 允许无依赖 feature，但没有代码对应
   的 feature 是虚假承诺——候选表保留在 todo，实现一批映射一批；
3. **feature 关闭时的行为契约与构建树一致**：`FALCON_ENABLE_X=OFF`
   时包照常编译、对应 handler 不注册（运行时 can_handle 返回不支持）
   ——端口不做「裁掉源文件」级别的拆分，粒度到 option 为止。

## 4. 阶段 1：CMake 每包自足化 + 每包导出

### 4.1 包头自足块（判定锚 = TARGET 存在性）

每包 CMakeLists 头部加同形块（以 protocols 为例）：

```cmake
# 独立 configure（vcpkg 端口）时根目录未提供依赖 finder 与上游包，
# 此处自足；monorepo 树内 falcon_core 已由根 add_subdirectory 创建，
# 整块跳过（零行为变化）
if(NOT TARGET falcon_core)
    find_package(FalconCore CONFIG REQUIRED)
    find_package(Curl QUIET)
    find_package(OpenSSL QUIET)
    find_package(libssh2 CONFIG QUIET)
    # ...（与根目录同款查找逻辑，含 pkg-config 兜底）
endif()
```

要点：
- **判定锚用 `TARGET falcon_core` 而非新开关变量**——monorepo 树内
  根必然已建该目标，端口树内必然没有；不引入第二个真值来源；
- 包内对 `CURL_FOUND` 等变量的消费点不改（自足块补上 find_package 后
  变量自然在位）；
- `FALCON_ENABLE_*` option 移入各包（protocols 的 7 枚、storage 的
  provider 拆分枚、drives 的 4 枚）或包头 `if(NOT DEFINED ...)` 补
  默认值——**选后者**（option 定义权留根目录，包内只在未定义时兜底，
  避免根与包两处 option 描述漂移）；
- third_party/libmetalink 的相对路径 `../../third_party/libmetalink`
  在端口形态下解析成立（checkout 整树、SOURCE_PATH 指包目录，相对
  路径仍指回同一 checkout）——无需搬运。

### 4.2 每包导出与 Config

每包新增自己的 export set 与 config 模板：

```
packages/libfalcon-core/cmake/FalconCoreConfig.cmake.in
packages/libfalcon-protocols/cmake/FalconProtocolsConfig.cmake.in
...
install(TARGETS falcon_protocols EXPORT FalconProtocolsTargets ...)
install(EXPORT FalconProtocolsTargets FILE FalconProtocolsTargets.cmake
        NAMESPACE Falcon:: DESTINATION share/FalconProtocols)
```

跨包引用链（导出接口里的 `$<LINK_ONLY:Falcon::core>` 等由消费方的
`find_package(FalconCore)` 解析）：

```
FalconProtocolsConfig.cmake
  → find_dependency(FalconCore)     # 上游包
  → find_dependency(CURL/OpenSSL)   # 自身公共闭包（按打标条件）
FalconStorageConfig.cmake
  → find_dependency(FalconCore)
  → find_dependency(CURL nlohmann_json OpenSSL)
FalconDrivesConfig.cmake
  → find_dependency(FalconCore)
  → find_dependency(SQLite3 CURL OpenSSL nlohmann_json)
```

### 4.3 单体 Falcon 与四包 Config 的同源收敛

#12 的单体 `FalconConfig` **保持原样**（源码消费方路径），其
find_dependency 闭包与四包 Config 的闭包共用 `FALCON_PACKAGE_NEEDS_*`
打标变量生成——同一事实源两份投影，漂移面收敛到打标点。阶段 1 验收
同时跑两条消费路径（单体 + FalconCore 单包），防拆分回归 #12。

（备选「聚合 Config 薄壳化为 find_dependency 四连」被否：install 前
缀同时装四份 config 意味着源码 install 场景必须四包全开，与
「FALCON_BUILD_CLI=OFF 只装 core」的裁剪安装冲突；双轨保留裁剪自由。）

## 5. 阶段 2：overlay 端口

### 5.1 端口文件形态

```
ports/libfalcon-core/{vcpkg.json, portfile.cmake, usage}
ports/libfalcon-protocols/{vcpkg.json, portfile.cmake, usage}
ports/libfalcon-storage/{vcpkg.json, portfile.cmake, usage}
ports/libfalcon-drives/{vcpkg.json, portfile.cmake, usage}
```

portfile 骨架（以 protocols 为例）：

```cmake
vcpkg_from_git(OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/cuihairu/falcon
    REF <锁定 commit>          # 手动随版本推进（见 5.3）
)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/packages/libfalcon-protocols"
    OPTIONS
        -DFALCON_BUILD_TESTS=OFF
        ${FEATURE_OPTIONS}     # FALCON_ENABLE_* 按 feature 映射
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME FalconProtocols)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION ...)
```

依赖走 vcpkg registry 公共端口（curl/openssl/libtorrent/expat/
miniupnpc/libnatpmp/libssh2/sqlite3/spdlog/nlohmann-json）；libnatpmp
无上游端口，继续用仓库 overlay（`ports/libnatpmp`）——protocols 端口
的 vcpkg.json 对它声明 overlay 内部依赖即可。

### 5.2 storage per-provider option（阶段 1 前置小改动）

现状 storage 只有 `FALCON_ENABLE_RESOURCE_BROWSER` /
`FALCON_ENABLE_CLOUD_STORAGE` 两档粒度；feature 级拆分需新增五枚
per-provider option（`FALCON_ENABLE_STORAGE_S3/OSS/COS/KODO/UPYUN`，
默认 ON 保持现行为，缺 OpenSSL 时与现 WHING 降级逻辑同构）。浏览器
对象与 mock 测试按 provider 归组挂 option 门禁。**drives 的 config_
manager 依赖 SQLite3 属包级必需**（feature 拆分不涉及）。

### 5.3 版本策略

**`version-date`（YYYY-MM-DD）起步**：滚动交付面，与 nightly 二进制
同哲学；端口 REF 锁 commit，用户按日期挑。稳定后若用户拍板
version-semver 再迁移（vcpkg 禁止 version 回退，date→semver 迁移需
`version>=` 基线约定，阶段 3 一并处理）。

与版本发布红线的关系：vcpkg 端口版本不是 GitHub Release——不打
tag、不发 Release、不跑发版 workflow，端口文件与 REF 推进属普通
commit，红线不触碰。

## 6. 阶段 3（可选，默认不排期）：上游化评估

触发条件：overlay 形态稳定运行 + 有外部消费方反馈。上游提交需要：
上游端口命名审核（`libfalcon-*` 前缀）、license/版本冻结配合、
CI 三平台端口冒烟。本设计只留判定注记，不预做。

## 7. 验收标准（对 todo 三条）

1. **包依赖清晰**：四端口 vcpkg.json 依赖声明与 §2.1 矩阵一一对应；
   usage 文档写明各自 `find_package` 名与 feature 表；
2. **核心库可单独安装**：`vcpkg install --overlay-ports=ports
   libfalcon-core` 在干净 manifest 工程成功，消费工程仅链
   `Falcon::core` 编译运行过（下载引擎冒烟）；
3. **storage/protocols 按 feature**：`libfalcon-storage[s3]` 安装后
   只有 s3 浏览器对象在库（nm/链接核对）；`libfalcon-protocols[bittorrent]`
   安装后 `Falcon::falcon_plugin_bittorrent` 目标在位、未选 feature
   的 handler 不注册（运行时 can_handle 负向冒烟）。

每阶段验收 = 阶段内改动全量 ctest 回归绿 + 本阶段消费路径冒烟绿
（阶段 1：单体 + FalconCore 双路径；阶段 2：四端口 install + 消费
工程）。

## 8. 风险与边界（如实披露）

- **protocols 的 SFTP/BT 第三方查找逻辑重复**：自足块需复制根目录的
  「CONFIG 优先 + pkg-config 兜底」两段式查找——与 FalconConfig 的
  消费方查找是三份同族逻辑；收敛手段是打标变量单一事实源 + 三处投影
  注释互指（不抽象共享 cmake 模块——vcpkg 端口形态下 include 路径
  不可控，显式重复优于隐式耦合）；
- **OBJECT 库 falcon_builtin_protocol_handlers**：归档内嵌进
  falcon_protocols（同一 CMakeLists），端口拆分零影响；weak/strong
  符号注意事项（批次 F 教训）随包走不变；
- **头文件转发 shim**（24 枚旧路径）：随各包 DIRECTORY 安装规则继续
  安装；BT shim 悬空排除逻辑（#12 mtime 教训）在 FalconProtocols
  导出中原样保留；
- **双轨维护成本**：单体 Falcon 与四包 Config 并存，安装规则与闭包
  打标点必须同步演进——验收条款把「双路径冒烟」钉进阶段 1 门禁防
  单侧腐化。
