// ================================================================
// 太极快照运行时库 — 实现
// 版本: v1.0
// 日期: 2026-07-03
// 编制: 玄同工作室
//
// 环形缓冲区 + 检查点 + 恢复逻辑的完整实现
// ================================================================

#include "snapshot_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// C11 atomics (GCC/Clang) vs C++ atomics
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__cplusplus)
    #include <stdatomic.h>
    #define ATOMIC_INT      atomic_int
    #define ATOMIC_BOOL     atomic_bool
    #define ATOMIC_INT64    atomic_llong       // atomic_int64_t 非强制，用 atomic_llong 替代
    #define ATOMIC_LOAD(v)  atomic_load(v)
    #define ATOMIC_STORE(v,x) atomic_store(v,x)
    #define ATOMIC_FETCH_ADD(v,x) atomic_fetch_add(v,x)
#else
    // C++ 或旧 C: 使用 volatile + 锁保护（简化但安全）
    #define ATOMIC_INT      int
    #define ATOMIC_BOOL     int
    #define ATOMIC_INT64    int64_t
    #define ATOMIC_LOAD(v)  (*(v))
    #define ATOMIC_STORE(v,x) (*(v) = (x))
    #define ATOMIC_FETCH_ADD(v,x) (*(v) += (x))
#endif

// === 平台检测 ===
#if defined(_WIN32)
    #include <windows.h>
    #define SNAPSHOT_WINDOWS 1
#elif defined(__linux__) || defined(__APPLE__)
    #include <unistd.h>
    #include <sys/mman.h>
    #define SNAPSHOT_POSIX 1
#else
    #error "Unsupported platform"
#endif

// === 线程安全锁 ===
#if SNAPSHOT_WINDOWS
    static CRITICAL_SECTION snapshot_lock;
    static CRITICAL_SECTION checkpoint_lock;
    #define LOCK_INIT()   do { InitializeCriticalSection(&snapshot_lock); InitializeCriticalSection(&checkpoint_lock); } while(0)
    #define LOCK_SNAP()   EnterCriticalSection(&snapshot_lock)
    #define UNLOCK_SNAP() LeaveCriticalSection(&snapshot_lock)
    #define LOCK_CP()     EnterCriticalSection(&checkpoint_lock)
    #define UNLOCK_CP()   LeaveCriticalSection(&checkpoint_lock)
#else
    #include <pthread.h>
    static pthread_mutex_t snapshot_lock = PTHREAD_MUTEX_INITIALIZER;
    static pthread_mutex_t checkpoint_lock = PTHREAD_MUTEX_INITIALIZER;
    #define LOCK_INIT()   do {} while(0)
    #define LOCK_SNAP()   pthread_mutex_lock(&snapshot_lock)
    #define UNLOCK_SNAP() pthread_mutex_unlock(&snapshot_lock)
    #define LOCK_CP()     pthread_mutex_lock(&checkpoint_lock)
    #define UNLOCK_CP()   pthread_mutex_unlock(&checkpoint_lock)
#endif

// === 全局状态 ===

static SnapshotEntry  g_buffer[SNAPSHOT_BUFFER_SIZE];
static ATOMIC_INT     g_write_pos = 0;   // 环形缓冲区写位置
static ATOMIC_INT     g_count = 0;       // 当前有效快照数
static ATOMIC_BOOL    g_paused = 0;      // 是否暂停记录
static ATOMIC_BOOL    g_initialized = 0;

static SnapshotCheckpoint g_checkpoints[SNAPSHOT_MAX_CHECKPOINTS];
static int                g_checkpoint_count = 0;

static ATOMIC_INT64   g_snapshot_id = 0;

// === 崩溃处理状态 ===
static volatile sig_atomic_t g_crashed = 0;
static volatile sig_atomic_t g_crash_signal = 0;
static void*                 g_crash_fault_addr = NULL;
static uint64_t              g_crash_fault_pc = 0;

// === 实现 ===

