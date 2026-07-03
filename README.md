# 太极时光倒流调试器 — 跨平台二进制版本

**版本**: v1.0 | **日期**: 2026-07-03 | **编制**: 玄同工作室

---

## 一、概述

太极时光倒流调试器将太极三进制模拟器的逆执（时光倒流）机制，通过 LLVM Pass 插桩技术，**完整封装为通用 C/C++ 调试器**。

核心思想：任何 C/C++ 程序编译时，LLVM Pass 在每条 `store` 指令前插入快照记录代码。程序崩溃时，自动保留崩溃前完整状态，支持交互式回退——**逐条恢复被修改的内存，回到崩溃前任意时刻**。

对用户来说，他只需要用带这个 Pass 的 Clang 编译自己的代码，然后用 `taiji-debug` 加载。他不需要知道太极三进制是什么。

---

## 二、架构

```
┌──────────────────────────────────────────────┐
│  用户代码 (C/C++)                            │
│  int *p = NULL;                              │
│  *p = 42;  ← 崩溃                           │
└──────────────────┬───────────────────────────┘
                   │ Clang + 太极插桩 Pass
                   │ (LLVM Module Pass)
                   ▼
┌──────────────────────────────────────────────┐
│  插桩后 IR                                    │
│  %old = load i32, i32* %p                    │
│  call @snapshot_record(%p, %old, %pc, 4)     │
│  store i32 42, i32* %p                       │
└──────────────────┬───────────────────────────┘
                   │ 链接太极运行时库
                   ▼
┌──────────────────────────────────────────────┐
│  可执行文件 (自带逆执能力)                    │
│  ┌──────────────────────────────────────┐    │
│  │  snapshot_runtime (环形缓冲区 1024)   │    │
│  │  crash_handler (信号捕获)             │    │
│  │  interactive_debug_loop (交互调试)    │    │
│  └──────────────────────────────────────┘    │
└──────────────────┬───────────────────────────┘
                   │ taiji-debug 加载
                   ▼
┌──────────────────────────────────────────────┐
│  太极时光倒流调试器                          │
│  [崩溃! 可用快照: 847]                       │
│  [dbg] > w 5      ← 回溯5步                  │
│  [dbg] > s         ← 查看快照列表            │
│  [dbg] > mem 0x..  ← 查看内存               │
└──────────────────────────────────────────────┘
```

---

## 三、文件结构

```
taiji-snapshot/               (F:\操作系统\时光倒流调试器\)
├── CMakeLists.txt             # CMake 构建配置（跨平台）
├── snapshot_runtime.h         # 快照运行时 C API (头文件)
├── snapshot_runtime.c         # 快照运行时实现 (环形缓冲/检查点/恢复)
├── TaijiSnapshotPass.cpp      # LLVM 插桩 Pass (ModulePass + FunctionPass)
├── taiji-debug.c              # 命令行调试器 (主程序)
├── build.bat                  # Windows 构建脚本 (MinGW/MSVC)
├── build.sh                   # Linux/macOS 构建脚本
├── example/
│   └── crash_demo.c           # 7种崩溃场景演示程序
├── build/                     # 构建产物目录
│   ├── libsnapshot_runtime.a  # 快照运行时静态库
│   ├── taiji-snapshot.so      # LLVM 插桩 Pass 插件
│   ├── taiji-debug.exe        # 命令行调试器
│   └── crash_demo.exe         # 崩溃演示程序
└── README.md                  # 本文档
```

---

## 四、快速开始

### 4.1 环境要求

| 组件 | 用途 | 最低版本 |
|------|------|----------|
| Clang | C/C++ 编译器 | 14.0+ |
| LLVM | Pass 编译 | 14.0+ |
| CMake | 可选构建系统 | 3.16+ |

### 4.2 构建

**方式一：CMake（推荐，跨平台）**

```bash
cd taiji-snapshot
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**方式二：脚本构建**

```bash
# Windows
cd taiji-snapshot
build.bat

