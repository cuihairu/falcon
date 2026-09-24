/* MSVC POSIX 桥接头——上游 libmetalink 仅支持 POSIX/mingw（mingw 自带
 * unistd.h）。MSVC 构建时经 CMake 注入本目录，为 metalink_parse_fd 的
 * fd 读取路径（read/ssize_t）提供 CRT 等价物；生产消费方只走
 * metalink_parse_memory，本桥接仅为该 TU 可编译。
 * POSIX 平台不加入此 include 路径，系统 unistd.h 原样生效。 */
#pragma once

#include <io.h>       /* _read */
#include <errno.h>    /* EINTR（MSVC errno.h 自带） */
#include <basetsd.h>  /* SSIZE_T */

typedef SSIZE_T ssize_t;
#define read _read