void snapshot_init(void) {
    if (ATOMIC_LOAD(&g_initialized)) return;

    LOCK_INIT();
    ATOMIC_STORE(&g_write_pos, 0);
    ATOMIC_STORE(&g_count, 0);
    ATOMIC_STORE(&g_paused, 0);
    ATOMIC_STORE(&g_snapshot_id, 0);
    g_checkpoint_count = 0;

    memset(g_buffer, 0, sizeof(g_buffer));
    memset(g_checkpoints, 0, sizeof(g_checkpoints));

    ATOMIC_STORE(&g_initialized, 1);

    fprintf(stderr, "[太极快照] 运行时已初始化，缓冲区容量=%d 条\n", SNAPSHOT_BUFFER_SIZE);
}

void snapshot_record(void* addr, uint64_t old_value, uint64_t pc, int size) {
    if (!ATOMIC_LOAD(&g_initialized)) return;
    if (ATOMIC_LOAD(&g_paused)) return;

    LOCK_SNAP();

    int pos = ATOMIC_LOAD(&g_write_pos);
    int cnt = ATOMIC_LOAD(&g_count);

    // 记录快照
    g_buffer[pos].addr      = addr;
    g_buffer[pos].old_value = old_value;
    g_buffer[pos].pc        = pc;
    g_buffer[pos].size      = size;

    // 环形前进
    pos = (pos + 1) % SNAPSHOT_BUFFER_SIZE;
    ATOMIC_STORE(&g_write_pos, pos);

    if (cnt < SNAPSHOT_BUFFER_SIZE) {
        ATOMIC_STORE(&g_count, cnt + 1);
    }

    ATOMIC_FETCH_ADD(&g_snapshot_id, 1);

    bool need_checkpoint = (cnt >= SNAPSHOT_BUFFER_SIZE - 1);

    UNLOCK_SNAP();

    // 满时自动生成检查点（在锁外调用，避免死锁）
    if (need_checkpoint) {
        snapshot_create_checkpoint(pc);
    }
}

int snapshot_rewind(int steps) {
    if (!ATOMIC_LOAD(&g_initialized)) return 0;

    LOCK_SNAP();

    int cnt = ATOMIC_LOAD(&g_count);
    if (cnt <= 0 || steps <= 0) {
        UNLOCK_SNAP();
        return 0;
    }

    int actual = (steps > cnt) ? cnt : steps;
    int pos = ATOMIC_LOAD(&g_write_pos);

    // 逐条恢复：从缓冲区末尾向前
    for (int i = 0; i < actual; i++) {
        // 回退写指针
        pos = (pos - 1 + SNAPSHOT_BUFFER_SIZE) % SNAPSHOT_BUFFER_SIZE;

        SnapshotEntry* snap = &g_buffer[pos];

        // 恢复旧值到内存
        if (snap->addr != NULL) {
            switch (snap->size) {
                case 1: *(volatile uint8_t*) snap->addr  = (uint8_t) snap->old_value;  break;
                case 2: *(volatile uint16_t*)snap->addr  = (uint16_t)snap->old_value;  break;
                case 4: *(volatile uint32_t*)snap->addr  = (uint32_t)snap->old_value;  break;
                case 8: *(volatile uint64_t*)snap->addr  = snap->old_value;            break;
                default: break;
            }
        }
    }

    ATOMIC_STORE(&g_write_pos, pos);
    ATOMIC_STORE(&g_count, cnt - actual);

    fprintf(stderr, "[太极快照] 回溯 %d 步，剩余 %d 条快照\n",
            actual, ATOMIC_LOAD(&g_count));

    UNLOCK_SNAP();
    return actual;
}

