# ---------------------------------------------------------------------------
# TacticalAI-Sandbox —— 战术 AI 沙盒
#
# 一个用于实验行为树 / 黑板 / 感知 / EQS / 寻路的可交互沙盒。
# 构建:  cmake -S . -B build -G Ninja && cmake --build build
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)

project(TacticalAI-Sandbox
    VERSION     0.1.0
    DESCRIPTION "战术 AI 沙盒：行为树 / EQS / 寻路 可视化实验平台"
    LANGUAGES   CXX
)

# --- 全局语言与输出 ---------------------------------------------------------
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)   # 严格 ISO C++17
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # 供 clangd / IDE 使用

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Dev CACHE STRING "构建类型" FORCE)
endif()

# 所有可执行文件与库统一输出到 build/bin，避免散落在各子目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# 多配置生成器（VS / Xcode）下同样规整输出目录
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach()

# --- 选项 -------------------------------------------------------------------
option(TACTICALAI_BUILD_TESTS  "构建 GoogleTest 单元测试" ON)
option(TACTICALAI_WARNINGS_AS_ERRORS "把编译警告视为错误" OFF)
option(TACTICALAI_ENABLE_ASAN  "启用 AddressSanitizer（GCC/Clang）" OFF)

# 各依赖的本地路径覆盖点，例如 -DTACTICALAI_RAYLIB_DIR=D:/libs/raylib
set(TACTICALAI_RAYLIB_DIR       "" CACHE PATH "raylib 源码或安装目录")
set(TACTICALAI_IMGUI_DIR        "" CACHE PATH "imgui 源码目录")
set(TACTICALAI_GLM_DIR          "" CACHE PATH "glm 源码目录")
set(TACTICALAI_NLOHMANN-JSON_DIR "" CACHE PATH "nlohmann-json 源码目录")
set(TACTICALAI_GOOGLETEST_DIR   "" CACHE PATH "googletest 源码目录")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- 依赖解析 ---------------------------------------------------------------
include(Dependencies)
tacticalai_resolve_dependencies()

# --- 编译选项 target --------------------------------------------------------
# 集中定义一次，各 target 只需 link 它，保证全项目编译选项一致。
add_library(tacticalai_build_flags INTERFACE)
target_compile_features(tacticalai_build_flags INTERFACE cxx_std_17)

if(MSVC)
    target_compile_options(tacticalai_build_flags INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /MP
    )
    target_compile_definitions(tacticalai_build_flags INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
else()
    target_compile_options(tacticalai_build_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    )
    target_compile_definitions(tacticalai_build_flags INTERFACE
        $<$<CONFIG:Debug>:TACTICALAI_DEBUG>
    )
    if(TACTICALAI_WARNINGS_AS_ERRORS)
        target_compile_options(tacticalai_build_flags INTERFACE -Werror)
    endif()
    if(TACTICALAI_ENABLE_ASAN AND NOT WIN32)
        target_compile_options(tacticalai_build_flags INTERFACE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(tacticalai_build_flags INTERFACE -fsanitize=address)
    endif()
endif()

# --- 源码模块 ---------------------------------------------------------------
# 拆成 4 个静态库而非一个大 target，好处是可分别复用于单元测试，
# 且依赖方向被 CMake 强制约束（game/ai 不能反向依赖 debug，debug 不能依赖 game）。
add_subdirectory(src)
add_subdirectory(tests)

# --- 汇总信息 ---------------------------------------------------------------
message(STATUS "")
message(STATUS "========== TacticalAI-Sandbox 配置摘要 ==========")
message(STATUS "  版本        : ${PROJECT_VERSION}")
message(STATUS "  构建类型    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  编译器      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ 标准    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  单元测试    : ${TACTICALAI_BUILD_TESTS}")
message(STATUS "  输出目录    : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "================================================")
message(STATUS "")
