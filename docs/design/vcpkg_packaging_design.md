[中文](vcpkg_packaging_design.zh.md) | **English**

# vcpkg Package Split Design (todo Phase 5 #13)

Status: **Stage 1 implemented** (2026-10-13: four-package standalone package
config, 13433cf core / 05e44fc protocols / 75752e9 storage / 6ea88ae drives;
acceptance smoke green on both consumption paths — four packages configured
standalone with a single consumer project, plus the monolithic
`find_package(Falcon)` in the same project shape). Stage 2 overlay ports are
queued. The full design and acceptance clauses are at the end of the
document. Prerequisite: the #12 install-export closeout (2cd0dc2) already
made `find_package(Falcon)` single-package consumption usable end to end;
this design splits that capability down to vcpkg package granularity.

## 1. Goals and Non-Goals

**Goal**: external vcpkg projects install Falcon at library granularity:

```bash
vcpkg install --overlay-ports=<repo>/ports libfalcon-core
vcpkg install --overlay-ports=<repo>/ports "libfalcon-protocols[bittorrent,sftp]"
vcpkg install --overlay-ports=<repo>/ports "libfalcon-storage[s3,oss]"
```

**Non-goals**:
- No ports for the CLI / daemon / swarmd / desktop applications —
  applications keep building from repository sources; if a `falcon` tool
  port is ever needed it will be a separate effort (application ports and
  library ports differ completely in version policy and feature matrix;
  mixing them only drags both down);
- No changes to the #12 monolithic `Falcon` install export — "install from
  the source tree, then find_package(Falcon)" is an already-accepted
  consumption path and stays usable;
- No submission to the upstream vcpkg registry — the first version ships as
  overlay ports (held by this repository); upstreaming is an optional
  stage 3 item (it requires version freezing and an upstream review
  cadence, neither of which this design governs).

## 2. Current State

### 2.1 Dependency Matrix (Build-Time Reality)

| Package | Public dependencies (export closure) | Private dependencies (plugin/optional surface) |
|----|--------------------|----------------------|
| core | Threads (required); spdlog / nlohmann-json / OpenSSL (optional, QUIET degradation when missing) | ws2_32 (Windows) |
| protocols | falcon_core | curl (HTTP/FTP/WebDAV plugins); OpenSSL (TLS/metalink hashing); libssh2 (SFTP); libtorrent (BT plugin); expat + vendored libmetalink (metalink); miniupnpc + libnatpmp (NAT facade) |
| storage | falcon_core; curl + nlohmann-json (resource browser); OpenSSL (signing/hashing) | — |
| drives | falcon_core; nlohmann-json; curl (search crawler); SQLite3 (config_manager); OpenSSL (encryption/decryption) | — |

### 2.2 Three Root-Context Dependencies for Standalone Configure

All four package CMakeLists already carry `cmake_minimum_required` +
`project()`, so standalone configure is structurally feasible; what is
actually missing falls into three classes of root-provided inputs:

1. **The `falcon_core` target**: protocols/storage/drives link by name;
   inside the monorepo tree it is provided by the root `add_subdirectory`;
   under standalone configure it must come from the installed `FalconCore`
   package (`find_package(FalconCore CONFIG)`);
2. **`FALCON_ENABLE_*` options**: all defined in the root CMakeLists;
   references inside packages are not guaranteed a definition (under
   standalone configure `if(FALCON_ENABLE_SFTP)` is always false);
3. **finder result variables**: three packages consume `CURL_FOUND` /
   `nlohmann_json_FOUND` directly without doing their own `find_package`
   (root finds, subpackage reads) — under standalone configure these are
   always undefined.

### 2.3 Existing Mechanisms Available for Reuse

- **overlay-ports already in place**: `vcpkg-configuration.json` already
  mounts `ports/` (the libnatpmp precedent); adding ports requires zero
  configuration changes;