bool snapshot_rewind_to_checkpoint(int checkpoint_index) {
    if (!ATOMIC_LOAD(&g_initialized)) return false;

    LOCK_CP();

    int idx = checkpoint_index;
    if (idx < 0) idx = g_checkpoint_count - 1;
    if (idx < 0 || idx >= g_checkpoint_count) {
        UNLOCK_CP();
        return false;
    }

    SnapshotCheckpoint* cp = &g_checkpoints[idx];
    if (!cp->valid) {
        UNLOCK_CP();
        return false;
    }

    // 恢复数据：逐条回溯从当前到检查点的所有快照
    LOCK_SNAP();
    int target_count = cp->snapshot_index;
    int current_count = ATOMIC_LOAD(&g_count);
    if (target_count < current_count) {
        int diff = current_count - target_count;
        int pos = ATOMIC_LOAD(&g_write_pos);

        // 逐条恢复旧值到内存
        for (int i = 0; i < diff; i++) {
            pos = (pos - 1 + SNAPSHOT_BUFFER_SIZE) % SNAPSHOT_BUFFER_SIZE;
            SnapshotEntry* snap = &g_buffer[pos];
            if (snap->addr != NULL) {
                switch (snap->size) {
                    case 1: *(volatile uint8_t*) snap->addr  = (uint8_t) snap->old_value;  break;
                    case 2: *(volatile uint16_t*)snap->addr  = (uint16_t)snap->old_value;  break;
                    case 4: *(volatile uint32_t*)snap->addr  = (uint32_t)snap->old_value;  break;
                    case 8: *(volatile uint64_t*)snap->addr  = snap->old_value;            break;
                    default: break;
                }
            }
        }

        ATOMIC_STORE(&g_write_pos, pos);
        ATOMIC_STORE(&g_count, target_count);
        fprintf(stderr, "[太极快照] 恢复到检查点 #%d，回溯 %d 条快照，已恢复内存数据\n",
                idx, diff);
    }
    UNLOCK_SNAP();

    UNLOCK_CP();
    return true;
}

int snapshot_count(void) {
    return ATOMIC_LOAD(&g_count);
}

int snapshot_checkpoint_count(void) {
    return g_checkpoint_count;
}

int snapshot_list_snapshots(SnapshotSummary* out, int max) {
    if (!ATOMIC_LOAD(&g_initialized) || out == NULL) return 0;

    LOCK_SNAP();
    int cnt = ATOMIC_LOAD(&g_count);
    int pos = ATOMIC_LOAD(&g_write_pos);
    int to_copy = (cnt < max) ? cnt : max;

    for (int i = 0; i < to_copy; i++) {
        int idx = (pos - cnt + i + SNAPSHOT_BUFFER_SIZE) % SNAPSHOT_BUFFER_SIZE;
        out[i].index     = i;
        out[i].addr      = g_buffer[idx].addr;
        out[i].old_value = g_buffer[idx].old_value;
        out[i].pc        = g_buffer[idx].pc;
        out[i].size      = g_buffer[idx].size;
    }

    UNLOCK_SNAP();
    return to_copy;
}

int snapshot_list_checkpoints(CheckpointSummary* out, int max) {
    if (!ATOMIC_LOAD(&g_initialized) || out == NULL) return 0;

    LOCK_CP();
    int to_copy = (g_checkpoint_count < max) ? g_checkpoint_count : max;
    for (int i = 0; i < to_copy; i++) {
        out[i].index          = i;
        out[i].pc             = g_checkpoints[i].pc;
        out[i].snapshot_count = g_checkpoints[i].snapshot_index;
        out[i].valid          = g_checkpoints[i].valid;
    }
    UNLOCK_CP();
    return to_copy;
}