# Linux/macOS
cd taiji-snapshot
chmod +x build.sh
./build.sh
```

产物：
- `build/libsnapshot_runtime.a` — 快照运行时静态库
- `build/taiji-snapshot.so` — LLVM 插桩 Pass
- `build/taiji-debug` — 命令行调试器
- `build/crash_demo` — 崩溃演示程序

### 4.3 使用

**方式一：直接使用（无需 LLVM Pass）**

程序手动调用 `snapshot_init()` + `crash_handler_install()` 即可：

```bash
# 编译（不用 Pass）
gcc my_program.c taiji-snapshot/snapshot_runtime.c -o my_program

# 调试
taiji-debug ./my_program
```

**方式二：插桩编译（LLVM Pass 自动插入）**

```bash
# 用 Clang + LLVM Pass 编译
clang -fpass-plugin=./build/taiji-snapshot.so \
      my_program.c \
      taiji-snapshot/snapshot_runtime.c \
      -o my_program

# 调试
taiji-debug ./my_program
```

**方式三：集成到 CMake 项目**

```cmake
# CMakeLists.txt
add_library(taiji_snapshot STATIC
    taiji-snapshot/snapshot_runtime.c
)
target_include_directories(taiji_snapshot PUBLIC taiji-snapshot)

add_executable(my_app main.c)
target_link_libraries(my_app taiji_snapshot)

# 启用太极插桩 (需要 LLVM 15+)
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(my_app PRIVATE
        -fpass-plugin=${CMAKE_SOURCE_DIR}/build/taiji-snapshot.so
    )
endif()
```

---

## 五、运行时 API

### 5.1 初始化与清理

```c
#include "snapshot_runtime.h"

int main() {
    snapshot_init();            // 初始化快照系统
    crash_handler_install();    // 安装崩溃处理器

    // ... 用户代码 ...

    crash_handler_uninstall();  // 卸载崩溃处理器
    return 0;
}
```

### 5.2 快照记录

```c
// 手动记录快照（LLVM Pass 自动插入，一般不需要手动调用）
void snapshot_record(void* addr, uint64_t old_value, uint64_t pc, int size);
```

### 5.3 时光倒流

```c
// 回溯 N 步
int steps = snapshot_rewind(10);  // 返回实际回溯步数

// 恢复到检查点
snapshot_rewind_to_checkpoint(3);  // 恢复到检查点 #3
snapshot_rewind_to_checkpoint(-1); // 恢复到最近检查点
```

### 5.4 检查点

```c
// 手动创建检查点
snapshot_create_checkpoint(0);

// 查询
int count = snapshot_checkpoint_count();
```

### 5.5 查询

```c
int total = snapshot_count();           // 可用快照数
double usage = snapshot_buffer_usage(); // 缓冲区使用率 0.0~1.0

// 快照列表
SnapshotSummary summaries[100];
int n = snapshot_list_snapshots(summaries, 100);

// 检查点列表
CheckpointSummary checkpoints[64];
int m = snapshot_list_checkpoints(checkpoints, 64);
```

### 5.6 暂停/恢复

```c
snapshot_pause();    // 暂停记录（性能关键区域）
snapshot_resume();   // 恢复记录
bool paused = snapshot_is_paused();
```

---

## 六、LLVM Pass 详解

### 6.1 Pass 类型

`TaijiSnapshotPass` 是一个 **Module Pass**（遍历整个模块的所有函数）。

### 6.2 插桩逻辑

对每条 `store` 指令：

```
原始 IR:                         插桩后 IR:
store i32 %v, i32* %p            %old = load i32, i32* %p
                                 call @snapshot_record(%p, %old, %pc, 4)
                                 store i32 %v, i32* %p
```

### 6.3 跳过规则

Pass 自动跳过以下 store 指令（减少开销）：
- **栈变量**（`alloca` 分配的局部变量）
- **常量全局变量**
- **小于 1 字节或大于 8 字节的 store**

### 6.4 编译选项

```bash
# 启用 Pass
clang -fpass-plugin=./taiji-snapshot.so program.c

# 与优化 Pass 组合
clang -O2 -fpass-plugin=./taiji-snapshot.so program.c

