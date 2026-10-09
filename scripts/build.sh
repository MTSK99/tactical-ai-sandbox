#!/usr/bin/env bash
# ---------------------------------------------------------------------------
#  build.sh —— 便捷构建脚本（Linux / macOS / Git Bash）
#
#  用法:
#     ./scripts/build.sh              # Release 构建 + 编译 + 跑测试
#     ./scripts/build.sh --debug      # Debug 构建
#     ./scripts/build.sh --clean      # 清理后重建
#     ./scripts/build.sh --no-tests   # 不构建测试（更快）
#     ./scripts/build.sh --run        # 构建完成后直接启动沙盒
# ---------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

BUILD_TYPE="Release"
BUILD_TESTS="ON"
CLEAN=0
RUN_AFTER=0

for arg in "$@"; do
    case "$arg" in
        --debug)     BUILD_TYPE="Debug" ;;
        --release)   BUILD_TYPE="Release" ;;
        --clean)     CLEAN=1 ;;
        --no-tests)  BUILD_TESTS="OFF" ;;
        --run)       RUN_AFTER=1 ;;
        --help|-h)
            sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "[build] 未知参数: $arg（用 --help 查看用法）" >&2
            exit 1
            ;;
    esac
done

# --- 选择生成器：优先 Ninja（增量构建快得多）---
GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
    echo "[build] 使用 Ninja 生成器"
else
    echo "[build] 未找到 Ninja，使用默认生成器"
fi

# --- 清理 ---
if [ "$CLEAN" -eq 1 ]; then
    echo "[build] 清理 build/ 目录"
    rm -rf build
fi

# --- 检查 external/ 依赖状况并给出提示 ---
if [ ! -d "external/raylib" ] && [ ! -d "third_party/raylib/src" ]; then
    echo "[build] 提示: external/raylib 不存在，将依赖 CMake 的自动解析"
    echo "        若构建失败，请运行: pwsh ./scripts/fetch_dependencies.ps1"
fi

# --- 配置 ---
echo "[build] 配置: 类型=$BUILD_TYPE 测试=$BUILD_TESTS"
cmake -S . -B build "${GENERATOR_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DTACTICALAI_BUILD_TESTS="$BUILD_TESTS"

# --- 编译 ---
# 用 CPU 核心数并行编译；失败时显示详细错误而不是静默退出
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
echo "[build] 编译（$JOBS 并行任务）"
if ! cmake --build build --parallel "$JOBS"; then
    echo ""
    echo "[build] 编译失败。请检查上方错误信息。" >&2
    echo "[build] 常见原因:" >&2
    echo "         - 依赖未获取: pwsh ./scripts/fetch_dependencies.ps1" >&2
    echo "         - 编译器版本过低（需要完整 C++17 支持）" >&2
    exit 1
fi

echo ""
echo "[build] 构建完成:"
echo "        可执行文件: build/bin/TacticalAI-Sandbox"

# --- 测试 ---
if [ "$BUILD_TESTS" = "ON" ] && [ -f "build/bin/TacticalAI-Tests" ]; then
    echo ""
    echo "[build] 运行单元测试"
    if [ -d "build" ]; then
        (cd build && ctest --output-on-failure --label-regex unit) || {
            echo "[build] 部分测试失败" >&2
            exit 1
        }
    fi
fi

# --- 启动 ---
if [ "$RUN_AFTER" -eq 1 ]; then
    echo ""
    echo "[build] 启动沙盒"
    ./build/bin/TacticalAI-Sandbox
fi

echo ""
echo "[build] 全部完成"