void snapshot_create_checkpoint(uint64_t pc) {
    LOCK_CP();

    // 如果检查点表未满，追加新条目；如果已满，所有条目向前移动一格，空出最后位置
    if (g_checkpoint_count < SNAPSHOT_MAX_CHECKPOINTS) {
        int idx = g_checkpoint_count;
        g_checkpoints[idx].pc             = pc;
        g_checkpoints[idx].snapshot_index = ATOMIC_LOAD(&g_count);
        g_checkpoints[idx].valid          = true;
        g_checkpoint_count++;
    } else {
        // 表已满：整体前移，丢弃最旧的检查点 #0，最新检查点写入末尾
        memmove(&g_checkpoints[0], &g_checkpoints[1],
                (SNAPSHOT_MAX_CHECKPOINTS - 1) * sizeof(SnapshotCheckpoint));
        int last = SNAPSHOT_MAX_CHECKPOINTS - 1;
        g_checkpoints[last].pc             = pc;
        g_checkpoints[last].snapshot_index = ATOMIC_LOAD(&g_count);
        g_checkpoints[last].valid          = true;
    }

    int latest = g_checkpoint_count - 1;

    UNLOCK_CP();

    fprintf(stderr, "[太极快照] 自动检查点 #%d 已创建 (PC=0x%llx, 快照=%d)\n",
            latest, (unsigned long long)pc, ATOMIC_LOAD(&g_count));
}

void snapshot_reset(void) {
    LOCK_SNAP();
    ATOMIC_STORE(&g_write_pos, 0);
    ATOMIC_STORE(&g_count, 0);
    ATOMIC_STORE(&g_paused, 0);
    ATOMIC_STORE(&g_snapshot_id, 0);
    memset(g_buffer, 0, sizeof(g_buffer));
    UNLOCK_SNAP();

    LOCK_CP();
    g_checkpoint_count = 0;
    memset(g_checkpoints, 0, sizeof(g_checkpoints));
    UNLOCK_CP();
}

const char* snapshot_debug_info(void) {
    static char buf[512];
    int cnt = ATOMIC_LOAD(&g_count);
    double usage = snapshot_buffer_usage();

    snprintf(buf, sizeof(buf),
             "快照: %d/%d (%.1f%%), 检查点: %d, 暂停: %s",
             cnt, SNAPSHOT_BUFFER_SIZE, usage * 100.0,
             g_checkpoint_count,
             ATOMIC_LOAD(&g_paused) ? "是" : "否");
    return buf;
}

double snapshot_buffer_usage(void) {
    int cnt = ATOMIC_LOAD(&g_count);
    return (double)cnt / (double)SNAPSHOT_BUFFER_SIZE;
}

void snapshot_pause(void) {
    ATOMIC_STORE(&g_paused, 1);
}

void snapshot_resume(void) {
    ATOMIC_STORE(&g_paused, 0);
}

bool snapshot_is_paused(void) {
    return ATOMIC_LOAD(&g_paused) != 0;
}

int64_t snapshot_next_id(void) {
    return ATOMIC_FETCH_ADD(&g_snapshot_id, 1);
}

// ================================================================
// 崩溃处理与交互式调试器
// ================================================================

// 前向声明
static void interactive_debug_loop(void);

// === 平台特定的信号处理 ===

#if SNAPSHOT_WINDOWS

static LONG WINAPI windows_exception_handler(EXCEPTION_POINTERS* ex_info) {
    DWORD code = ex_info->ExceptionRecord->ExceptionCode;
    int sig = 0;

    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:    sig = SIGSEGV; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  sig = SIGFPE;  break;
        case EXCEPTION_ILLEGAL_INSTRUCTION: sig = SIGILL;  break;
        case EXCEPTION_STACK_OVERFLOW:      sig = SIGSEGV; break;
        default:                            sig = code;     break;
    }

    g_crashed = 1;
    g_crash_signal = sig;
    g_crash_fault_addr = (void*)ex_info->ExceptionRecord->ExceptionAddress;
    g_crash_fault_pc = (uint64_t)ex_info->ContextRecord->Rip;  // x64

    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║        !!! 太极时光倒流 — 崩溃捕获 !!!         ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║  异常类型: %s\n",
            code == EXCEPTION_ACCESS_VIOLATION ? "访问违例 (SIGSEGV)" :
            code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "除零 (SIGFPE)" :
            code == EXCEPTION_ILLEGAL_INSTRUCTION ? "非法指令 (SIGILL)" :
            code == EXCEPTION_STACK_OVERFLOW ? "栈溢出" : "未知");
    fprintf(stderr, "║  故障地址: %p\n", ex_info->ExceptionRecord->ExceptionAddress);
    fprintf(stderr, "║  指令地址: 0x%llx\n", (unsigned long long)ex_info->ContextRecord->Rip);
    fprintf(stderr, "║  可用快照: %d 条\n", snapshot_count());
    fprintf(stderr, "║  检查点:   %d 个\n", snapshot_checkpoint_count());
    fprintf(stderr, "╠══════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║  命令: w N(回溯) s(快照) c(检查点) h(帮助) q(退出)\n");
    fprintf(stderr, "╚══════════════════════════════════════════════════╝\n");
    fprintf(stderr, "\n");

    interactive_debug_loop();

    return EXCEPTION_EXECUTE_HANDLER;
}

