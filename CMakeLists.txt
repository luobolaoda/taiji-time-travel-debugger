# ================================================================
# 太极快照插桩 LLVM Pass — CMake 构建配置
# 版本: v1.0
# 日期: 2026-07-03
# 编制: 玄同工作室
#
# 构建方式:
#   mkdir build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   make
#
# 产出:
#   - taiji-snapshot.so     LLVM Pass 插件
#   - libsnapshot_runtime.a 快照运行时静态库
#   - taiji-debug           命令行调试器
#   - crash_demo            崩溃演示程序
# ================================================================

cmake_minimum_required(VERSION 3.16)
project(TaijiSnapshotPass VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# === 选项 ===
option(BUILD_LLVM_PASS   "构建 LLVM 插桩 Pass" ON)
option(BUILD_RUNTIME     "构建快照运行时库"     ON)
option(BUILD_TOOLS       "构建命令行工具"       ON)
option(BUILD_EXAMPLES    "构建示例程序"         ON)
option(ENABLE_THREAD_SAFE "启用线程安全（需要 pthread/Windows API）" ON)

# === 1. 快照运行时库 ===
if(BUILD_RUNTIME)
    add_library(snapshot_runtime STATIC snapshot_runtime.c)
    target_include_directories(snapshot_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

    if(ENABLE_THREAD_SAFE)
        if(WIN32)
            # Windows: CRITICAL_SECTION 无需额外链接
        elseif(UNIX)
            find_package(Threads REQUIRED)
            target_link_libraries(snapshot_runtime PUBLIC Threads::Threads)
        endif()
    endif()

    message(STATUS "快照运行时库: 已启用")
endif()

# === 2. LLVM 插桩 Pass ===
if(BUILD_LLVM_PASS)
    find_package(LLVM QUIET CONFIG)
    if(LLVM_FOUND)
        message(STATUS "LLVM 版本: ${LLVM_PACKAGE_VERSION}")

        # 新版 Pass Manager (LLVM 13+)
        add_library(TaijiSnapshotPass MODULE TaijiSnapshotPass.cpp)
        target_include_directories(TaijiSnapshotPass PRIVATE ${LLVM_INCLUDE_DIRS})
        target_compile_definitions(TaijiSnapshotPass PRIVATE ${LLVM_DEFINITIONS})
        target_link_libraries(TaijiSnapshotPass PRIVATE LLVM)

        # 禁止 RTTI（LLVM 要求）
        target_compile_options(TaijiSnapshotPass PRIVATE -fno-rtti)

        # 设置输出名称
        if(WIN32)
            set_target_properties(TaijiSnapshotPass PROPERTIES
                PREFIX ""
                SUFFIX ".dll"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            )
        else()
            set_target_properties(TaijiSnapshotPass PROPERTIES
                PREFIX ""
                SUFFIX ".so"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            )
        endif()

        message(STATUS "LLVM 插桩 Pass: 已启用")
    else()
        message(WARNING "未找到 LLVM，跳过 LLVM Pass 构建")
        message(WARNING "安装: sudo apt install llvm-dev (Linux)")
        message(WARNING "      brew install llvm       (macOS)")
        message(WARNING "      choco install llvm      (Windows)")
    endif()
endif()

# === 3. 命令行调试器 ===
if(BUILD_TOOLS)
    add_executable(taiji-debug taiji-debug.c snapshot_runtime.c)
    target_include_directories(taiji-debug PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(taiji-debug PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    if(WIN32)
        set_target_properties(taiji-debug PROPERTIES SUFFIX ".exe")
    endif()

    message(STATUS "命令行调试器: 已启用")
endif()

# === 4. 示例程序 ===
if(BUILD_EXAMPLES)
    add_executable(crash_demo
        example/crash_demo.c
        snapshot_runtime.c
    )
    target_include_directories(crash_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(crash_demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    if(WIN32)
        set_target_properties(crash_demo PROPERTIES SUFFIX ".exe")
    endif()

    message(STATUS "示例程序: 已启用")
endif()

# === 安装规则 ===
install(TARGETS snapshot_runtime
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(TARGET TaijiSnapshotPass)
    install(TARGETS TaijiSnapshotPass
        LIBRARY DESTINATION lib/taiji
    )
endif()

if(TARGET taiji-debug)
    install(TARGETS taiji-debug
        RUNTIME DESTINATION bin
    )
endif()

install(FILES snapshot_runtime.h
    DESTINATION include/taiji
)

# === 构建信息 ===
message(STATUS "")
message(STATUS "════════ 太极快照工具链 v${PROJECT_VERSION} ════════")
message(STATUS "  构建目录: ${CMAKE_BINARY_DIR}")
message(STATUS "  源码目录: ${CMAKE_SOURCE_DIR}")
message(STATUS "  编译器:   ${CMAKE_C_COMPILER}")
message(STATUS "  C++编译器:${CMAKE_CXX_COMPILER}")
message(STATUS "═══════════════════════════════════════════════")
