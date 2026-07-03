// ================================================================
// 太极快照运行时库 — 二进制逆执支持
// 版本: v1.0
// 日期: 2026-07-03
// 编制: 玄同工作室
//
// 本运行时库为任意 C/C++ 程序提供"时光倒流"能力。
// 通过 LLVM Pass 在每条 store 指令前插入 snapshot_record() 调用，
// 程序崩溃时自动保留完整历史状态，支持交互式回退。
//
// 核心思想: 太极阳指令逆执机制的二进制等价实现
//   - 阳指令执行前保存旧值 → snapshot_record() 在 store 前保存旧值
//   - 逆执时逐条恢复旧值   → snapshot_rewind() 从环形缓冲区弹出旧值
//   - 检查点机制            → 每 1024 条快照自动生成全量检查点
//
// 兼容: C99 / C++11 / Linux / macOS / Windows (MinGW / MSVC)
// 依赖: 仅标准库 (stdatomic / pthread / Windows CRITICAL_SECTION)
// ================================================================

#ifndef SNAPSHOT_RUNTIME_H
#define SNAPSHOT_RUNTIME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// === 常量 ===

#define SNAPSHOT_BUFFER_SIZE    1024    // 环形缓冲区容量
#define SNAPSHOT_MAX_CHECKPOINTS 64     // 最大检查点数
#define SNAPSHOT_REGISTER_COUNT  32     // 通用寄存器数（模拟）
#define SNAPSHOT_STACK_SIZE     4096    // 检查点栈帧快照大小

// === 数据结构 ===

// 单条快照记录
typedef struct {
    void*    addr;          // 被修改的内存地址
    uint64_t old_value;     // 旧值（8字节）
    uint64_t pc;            // 指令地址（调用 snapshot_record 时的返回地址）
    int      size;          // 写入大小（1/2/4/8 字节）
} SnapshotEntry;

// 全量检查点
typedef struct {
    uint64_t pc;                                    // 检查点时的 PC
    uint64_t registers[SNAPSHOT_REGISTER_COUNT];    // 寄存器快照
    uint64_t stack_top;                             // 栈顶指针
    uint8_t  stack_snapshot[SNAPSHOT_STACK_SIZE];   // 栈帧快照
    int      snapshot_index;                        // 对应的快照编号
    bool     valid;                                 // 是否有效
} SnapshotCheckpoint;

// 快照摘要（调试器查询用）
typedef struct {
    int      index;          // 快照编号
    void*    addr;           // 内存地址
    uint64_t old_value;      // 旧值
    uint64_t pc;             // 指令地址
    int      size;           // 写入大小
} SnapshotSummary;

// 检查点摘要
typedef struct {
    int      index;          // 检查点编号
    uint64_t pc;             // PC
    int      snapshot_count; // 包含的快照数
    bool     valid;
} CheckpointSummary;

// 回溯后的状态信息
typedef struct {
    uint64_t pc;             // 回退后的 PC
    int      steps_rewound;  // 实际回退步数
    int      remaining;      // 剩余可用快照数
} RewindResult;

// === 运行时 API ===

// 初始化快照系统
// 应在 main() 开头调用
void snapshot_init(void);

// 记录快照（由 LLVM Pass 自动插入，用户无需调用）
// addr:     被修改的内存地址
// old_value: 旧值（load 指令读取的结果）
// pc:       当前指令地址（__builtin_return_address(0)）
// size:     写入大小（1/2/4/8）
void snapshot_record(void* addr, uint64_t old_value, uint64_t pc, int size);

// 回退 N 步
// 返回: 实际回退步数
int snapshot_rewind(int steps);

// 回退到指定检查点
// checkpoint_index: -1 表示最近检查点
bool snapshot_rewind_to_checkpoint(int checkpoint_index);

// 获取当前可用快照数
int snapshot_count(void);

// 获取当前可用检查点数
int snapshot_checkpoint_count(void);

// 获取快照列表（调试器用）
// out: 输出缓冲区
// max: 最大条目数
// 返回: 实际填充条目数
int snapshot_list_snapshots(SnapshotSummary* out, int max);

// 获取检查点列表
int snapshot_list_checkpoints(CheckpointSummary* out, int max);

// 手动生成检查点
void snapshot_create_checkpoint(uint64_t pc);

// 重置快照系统（用于程序重新运行）
void snapshot_reset(void);

// 获取调试信息字符串（供命令行调试器显示）
const char* snapshot_debug_info(void);

// 获取环形缓冲区使用率（0.0 ~ 1.0）
double snapshot_buffer_usage(void);

// 启用/禁用快照记录（用于性能关键区域临时关闭）
void snapshot_pause(void);
void snapshot_resume(void);
bool snapshot_is_paused(void);

// === 崩溃处理 ===

// 安装信号处理器（自动捕获 SIGSEGV/SIGFPE/SIGILL/SIGABRT）
// 崩溃时自动进入交互式调试模式
void crash_handler_install(void);

// 卸载信号处理器
void crash_handler_uninstall(void);

// 手动触发交互式调试模式
void crash_enter_debug_mode(int sig, void* fault_addr, uint64_t fault_pc);

// === 跨平台原子操作 ===

// 获取单调递增的快照 ID（线程安全）
int64_t snapshot_next_id(void);

#ifdef __cplusplus
}
#endif

#endif // SNAPSHOT_RUNTIME_H
