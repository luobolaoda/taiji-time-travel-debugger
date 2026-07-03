@echo off
REM ================================================================
REM 太极快照工具链 构建脚本
REM 版本: v1.0 | 日期: 2026-07-03
REM
REM 构建三个组件:
REM   1. libsnapshot_runtime.a — 快照运行时静态库
REM   2. taiji-snapshot.so     — LLVM 插桩 Pass
REM   3. taiji-debug.exe       — 命令行调试器
REM ================================================================

setlocal enabledelayedexpansion

echo.
echo ╔══════════════════════════════════════════════════╗
echo ║     太极快照工具链 构建脚本 v1.0               ║
echo ╚══════════════════════════════════════════════════╝
echo.

REM === 环境检测 ===
set CC=gcc
set CXX=g++
set HAS_LLVM=0

where clang >nul 2>&1
if %errorlevel% equ 0 (
    set CC=clang
    set CXX=clang++
    echo [检测] 使用 Clang 编译器
) else (
    where gcc >nul 2>&1
    if %errorlevel% equ 0 (
        echo [检测] 使用 GCC 编译器
    ) else (
        echo [错误] 未找到 C 编译器 (clang/gcc)
        exit /b 1
    )
)

where llvm-config >nul 2>&1
if %errorlevel% equ 0 (
    set HAS_LLVM=1
    echo [检测] 找到 llvm-config
) else (
    echo [警告] 未找到 llvm-config，跳过 LLVM Pass 构建
)

REM === 创建输出目录 ===
if not exist "build\" mkdir build

echo.
echo ──── 1. 构建快照运行时静态库 ────
%CC% -std=c11 -O2 -c snapshot_runtime.c -o build\snapshot_runtime.o
if %errorlevel% neq 0 (
    echo [失败] snapshot_runtime.c 编译失败
    exit /b 1
)
ar rcs build\libsnapshot_runtime.a build\snapshot_runtime.o
echo [成功] build\libsnapshot_runtime.a

echo.
echo ──── 2. 构建 taiji-debug 命令行调试器 ────
%CC% -std=c11 -O2 ^
    taiji-debug.c snapshot_runtime.c ^
    -o build\taiji-debug.exe
if %errorlevel% neq 0 (
    echo [失败] taiji-debug 编译失败
    exit /b 1
)
echo [成功] build\taiji-debug.exe

REM === LLVM Pass ===
if %HAS_LLVM% equ 1 (
    echo.
    echo ──── 3. 构建 LLVM 插桩 Pass ────
    for /f "tokens=*" %%i in ('llvm-config --cxxflags') do set LLVM_CXXFLAGS=%%i
    for /f "tokens=*" %%i in ('llvm-config --ldflags --libs') do set LLVM_LDFLAGS=%%i

    %CXX% -shared -fPIC -fno-rtti -std=c++17 ^
        %LLVM_CXXFLAGS% ^
        TaijiSnapshotPass.cpp ^
        %LLVM_LDFLAGS% ^
        -o build\taiji-snapshot.so
    if %errorlevel% neq 0 (
        echo [警告] LLVM Pass 编译失败（可能 LLVM 版本不兼容）
    ) else (
        echo [成功] build\taiji-snapshot.so
    )
) else (
    echo.
    echo ──── 3. LLVM 插桩 Pass ────
    echo [跳过] 未检测到 LLVM 开发环境
    echo   安装: sudo apt install llvm-dev  (Linux)
    echo        brew install llvm           (macOS)
    echo        choco install llvm          (Windows)
)

echo.
echo ──── 4. 编译示例程序 ────
%CC% -std=c11 -O0 -g ^
    example\crash_demo.c ^
    snapshot_runtime.c ^
    -o build\crash_demo.exe
if %errorlevel% neq 0 (
    echo [警告] 示例程序编译失败
) else (
    echo [成功] build\crash_demo.exe
)

REM === 构建总结 ===
echo.
echo ╔══════════════════════════════════════════════════╗
echo ║  构建完成!                                      ║
echo ╠══════════════════════════════════════════════════╣
echo ║  build\libsnapshot_runtime.a  — 快照运行时库    ║
echo ║  build\taiji-debug.exe        — 命令行调试器    ║
if %HAS_LLVM% equ 1 (
    echo ║  build\taiji-snapshot.so      — LLVM 插桩 Pass   ║
)
echo ║  build\crash_demo.exe         — 示例程序        ║
echo ╚══════════════════════════════════════════════════╝
echo.
echo 快速测试: build\taiji-debug.exe build\crash_demo.exe
echo.

endlocal