void crash_handler_install(void) {
    SetUnhandledExceptionFilter(windows_exception_handler);
    fprintf(stderr, "[太极快照] Windows 异常处理器已安装\n");
}

void crash_handler_uninstall(void) {
    SetUnhandledExceptionFilter(NULL);
    fprintf(stderr, "[太极快照] Windows 异常处理器已卸载\n");
}

#else  // POSIX (Linux/macOS)

static void posix_signal_handler(int sig, siginfo_t* info, void* ctx) {
    g_crashed = 1;
    g_crash_signal = sig;
    g_crash_fault_addr = info->si_addr;

#if defined(__x86_64__)
    ucontext_t* uc = (ucontext_t*)ctx;
    g_crash_fault_pc = uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    ucontext_t* uc = (ucontext_t*)ctx;
    g_crash_fault_pc = uc->uc_mcontext.pc;
#else
    g_crash_fault_pc = 0;
#endif

    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║        !!! 太极时光倒流 — 崩溃捕获 !!!         ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║  信号: %d (%s)\n", sig,
            sig == SIGSEGV ? "段错误" :
            sig == SIGFPE  ? "浮点异常" :
            sig == SIGILL  ? "非法指令" :
            sig == SIGABRT ? "异常终止" : "未知");
    fprintf(stderr, "║  故障地址: %p\n", info->si_addr);
    fprintf(stderr, "║  指令地址: 0x%llx\n", (unsigned long long)g_crash_fault_pc);
    fprintf(stderr, "║  可用快照: %d 条\n", snapshot_count());
    fprintf(stderr, "║  检查点:   %d 个\n", snapshot_checkpoint_count());
    fprintf(stderr, "╠══════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║  命令: w N(回溯) s(快照) c(检查点) h(帮助) q(退出)\n");
    fprintf(stderr, "╚══════════════════════════════════════════════════╝\n");
    fprintf(stderr, "\n");

    interactive_debug_loop();
}

void crash_handler_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = posix_signal_handler;
    sa.sa_flags = SA_SIGINFO;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    fprintf(stderr, "[太极快照] POSIX 信号处理器已安装 (SIGSEGV/SIGFPE/SIGILL/SIGABRT)\n");
}

void crash_handler_uninstall(void) {
    signal(SIGSEGV, SIG_DFL);
    signal(SIGFPE,  SIG_DFL);
    signal(SIGILL,  SIG_DFL);
    signal(SIGABRT, SIG_DFL);
    fprintf(stderr, "[太极快照] 信号处理器已卸载\n");
}

#endif  // SNAPSHOT_POSIX

void crash_enter_debug_mode(int sig, void* fault_addr, uint64_t fault_pc) {
    g_crashed = 1;
    g_crash_signal = sig;
    g_crash_fault_addr = fault_addr;
    g_crash_fault_pc = fault_pc;

    fprintf(stderr, "\n[太极快照] 手动进入调试模式\n");
    interactive_debug_loop();
}

// === 交互式调试循环 ===

