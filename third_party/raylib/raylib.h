# ---------------------------------------------------------------------------
#  third_party/raylib/raylib.h
#
#  兼容转发头。
#
#  为什么需要它：
#    游戏层里 `#include "raylib.h"` 的写法在「系统安装 raylib」时可用，
#    但在「external/raylib 源码构建」时，头文件实际位于
#    external/raylib/src/raylib.h，因此编译会失败。
#
#    这个转发头让两种情形都成立：
#      1. 若 raylib 的 include 路径已由 CMake 的 target 传递（正常情况），
#         下方的 #include_next / 直接包含会命中真正的头文件。
#      2. 若路径未传递，它按常见位置逐个尝试。
#
#  注意：这是「兜底」，不替代 CMake 的 target_include_directories。
#  实践中 raylib 的 CMake target 已经正确导出 include 路径，
#  因此本文件通常不会被用到。保留下来的价值是让 IDE 的
#  「跳转到定义」与静态分析工具也能找到头文件。
# ---------------------------------------------------------------------------
#pragma once

// 情况 1：raylib 的 include 路径已由 CMake 传递
#if defined(__has_include)
#  if __has_include(<raylib.h>) && !defined(TACTICALAI_RAYLIB_FORWARDING)
#    define TACTICALAI_RAYLIB_FORWARDING
#    include <raylib.h>
#    undef TACTICALAI_RAYLIB_FORWARDING
#    define TACTICALAI_RAYLIB_INCLUDED
#  endif
#endif

// 情况 2：使用 external/raylib 源码布局，头文件在 src/ 下
#if !defined(TACTICALAI_RAYLIB_INCLUDED)
#  if defined(__has_include)
#    if __has_include("../../../external/raylib/src/raylib.h")
#      include "../../../external/raylib/src/raylib.h"
#      define TACTICALAI_RAYLIB_INCLUDED
#    elif __has_include("../../../third_party/raylib/src/raylib.h")
#      include "../../../third_party/raylib/src/raylib.h"
#      define TACTICALAI_RAYLIB_INCLUDED
#    endif
#  endif
#endif

// 情况 3：都失败时给出明确诊断，而不是让编译器报「找不到 raylib.h」
// 这种令人困惑的间接错误。
#if !defined(TACTICALAI_RAYLIB_INCLUDED)
#  error "找不到 raylib.h。请运行 pwsh ./scripts/fetch_dependencies.ps1 获取依赖，或用 -DTACTICALAI_RAYLIB_DIR=<path> 指定 raylib 路径。"
#endif