- **`FALCON_PACKAGE_NEEDS_*` CACHE INTERNAL markers** (introduced by #12):
  the single source of truth for the dependency closure lives at "the
  CMakeLists link point that creates the package" — after the vcpkg split,
  each package Config's find_dependency closure must evolve in lockstep
  with the monolithic FalconConfig; reusing the same marker variables is
  the convergence mechanism (see §4.3);
- **Per-package `EXPORT_NAME` already matches the alias** (Falcon::core
  etc., closed out in #12); the exported target names referenced across
  packages do not change after the split.

## 3. Ports and Feature Matrix

Port names follow the todo candidates (vcpkg lowercase-hyphen convention):

| Port | CMake package name | features | → build switches |
|------|-----------|---------|-----------|
| `libfalcon-core` | `FalconCore` | none (optional dependencies auto-degrade per registry reality) | — |
| `libfalcon-protocols` | `FalconProtocols` | `http` / `ftp` / `bittorrent` / `sftp` / `webdav` / `metalink` / `nat` | `FALCON_ENABLE_HTTP/FTP/BITTORRENT/SFTP/WEBDAV/METALINK/NAT` |
| `libfalcon-storage` | `FalconStorage` | `s3` / `oss` / `cos` / `kodo` / `upyun` | **new per-provider options required** (see §5.2) |
| `libfalcon-drives` | `FalconDrives` | no features in the first version (baidu/aliyundrive/115/quark/pikpak candidates unimplemented; the todo netdisk direct-link resolution item still awaits the user naming targets; map each batch as it lands) | — |

Three design rulings:

1. **protocols default feature = `http`** (the minimal usable download
   surface; bittorrent's heavy build cost (boost+libtorrent source build
   time) stays out of the default);
2. **drives ships zero features in the first version**: vcpkg allows
   features with no dependencies, but a feature with no code behind it is
   a false promise — the candidate table stays in todo, and each batch of
   implementations maps a batch of features;
3. **the behavior contract with a feature off matches the build tree**:
   with `FALCON_ENABLE_X=OFF` the package still compiles and the
   corresponding handler is not registered (runtime can_handle reports
   unsupported) — the ports do no "trim the source files" level splitting;
   granularity stops at the option.

## 4. Stage 1: Per-Package CMake Self-Sufficiency + Per-Package Export

### 4.1 Package-Header Self-Sufficiency Block (Decision Anchor = TARGET Existence)

Add an identically-shaped block at the head of each package's CMakeLists
(protocols shown as an example):

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

Key points:

- **The decision anchor is `TARGET falcon_core`, not a new switch
  variable** — inside the monorepo tree the root has necessarily already
  created the target, and in a port tree it necessarily does not exist; no
  second source of truth is introduced;
- Consumption points inside packages that read `CURL_FOUND` and similar
  variables stay unchanged (once the self-sufficiency block adds the
  find_package calls, the variables are naturally in place);
- The `FALCON_ENABLE_*` options either move into each package (protocols'
  7, storage's provider-split set, drives' 4) or get
  `if(NOT DEFINED ...)` defaults in the package header — **the latter is
  chosen** (option definition authority stays with the root directory;
  packages only fill in defaults when undefined, avoiding drift between
  the root and package option descriptions);
- third_party/libmetalink's relative path `../../third_party/libmetalink`
  still resolves in the port shape (full-tree checkout, SOURCE_PATH
  pointing at the package directory; the relative path still points back
  into the same checkout) — no relocation needed.

### 4.2 Per-Package Export and Config

Each package gains its own export set and config template:

```
packages/libfalcon-core/cmake/FalconCoreConfig.cmake.in
packages/libfalcon-protocols/cmake/FalconProtocolsConfig.cmake.in
...
install(TARGETS falcon_protocols EXPORT FalconProtocolsTargets ...)
install(EXPORT FalconProtocolsTargets FILE FalconProtocolsTargets.cmake
        NAMESPACE Falcon:: DESTINATION share/FalconProtocols)
```

Cross-package reference chain (`$<LINK_ONLY:Falcon::core>` and friends
inside the exported interfaces are resolved by the consumer's
`find_package(FalconCore)`):

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

### 4.3 Single-Source Convergence of the Monolithic Falcon and the Four-Package Configs

The #12 monolithic `FalconConfig` **stays as is** (the source-tree
consumer path); its find_dependency closure and the four package Configs'
closures are both generated from the `FALCON_PACKAGE_NEEDS_*` marker
variables — one source of truth, two projections, with the drift surface
collapsed to the marking points. Stage 1 acceptance runs both consumption
paths (monolithic + single FalconCore package) to guard the split against
regressing #12.

(The alternative — "turn the aggregate Config into a thin shell of four
find_dependency calls" — was rejected: installing all four configs under
one install prefix would mean source-tree installs must enable all four
packages, conflicting with trimmed installs like `FALCON_BUILD_CLI=OFF`
installing only core; the dual track preserves trimming freedom.)

## 5. Stage 2: Overlay Ports

### 5.1 Port File Shape

