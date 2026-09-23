# Vendored: libmetalink

- 上游: https://github.com/metalink-dev/libmetalink
- 版本: master @ `5bcdfc0`（2026-09 调研时上游 HEAD）
- 许可证: MIT（见 `COPYING`），与本项目 Apache-2.0 兼容
- 引入方式: 原样拷贝（`src/` 平面对应上游 `lib/`，`include/metalink/` 对应上游 `lib/includes/metalink/`），**零源码修改**
- 源码包: `/tmp/libmetalink.tar.gz`（76358 字节，SHA512 前缀 `c429e02265809d29f03ae34cc127c6cd`）
- 排除文件: `libxml2_metalink_parser.c`（使用 expat 版解析器）；`strptime.c`/`timegm.c` 仅在系统缺失对应函数时编译（见 CMakeLists 的 check_symbol_exists 探测）
- 平台适配: 等价上游 autoconf `config.h` 的编译定义注入（`HAVE_TIME`/`HAVE__MKGMTIME`），见 `CMakeLists.txt`
- 消费方: `packages/libfalcon-protocols` 的 metalink 插件（`FALCON_ENABLE_METALINK` 门控），
  经 `metalink_parse_memory` 做语义解析；XML 格式预检（行列号错误）与字段映射在 falcon 侧
  `MetalinkFileParser`（`plugins/metalink/metalink_handler.cpp`）
- 升级方式: 覆盖 `src/` 与 `include/` 后重跑本目录 CMakeLists 的探测逻辑；行为契约由
  `packages/libfalcon-protocols/tests/unit/metalink_parse_test.cpp` 与
  `metalink_handler_test.cpp` 的存量用例守卫
