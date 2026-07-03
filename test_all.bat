@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo.
echo ╔══════════════════════════════════════════════════╗
echo ║     太极时光倒流调试器 — 全场景自动化测试        ║
echo ╚══════════════════════════════════════════════════╝
echo.

:: 编译
echo [1/8] 编译 snapshot_runtime...
gcc -std=c11 -O0 -g -c snapshot_runtime.c -o snapshot_runtime.o
if %ERRORLEVEL% NEQ 0 (
    echo [失败] snapshot_runtime 编译失败
    exit /b 1
)

echo [2/8] 编译 crash_demo...
gcc -std=c11 -O0 -g example/crash_demo.c snapshot_runtime.o -o crash_demo.exe
if %ERRORLEVEL% NEQ 0 (
    echo [失败] crash_demo 编译失败
    exit /b 1
)

echo [3/8] 编译 taiji-debug...
gcc -std=c11 -O0 -g taiji-debug.c snapshot_runtime.o -o taiji-debug.exe
if %ERRORLEVEL% NEQ 0 (
    echo [失败] taiji-debug 编译失败
    exit /b 1
)

set PASS=0
set FAIL=0

echo.
echo ══════════════ 运行测试 ══════════════
echo.

:: 场景0: 全部安全场景
echo --- 场景0: 全部安全场景 ---
set TAIJI_NONINTERACTIVE=1
crash_demo.exe 0 >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo   [通过] 场景0 - 全部安全场景
    set /a PASS+=1
) else (
    echo   [失败] 场景0
    set /a FAIL+=1
)

:: 场景6: 数据损坏追踪
echo --- 场景6: 数据损坏追踪 ---
crash_demo.exe 6 >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo   [通过] 场景6 - 数据损坏追踪
    set /a PASS+=1
) else (
    echo   [失败] 场景6
    set /a FAIL+=1
)

:: 场景7: 多步操作+检查点
echo --- 场景7: 多步操作+检查点 ---
crash_demo.exe 7 >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo   [通过] 场景7 - 多步操作+检查点
    set /a PASS+=1
) else (
    echo   [失败] 场景7
    set /a FAIL+=1
)

:: 场景1: 数组越界（安全场景，不会崩溃）
echo --- 场景1: 数组越界写入 ---
crash_demo.exe 1 >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo   [通过] 场景1 - 数组越界写入
    set /a PASS+=1
) else (
    echo   [失败] 场景1
    set /a FAIL+=1
)

:: 场景3: 空指针解引用（崩溃场景，非交互模式）
echo --- 场景3: 空指针解引用 ---
set TAIJI_NONINTERACTIVE=1
crash_demo.exe 3 >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo   [通过] 场景3 - 空指针解引用（崩溃被捕获）
    set /a PASS+=1
) else (
    echo   [失败] 场景3 - 期望崩溃但正常退出
    set /a FAIL+=1
)

:: 场景5: 除零（崩溃场景）
echo --- 场景5: 除零 ---
set TAIJI_NONINTERACTIVE=1
crash_demo.exe 5 >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo   [通过] 场景5 - 除零（崩溃被捕获）
    set /a PASS+=1
) else (
    echo   [失败] 场景5 - 期望崩溃但正常退出
    set /a FAIL+=1
)

:: taiji-debug 帮助
echo --- taiji-debug --help ---
taiji-debug.exe --help >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo   [通过] taiji-debug --help
    set /a PASS+=1
) else (
    echo   [失败] taiji-debug --help
    set /a FAIL+=1
)

:: taiji-debug --version
echo --- taiji-debug --version ---
taiji-debug.exe --version >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo   [通过] taiji-debug --version
    set /a PASS+=1
) else (
    echo   [失败] taiji-debug --version
    set /a FAIL+=1
)

echo.
echo ════════════════════════════════════════
echo   测试结果: %PASS% 通过, %FAIL% 失败
echo ════════════════════════════════════════

:: 清理
del snapshot_runtime.o >nul 2>&1
del crash_demo.exe >nul 2>&1
del taiji-debug.exe >nul 2>&1