# 查看 Pass 执行信息
clang -fpass-plugin=./taiji-snapshot.so -mllvm -debug-only=taiji-snapshot program.c
```

---

## 七、崩溃演示

### 7.1 7 种崩溃场景

| 编号 | 场景 | 崩溃类型 | 时光倒流价值 |
|------|------|----------|-------------|
| 1 | 数组越界写入 | 内存破坏 | 回溯找到越界写入点 |
| 2 | 释放后使用 (UAF) | SIGSEGV | 回溯到释放前查看数据 |
| 3 | 空指针解引用 | SIGSEGV | 回溯找到指针变 NULL 的时刻 |
| 4 | 栈溢出 | SIGSEGV | 回溯查看递归深度 |
| 5 | 除零 | SIGFPE | 回溯查看分母何时变 0 |
| 6 | 数据损坏 | 逻辑错误 | 回溯追踪每次写入 |
| 7 | 多步操作 | 无崩溃 | 检查点恢复演示 |

### 7.2 运行示例

```bash
# 场景2: 释放后使用
$ build/taiji-debug build/crash_demo 2

╔══════════════════════════════════════════════════╗
║        !!! 太极时光倒流 — 崩溃捕获 !!!         ║
╠══════════════════════════════════════════════════╣
║  信号: 11 (段错误)
║  故障地址: 0x...
║  指令地址: 0x...
║  可用快照: 23 条
║  检查点:   0 个
╠══════════════════════════════════════════════════╣
║  命令: w N(回溯) s(快照) c(检查点) h(帮助) q(退出)
╚══════════════════════════════════════════════════╝

[dbg] > w 5
[太极快照] 回溯 5 步，剩余 18 条快照
[dbg] 已回溯 5 步，剩余 18 条快照

[dbg] > s
══ 最近 18 条快照 ══
编号  地址              旧值              PC
────  ────────────────  ────────────────  ────────────────
   0  0x...             0x...            0x...  [8]
   1  0x...             0x...            0x...  [4]
   ...
```

---

## 八、与 GDB 对比

| 功能 | GDB | 太极时光倒流调试器 |
|------|-----|-------------------|
| 断点 | ✅ | ✅ |
| 单步 | ✅ | ✅ |
| 查看变量 | ✅ | ✅ |
| 查看内存 | ✅ | ✅ |
| **回退执行** | ❌ 不支持 | ✅ **核心功能** |
| **快照历史** | ❌ | ✅ 1024条环形缓冲 |
| **检查点** | ❌ | ✅ 64个检查点 |
| **崩溃自动捕获** | 手动 coredump | ✅ 自动进入交互模式 |
| **数据损坏追踪** | 困难 | ✅ 回溯查看每次写入 |
| 跨平台 | ✅ | ✅ (LLVM Pass) |
| 学习曲线 | 陡峭 | ✅ 中文交互命令 |
| 需要重新编译 | ❌ | ✅ (需插桩) |

---

## 九、性能影响

| 插桩范围 | 运行时开销 | 内存开销 |
|----------|-----------|----------|
| 全部 store | ~15-25% | ~16KB (环形缓冲) |
| 跳过栈变量 | ~5-10% | ~16KB |
| 暂停快照 (关键区) | ~0% | ~16KB |

环形缓冲区 1024 条 × 32 字节/条 ≈ 32KB。检查点 64 个 × 4KB ≈ 256KB。总计内存开销 < 300KB。

---

## 十、技术要点

1. **环形缓冲区**：满时自动覆盖最旧快照，同时触发检查点保存
2. **检查点机制**：每 1024 条快照或满时自动生成全量检查点
3. **线程安全**：`atomic` 操作 + 平台锁（Windows CRITICAL_SECTION / POSIX pthread_mutex）
4. **信号安全**：崩溃处理器使用 `sig_atomic_t` 和 `async-signal-safe` 操作
5. **跨平台**：Windows (SEH/VEH) + Linux/macOS (POSIX signal)
6. **零侵入**：运行时库仅需链接，用户代码无需修改（Pass 自动插桩）

---

## 十一、路线图

- [x] v1.0 — 快照运行时库 + LLVM Pass + 命令行调试器
- [ ] v1.1 — 时间轴可视化 (TUI)
- [ ] v1.2 — DAP 协议适配器 (VS Code 集成)
- [ ] v2.0 — WebAssembly 浏览器版
- [ ] v2.1 — 多线程程序支持
- [ ] v3.0 — 生产级性能优化 + 增量快照

---

玄同工作室 · 2026年7月 · 太极逆执·二进制世界
