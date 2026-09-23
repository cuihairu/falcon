# Vendored libmetalink（上游 metalink-dev/libmetalink @ 5bcdfc0，MIT）。
# 原样拷贝零修改：src/ 平面结构（上游 lib/）、include/metalink 公共头、COPYING。
# 平台适配经由编译定义注入（等价上游 autoconf 的 config.h，见 src/metalink_config.h）：
#   HAVE_STRPTIME / HAVE_TIMEGM —— 系统有则不编自带替代品（strptime.c/timegm.c）
#   HAVE_TIME 恒有；HAVE__MKGMTIME 供 timegm.c 在 MSVC 上选 _mkgmtime

include(CheckSymbolExists)

# 根 project() 只启 CXX；vendored 库是纯 C，局部启用 C 语言
enable_language(C)

check_symbol_exists(strptime "time.h" METALINK_HAVE_STRPTIME)
if(NOT METALINK_HAVE_STRPTIME)
    # 非 glibc 平台可能在 string.h（POSIX 未规定声明位置，
    # metalink_config.h 的 _XOPEN_SOURCE 已按 config.h 惯例定义）
    check_symbol_exists(strptime "string.h" METALINK_HAVE_STRPTIME_SH)
    set(METALINK_HAVE_STRPTIME ${METALINK_HAVE_STRPTIME_SH})
endif()
check_symbol_exists(timegm "time.h" METALINK_HAVE_TIMEGM)

set(libmetalink_SOURCES
    src/metalink_helper.c
    src/metalink_list.c
    src/metalink_parser_common.c
    src/metalink_pctrl.c
    src/metalink_pstate.c
    src/metalink_pstate_v3.c
    src/metalink_pstate_v4.c
    src/metalink_pstm.c
    src/metalink_session_data.c
    src/metalink_stack.c
    src/metalink_string_buffer.c
    src/metalink_types.c
    src/libexpat_metalink_parser.c
)
if(NOT METALINK_HAVE_STRPTIME)
    list(APPEND libmetalink_SOURCES src/strptime.c)
endif()
if(NOT METALINK_HAVE_TIMEGM)
    list(APPEND libmetalink_SOURCES src/timegm.c)
endif()

add_library(libmetalink STATIC ${libmetalink_SOURCES})
add_library(Metalink::libmetalink ALIAS libmetalink)

target_include_directories(libmetalink
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

find_package(expat CONFIG REQUIRED)
target_link_libraries(libmetalink PRIVATE expat::expat)

target_compile_definitions(libmetalink PRIVATE HAVE_TIME)
if(MSVC)
    target_compile_definitions(libmetalink PRIVATE HAVE__MKGMTIME)
    target_compile_definitions(libmetalink PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# 根 CMakeLists 的全局 add_compile_options（-Wall -Wextra -Wconversion…
# /W4）面向自研 C++；vendored C 代码按上游原样编译，target 级关警告
# （覆盖顺序在全局标志之后，GCC/Clang 的 -w 与 MSVC 的 /w 均为最终生效）
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(libmetalink PRIVATE -w)
elseif(MSVC)
    target_compile_options(libmetalink PRIVATE /w)
endif()
