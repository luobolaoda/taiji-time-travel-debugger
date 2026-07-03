// ================================================================
// 太极时光倒流调试器 — 崩溃演示程序
// 版本: v1.0
// 日期: 2026-07-03
//
// 本程序演示多种崩溃场景，以及时光倒流如何帮助定位根因。
// 用 taiji-debug 加载后，崩溃时自动进入交互调试模式。
// ================================================================

#include "../snapshot_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// === 模拟快照记录（无 LLVM Pass 时的手动插桩） ===
// 在真正的 LLVM Pass 场景中，编译器会在每次 store 前自动插入
// snapshot_record(addr, old_value, pc, size) 调用。
// 这里手动模拟，让演示程序在没有 LLVM 的情况下也能展示快照功能。
#define SIMULATE_SNAPSHOT(var) do { \
    snapshot_record((void*)&(var), (uint64_t)(var), (uint64_t)0, sizeof(var)); \
} while(0)

// === 全局状态（会被 store 插桩覆盖） ===
static int    g_counter = 0;
static int    g_array[10];
static char*  g_buffer = NULL;
static int    g_corrupt_data = 42;

// === 场景1: 数组越界写入 ===
void scenario_array_bounds(void) {
    printf("\n══ 场景1: 数组越界写入 ══\n");
    printf("向数组 [0..9] 写入数据...\n");

    for (int i = 0; i <= 10; i++) {
        SIMULATE_SNAPSHOT(g_array[i]);
        g_array[i] = i * 100;
        printf("  g_array[%d] = %d\n", i, g_array[i]);
    }

    printf("越界写入完成（可能已破坏相邻内存）\n");
}

// === 场景2: 释放后使用 (Use-After-Free) ===
void scenario_use_after_free(void) {
    printf("\n══ 场景2: 释放后使用 ══\n");

    g_buffer = (char*)malloc(256);
    strcpy(g_buffer, "Hello, Taiji Snapshot!");
    printf("  分配缓冲区: %p -> \"%s\"\n", (void*)g_buffer, g_buffer);

    free(g_buffer);
    printf("  已释放缓冲区\n");

    // 释放后写入 — 崩溃点（快照记录释放前的状态）
    SIMULATE_SNAPSHOT(g_buffer);
    printf("  尝试写入已释放的内存...\n");
    strcpy(g_buffer, "CORRUPTED!");
}

// === 场景3: 空指针解引用 ===
void scenario_null_pointer(void) {
    printf("\n══ 场景3: 空指针解引用 ══\n");

    int* p = NULL;
    printf("  指针: %p\n", (void*)p);

    // 空指针写入 — 崩溃点
    printf("  尝试写入空指针...\n");
    *p = 42;
}

// === 场景4: 栈溢出 ===
void recursive_overflow(int depth) {
    char local[1024];  // 每次调用消耗 ~1KB 栈
    local[0] = (char)(depth & 0xFF);

    printf("  递归深度: %d\n", depth);
    recursive_overflow(depth + 1);
}

void scenario_stack_overflow(void) {
    printf("\n══ 场景4: 栈溢出 ══\n");
    recursive_overflow(1);
}

// === 场景5: 除零 ===
void scenario_divide_by_zero(void) {
    printf("\n══ 场景5: 除零 ══\n");

    int a = 100;
    int b = 0;

    printf("  计算 %d / %d ...\n", a, b);
    int result = a / b;
    printf("  结果: %d\n", result);
}

// === 场景6: 数据损坏追踪 ===
void scenario_data_corruption(void) {
    printf("\n══ 场景6: 数据损坏追踪 ══\n");
    printf("  初始值: g_corrupt_data = %d\n", g_corrupt_data);

    // 模拟多次写入
    SIMULATE_SNAPSHOT(g_corrupt_data);
    g_corrupt_data = 100;
    printf("  写入 100: g_corrupt_data = %d\n", g_corrupt_data);

    SIMULATE_SNAPSHOT(g_corrupt_data);
    g_corrupt_data = 200;
    printf("  写入 200: g_corrupt_data = %d\n", g_corrupt_data);

    SIMULATE_SNAPSHOT(g_corrupt_data);
    g_corrupt_data = -1;  // 意外写入
    printf("  意外写入 -1: g_corrupt_data = %d\n", g_corrupt_data);

    SIMULATE_SNAPSHOT(g_corrupt_data);
    g_corrupt_data = 300;
    printf("  写入 300: g_corrupt_data = %d\n", g_corrupt_data);

    // 检查数据是否正确
    if (g_corrupt_data != 300) {
        printf("  [错误] 数据损坏! 期望 300，实际 %d\n", g_corrupt_data);
        // 此时可以用快照回溯查看是哪次写入导致的问题
    }
}

// === 场景7: 多步操作 + 手动检查点 ===
void scenario_multi_step(void) {
    printf("\n══ 场景7: 多步操作 + 检查点 ══\n");

    g_counter = 0;
    for (int i = 1; i <= 10; i++) {
        SIMULATE_SNAPSHOT(g_counter);
        g_counter += i;
        printf("  步骤 %2d: g_counter = %d\n", i, g_counter);

        // 每5步生成一个检查点
        if (i % 5 == 0) {
            snapshot_create_checkpoint(0);
            printf("    [检查点 #%d 已创建]\n", snapshot_checkpoint_count() - 1);
        }
    }

    printf("  最终值: g_counter = %d (期望 55)\n", g_counter);

    if (g_counter != 55) {
        printf("  [错误] 累加结果不正确!\n");
    }
}

// === 主函数 ===
int main(int argc, char* argv[]) {
    // 禁用 stdout 缓冲，确保输出即时可见
    setbuf(stdout, NULL);

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║     太极时光倒流 — 崩溃演示程序                ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
    printf("\n");

    // 初始化快照系统
    snapshot_init();

    // 安装崩溃处理器
    crash_handler_install();

    // 选择场景
    int scenario = 0;

    if (argc > 1) {
        scenario = atoi(argv[1]);
    } else {
        printf("请选择崩溃场景:\n");
        printf("  1 — 数组越界写入\n");
        printf("  2 — 释放后使用 (Use-After-Free)\n");
        printf("  3 — 空指针解引用\n");
        printf("  4 — 栈溢出\n");
        printf("  5 — 除零\n");
        printf("  6 — 数据损坏追踪\n");
        printf("  7 — 多步操作 + 检查点\n");
        printf("  0 — 全部运行 (无崩溃场景)\n");
        printf("\n选择 (0-7): ");
        fflush(stdout);
        scanf("%d", &scenario);
    }

    printf("\n");

    switch (scenario) {
        case 1:
            scenario_array_bounds();
            break;
        case 2:
            scenario_use_after_free();
            break;
        case 3:
            scenario_null_pointer();
            break;
        case 4:
            scenario_stack_overflow();
            break;
        case 5:
            scenario_divide_by_zero();
            break;
        case 6:
            scenario_data_corruption();
            break;
        case 7:
            scenario_multi_step();
            break;
        case 0:
            scenario_multi_step();
            scenario_data_corruption();
            printf("\n══ 全部安全场景执行完毕 ══\n");
            printf("可用快照: %d 条\n", snapshot_count());
            printf("检查点:   %d 个\n", snapshot_checkpoint_count());
            break;
        default:
            printf("未知场景: %d\n", scenario);
            break;
    }

    printf("\n══════════════════════════════════════════════════\n");
    printf("  程序正常退出\n");
    printf("  最终快照数: %d\n", snapshot_count());
    printf("══════════════════════════════════════════════════\n");

    crash_handler_uninstall();
    return 0;
}