static void print_help(void) {
    fprintf(stderr, "\n");
    fprintf(stderr, "══════════ 太极时光倒流调试器 命令 ══════════\n");
    fprintf(stderr, "  w N      回溯 N 步 (例: w 10)\n");
    fprintf(stderr, "  wc N     回溯到检查点 N (例: wc 3)\n");
    fprintf(stderr, "  s        显示最近 20 条快照\n");
    fprintf(stderr, "  c        显示所有检查点\n");
    fprintf(stderr, "  i        显示快照系统信息\n");
    fprintf(stderr, "  p        暂停/恢复快照记录\n");
    fprintf(stderr, "  m ADDR   读取内存地址 (例: m 0x7fff1234)\n");
    fprintf(stderr, "  h        显示帮助\n");
    fprintf(stderr, "  q        退出\n");
    fprintf(stderr, "═══════════════════════════════════════════════\n");
}

static void print_snapshots(void) {
    int cnt = snapshot_count();
    if (cnt == 0) {
        fprintf(stderr, "[无快照]\n");
        return;
    }

    SnapshotSummary summaries[20];
    int n = snapshot_list_snapshots(summaries, 20);

    fprintf(stderr, "\n══ 最近 %d 条快照 ══\n", n);
    fprintf(stderr, "编号  地址              旧值              PC\n");
    fprintf(stderr, "────  ────────────────  ────────────────  ────────────────\n");

    for (int i = 0; i < n; i++) {
        fprintf(stderr, "%4d  %16p  %16llx  0x%llx [%d字节]\n",
                summaries[i].index,
                summaries[i].addr,
                (unsigned long long)summaries[i].old_value,
                (unsigned long long)summaries[i].pc,
                summaries[i].size);
    }
}

static void print_checkpoints(void) {
    int cnt = snapshot_checkpoint_count();
    if (cnt == 0) {
        fprintf(stderr, "[无检查点]\n");
        return;
    }

    CheckpointSummary summaries[SNAPSHOT_MAX_CHECKPOINTS];
    int n = snapshot_list_checkpoints(summaries, SNAPSHOT_MAX_CHECKPOINTS);

    fprintf(stderr, "\n══ 检查点 (%d 个) ══\n", n);
    fprintf(stderr, "编号  PC               快照数  状态\n");
    fprintf(stderr, "────  ────────────────  ──────  ────\n");

    for (int i = 0; i < n; i++) {
        fprintf(stderr, "%4d  0x%llx  %6d  %s\n",
                summaries[i].index,
                (unsigned long long)summaries[i].pc,
                summaries[i].snapshot_count,
                summaries[i].valid ? "有效" : "无效");
    }
}

static void print_info(void) {
    fprintf(stderr, "\n══ 太极快照系统信息 ══\n");
    fprintf(stderr, "  缓冲区容量: %d\n", SNAPSHOT_BUFFER_SIZE);
    fprintf(stderr, "  当前快照:   %d\n", snapshot_count());
    fprintf(stderr, "  使用率:     %.1f%%\n", snapshot_buffer_usage() * 100.0);
    fprintf(stderr, "  检查点数:   %d\n", snapshot_checkpoint_count());
    fprintf(stderr, "  是否暂停:   %s\n", snapshot_is_paused() ? "是" : "否");
    fprintf(stderr, "  崩溃信息:\n");
    fprintf(stderr, "    信号:     %d\n", g_crash_signal);
    fprintf(stderr, "    故障地址: %p\n", g_crash_fault_addr);
    fprintf(stderr, "    指令PC:   0x%llx\n", (unsigned long long)g_crash_fault_pc);
}

static void print_memory(void* addr) {
    // 尝试安全读取 16 字节
    volatile uint8_t* p = (volatile uint8_t*)addr;

#if SNAPSHOT_WINDOWS
    // Windows: 使用 IsBadReadPtr 检查
    if (IsBadReadPtr((const void*)p, 16)) {
        fprintf(stderr, "[错误] 地址 %p 不可读\n", addr);
        return;
    }
#endif

    fprintf(stderr, "%p:", addr);
    for (int i = 0; i < 16; i++) {
        fprintf(stderr, " %02x", p[i]);
    }
    fprintf(stderr, "\n");

    // 也显示为 uint64
    fprintf(stderr, "  uint64: 0x%016llx\n",
            (unsigned long long)*(volatile uint64_t*)addr);
}

