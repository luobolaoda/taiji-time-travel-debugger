#!/bin/bash
# ================================================================
# 太极快照工具链 构建脚本 (Linux/macOS)
# ================================================================

set -e

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║     太极快照工具链 构建脚本 v1.0               ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""

mkdir -p build

# 编译器检测
if command -v clang &>/dev/null; then
    CC=clang
    CXX=clang++
    echo "[检测] 使用 Clang 编译器"
else
    CC=gcc
    CXX=g++
    echo "[检测] 使用 GCC 编译器"
fi

# === 1. 快照运行时静态库 ===
echo ""
echo "──── 1. 构建快照运行时静态库 ────"
$CC -std=c11 -O2 -fPIC -c snapshot_runtime.c -o build/snapshot_runtime.o
ar rcs build/libsnapshot_runtime.a build/snapshot_runtime.o
echo "[成功] build/libsnapshot_runtime.a"

# === 2. taiji-debug ===
echo ""
echo "──── 2. 构建 taiji-debug 命令行调试器 ────"
$CC -std=c11 -O2 -pthread \
    taiji-debug.c snapshot_runtime.c \
    -o build/taiji-debug
echo "[成功] build/taiji-debug"

# === 3. LLVM Pass ===
echo ""
echo "──── 3. 构建 LLVM 插桩 Pass ────"
if command -v llvm-config &>/dev/null; then
    LLVM_CXXFLAGS=$(llvm-config --cxxflags)
    LLVM_LDFLAGS=$(llvm-config --ldflags --libs core support)
    $CXX -shared -fPIC -fno-rtti -std=c++17 \
        $LLVM_CXXFLAGS \
        TaijiSnapshotPass.cpp \
        $LLVM_LDFLAGS \
        -o build/taiji-snapshot.so
    echo "[成功] build/taiji-snapshot.so"
else
    echo "[跳过] 未检测到 llvm-config"
    echo "  安装: sudo apt install llvm-dev (Ubuntu)"
    echo "        brew install llvm (macOS)"
fi

# === 4. 示例程序 ===
echo ""
echo "──── 4. 编译示例程序 ────"
$CC -std=c11 -O0 -g -pthread \
    example/crash_demo.c \
    snapshot_runtime.c \
    -o build/crash_demo
echo "[成功] build/crash_demo"

# === 5. 如果用 Clang + LLVM，演示插桩编译 ===
if command -v llvm-config &>/dev/null && [ "$CC" = "clang" ]; then
    echo ""
    echo "──── 5. 插桩编译示例 (Clang + Pass) ────"
    $CC -std=c++17 -O0 -g \
        -fpass-plugin=./build/taiji-snapshot.so \
        example/crash_demo.c \
        snapshot_runtime.c \
        -o build/crash_demo_instrumented
    echo "[成功] build/crash_demo_instrumented (已插桩)"
fi

echo ""
echo "╔══════════════════════════════════════════════════╗"
echo "║  构建完成!                                      ║"
echo "╠══════════════════════════════════════════════════╣"
echo "║  build/libsnapshot_runtime.a  — 快照运行时库    ║"
echo "║  build/taiji-debug            — 命令行调试器    ║"
if command -v llvm-config &>/dev/null; then
echo "║  build/taiji-snapshot.so      — LLVM 插桩 Pass   ║"
fi
echo "║  build/crash_demo             — 示例程序        ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""
echo "快速测试: ./build/taiji-debug ./build/crash_demo"
echo ""