```
ports/libfalcon-core/{vcpkg.json, portfile.cmake, usage}
ports/libfalcon-protocols/{vcpkg.json, portfile.cmake, usage}
ports/libfalcon-storage/{vcpkg.json, portfile.cmake, usage}
ports/libfalcon-drives/{vcpkg.json, portfile.cmake, usage}
```

portfile skeleton (protocols as an example):

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

Dependencies come from public ports in the vcpkg registry
(curl/openssl/libtorrent/expat/miniupnpc/libnatpmp/libssh2/sqlite3/spdlog/
nlohmann-json); libnatpmp has no upstream port, so the repository overlay
(`ports/libnatpmp`) keeps being used — the protocols port's vcpkg.json
simply declares an intra-overlay dependency on it.

### 5.2 storage per-provider option (small stage-1 prerequisite change)

Today storage only has two granularity tiers,
`FALCON_ENABLE_RESOURCE_BROWSER` / `FALCON_ENABLE_CLOUD_STORAGE`;
feature-level splitting requires five new per-provider options
(`FALCON_ENABLE_STORAGE_S3/OSS/COS/KODO/UPYUN`, default ON to keep current
behavior, degrading isomorphically with the existing WARNING fallback
logic when OpenSSL is missing). Browser objects and mock tests get grouped
per provider behind the option gates. **drives' config_manager dependency
on SQLite3 is package-level required** (feature splitting does not touch
it).

### 5.3 Version Policy

**Start with `version-date` (YYYY-MM-DD)**: a rolling delivery surface,
same philosophy as the nightly binaries; the port REF pins a commit and
users pick by date. Once stable, if the user decides on version-semver,
migrate then (vcpkg forbids version downgrades; a date→semver migration
needs a `version>=` baseline agreement — handled together in stage 3).

Relation to the version-release red line: a vcpkg port version is not a
GitHub Release — no tags, no Releases, no release workflows; port file
and REF advancement are ordinary commits and never touch the red line.

## 6. Stage 3 (Optional, Unscheduled by Default): Upstreaming Assessment

Trigger conditions: the overlay shape running stably + feedback from
external consumers. An upstream submission requires: upstream port-name
review (the `libfalcon-*` prefix), license/version-freeze cooperation, and
three-platform port smoke tests in CI. This design only leaves the
decision note and does no preparatory work.

## 7. Acceptance Criteria (Against the Three todo Items)

1. **Package dependencies are clear**: the four ports' vcpkg.json
   dependency declarations correspond one-to-one with the §2.1 matrix;
   the usage docs state each package's `find_package` names and feature
   table;
2. **The core library installs standalone**: `vcpkg install
   --overlay-ports=ports libfalcon-core` succeeds in a clean manifest
   project, and the consumer project links only `Falcon::core`, compiles
   and runs (download engine smoke);
3. **storage/protocols by feature**: after installing
   `libfalcon-storage[s3]`, only the s3 browser objects are in the library
   (checked via nm/linking); after installing
   `libfalcon-protocols[bittorrent]`, the
   `Falcon::falcon_plugin_bittorrent` target is in place and handlers
   whose features were not selected are not registered (runtime
   can_handle negative smoke).

Each stage's acceptance = full ctest regression green for the changes
within the stage + green smoke on that stage's consumption path (stage 1:
monolithic + FalconCore dual path; stage 2: four-port install + consumer
project).

## 8. Risks and Boundaries (Disclosed As-Is)

- **protocols' SFTP/BT third-party lookup logic is duplicated**: the
  self-sufficiency block must copy the root's "CONFIG first + pkg-config
  fallback" two-stage lookup — together with the FalconConfig consumer
  lookup that makes three same-family copies; the convergence tool is the
  marker variables as the single source of truth plus cross-referencing
  comments at all three projections (no shared cmake module abstraction —
  under the vcpkg port shape include paths are not controllable, and
  explicit duplication beats implicit coupling);
- **The OBJECT library falcon_builtin_protocol_handlers**: it is archived
  into falcon_protocols (same CMakeLists), so the port split has zero
  impact; the weak/strong symbol caveats (batch F lesson) travel with the
  package unchanged;
- **Header forwarding shims** (the 24 old paths): they continue to install
  via each package's DIRECTORY install rules; the BT shim
  dangling-exclusion logic (the #12 mtime lesson) is preserved verbatim in
  the FalconProtocols export;
- **Dual-track maintenance cost**: the monolithic Falcon and the four
  package Configs coexist, so install rules and closure markers must
  evolve in sync — the acceptance clause pins "dual-path smoke" into the
  stage 1 gate to prevent one-sided rot.