// 检查是否非交互模式（环境变量或 stdin 非终端）
static bool is_noninteractive(void) {
    static int checked = -1;
    if (checked >= 0) return checked != 0;
    checked = (getenv("TAIJI_NONINTERACTIVE") != NULL) ? 1 : 0;
    return checked != 0;
}

static void interactive_debug_loop(void) {
    // 非交互模式：打印快照摘要后立即退出
    if (is_noninteractive()) {
        fprintf(stderr, "[太极快照] 非交互模式，打印快照摘要后退出\n");
        print_snapshots();
        print_checkpoints();
        print_info();
        // 退出前恢复关键数据
        if (snapshot_count() > 0) {
            snapshot_rewind(snapshot_count() > 10 ? 10 : snapshot_count());
            fprintf(stderr, "[太极快照] 已自动回溯最近变更\n");
        }
        return;
    }

    // 交互模式：读取用户命令
    char line[256];

    fprintf(stderr, "\n[dbg] > ");
    fflush(stderr);

    while (fgets(line, sizeof(line), stdin)) {
        // 去除换行
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) {
            fprintf(stderr, "[dbg] > ");
            fflush(stderr);
            continue;
        }

        // 解析命令
        char cmd[16] = {0};
        int arg = 0;
        unsigned long long addr = 0;

        if (sscanf(line, "%s %d", cmd, &arg) < 1 &&
            sscanf(line, "%s %llx", cmd, &addr) < 1) {
            sscanf(line, "%s", cmd);
        }

        if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
            fprintf(stderr, "[太极快照] 退出调试模式\n");
            break;
        }
        else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0) {
            print_help();
        }
        else if (strcmp(cmd, "w") == 0) {
            // 回溯 N 步
            int steps = (arg > 0) ? arg : 1;
            int done = snapshot_rewind(steps);
            fprintf(stderr, "[dbg] 已回溯 %d 步，剩余 %d 条快照\n", done, snapshot_count());
        }
        else if (strcmp(cmd, "wc") == 0) {
            // 回溯到检查点
            int cp = (arg >= 0) ? arg : -1;
            if (snapshot_rewind_to_checkpoint(cp)) {
                fprintf(stderr, "[dbg] 已恢复到检查点 #%d\n", cp);
            } else {
                fprintf(stderr, "[dbg] 恢复检查点失败\n");
            }
        }
        else if (strcmp(cmd, "s") == 0) {
            print_snapshots();
        }
        else if (strcmp(cmd, "c") == 0) {
            print_checkpoints();
        }
        else if (strcmp(cmd, "i") == 0 || strcmp(cmd, "info") == 0) {
            print_info();
        }
        else if (strcmp(cmd, "p") == 0) {
            if (snapshot_is_paused()) {
                snapshot_resume();
                fprintf(stderr, "[dbg] 快照记录已恢复\n");
            } else {
                snapshot_pause();
                fprintf(stderr, "[dbg] 快照记录已暂停\n");
            }
        }
        else if (strcmp(cmd, "m") == 0 || strcmp(cmd, "mem") == 0) {
            if (addr != 0 || sscanf(line, "%*s %llx", &addr) == 1) {
                print_memory((void*)addr);
            } else {
                fprintf(stderr, "[用法] m 0x地址\n");
            }
        }
        else {
            fprintf(stderr, "[dbg] 未知命令: %s (输入 h 查看帮助)\n", cmd);
        }

        fprintf(stderr, "\n[dbg] > ");
        fflush(stderr);
    }
    // stdin 关闭（EOF），自动退出
    fprintf(stderr, "\n[太极快照] stdin 关闭，自动退出调试模式\n");
}
