// ================================================================
// 太极时光倒流调试器 — 命令行版 (taiji-debug)
// 版本: v1.0
// 日期: 2026-07-03
// 编制: 玄同工作室
//
// 加载任意 C/C++ 二进制程序（需用太极快照运行时编译），
// 提供交互式时光倒流调试功能。
//
// 编译:
//   clang taiji-debug.c snapshot_runtime.c -o taiji-debug
//
// 使用:
//   taiji-debug ./my_program [args...]
//   taiji-debug --attach PID
//   taiji-debug --core core.dump
// ================================================================

#include "snapshot_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#if defined(_WIN32)
    #include <windows.h>
    #include <process.h>
    #include <conio.h>
#else
    #include <unistd.h>
    #include <sys/wait.h>
    #include <sys/ptrace.h>
#endif

// === 版本 ===
#define TAIJI_DEBUG_VERSION "v1.0"
#define TAIJI_DEBUG_DATE    "2026-07-03"

// === 颜色输出 ===
#if defined(_WIN32)
    #define COLOR_RESET   ""
    #define COLOR_RED     ""
    #define COLOR_GREEN   ""
    #define COLOR_YELLOW  ""
    #define COLOR_CYAN    ""
    #define COLOR_BOLD    ""
#else
    #define COLOR_RESET   "\033[0m"
    #define COLOR_RED     "\033[31m"
    #define COLOR_GREEN   "\033[32m"
    #define COLOR_YELLOW  "\033[33m"
    #define COLOR_CYAN    "\033[36m"
    #define COLOR_BOLD    "\033[1m"
#endif

// === 全局状态 ===
static const char* g_target_path = NULL;
static pid_t g_child_pid = 0;
static bool  g_running = false;
static bool  g_attached = false;

// === 前向声明 ===
static void debug_main_loop(void);
static void print_banner(void);
static void print_help(void);
static int  launch_target(int argc, char* argv[]);
static void kill_target(void);

// ================================================================
// 主函数
// ================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_banner();
        fprintf(stderr, "用法: taiji-debug <可执行文件> [参数...]\n");
        fprintf(stderr, "      taiji-debug --help\n");
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_banner();
        print_help();
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("太极时光倒流调试器 %s (%s)\n", TAIJI_DEBUG_VERSION, TAIJI_DEBUG_DATE);
        return 0;
    }

    g_target_path = argv[1];
    print_banner();

    // 初始化快照运行时
    snapshot_init();

    // 安装崩溃处理器
    crash_handler_install();

    // 启动目标程序
    int ret = launch_target(argc, argv);

    if (ret == 0 && g_child_pid > 0) {
        // 进入交互式调试循环
        debug_main_loop();
    }

    // 清理
    crash_handler_uninstall();

    return ret;
}

// ================================================================
// 启动目标程序
// ================================================================
static int launch_target(int argc, char* argv[]) {
#if defined(_WIN32)

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // 构建命令行
    char cmd_line[4096];
    snprintf(cmd_line, sizeof(cmd_line), "\"%s\"", g_target_path);
    for (int i = 2; i < argc; i++) {
        strncat(cmd_line, " ", sizeof(cmd_line) - strlen(cmd_line) - 1);
        strncat(cmd_line, argv[i], sizeof(cmd_line) - strlen(cmd_line) - 1);
    }

    fprintf(stderr, "%s[太极调试]%s 启动目标: %s\n", COLOR_CYAN, COLOR_RESET, cmd_line);

    if (!CreateProcess(
            NULL,           // 应用程序名
            cmd_line,       // 命令行
            NULL, NULL,     // 进程/线程安全属性
            FALSE,          // 不继承句柄
            DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS,  // 调试模式
            NULL, NULL,     // 环境/当前目录
            &si, &pi
        )) {
        fprintf(stderr, "%s[错误]%s 无法启动进程，错误码: %lu\n",
                COLOR_RED, COLOR_RESET, GetLastError());
        return 1;
    }

    g_child_pid = pi.dwProcessId;
    g_running = true;

    fprintf(stderr, "%s[太极调试]%s 进程已启动，PID: %lu\n",
            COLOR_GREEN, COLOR_RESET, (unsigned long)g_child_pid);

    // 进入 Windows 调试事件循环
    DEBUG_EVENT debug_event;
    bool first_break = true;

    while (g_running) {
        if (!WaitForDebugEvent(&debug_event, 100)) {
            // 超时，检查用户输入
            if (_kbhit()) {
                int ch = _getch();
                if (ch == 3) {  // Ctrl+C
                    fprintf(stderr, "\n%s[太极调试]%s 用户中断\n", COLOR_YELLOW, COLOR_RESET);
                    TerminateProcess(pi.hProcess, 0);
                    break;
                }
            }
            continue;
        }

        DWORD continue_status = DBG_CONTINUE;

        switch (debug_event.dwDebugEventCode) {
            case EXCEPTION_DEBUG_EVENT: {
                EXCEPTION_DEBUG_INFO* exc = &debug_event.u.Exception;
                DWORD code = exc->ExceptionRecord.ExceptionCode;

                if (first_break) {
                    // 首次断点：程序入口
                    first_break = false;
                    fprintf(stderr, "%s[太极调试]%s 到达入口点\n",
                            COLOR_GREEN, COLOR_RESET);
                    // 安装快照运行时信号处理器
                    // 注意：这里子进程已经加载了 snapshot_runtime
                    // crash_handler_install() 由子进程在 main() 中调用
                }
                else if (code == EXCEPTION_BREAKPOINT) {
                    // 用户断点
                    fprintf(stderr, "%s[太极调试]%s 断点命中\n",
                            COLOR_YELLOW, COLOR_RESET);
                }
                else {
                    // 真正的异常 — 交由子进程的快照运行时处理
                    fprintf(stderr, "%s[太极调试]%s 异常 0x%08lx，交由快照运行时处理\n",
                            COLOR_RED, COLOR_RESET, code);
                    continue_status = DBG_EXCEPTION_NOT_HANDLED;
                }
                break;
            }

            case CREATE_THREAD_DEBUG_EVENT:
            case CREATE_PROCESS_DEBUG_EVENT:
                break;

            case EXIT_THREAD_DEBUG_EVENT:
            case EXIT_PROCESS_DEBUG_EVENT:
                fprintf(stderr, "%s[太极调试]%s 进程已退出，代码: %lu\n",
                        COLOR_YELLOW, COLOR_RESET,
                        debug_event.u.ExitProcess.dwExitCode);
                g_running = false;
                break;

            case LOAD_DLL_DEBUG_EVENT:
            case UNLOAD_DLL_DEBUG_EVENT:
                break;

            case OUTPUT_DEBUG_STRING_EVENT:
                // 从目标程序发来的调试字符串
                {
                    OUTPUT_DEBUG_STRING_INFO* ods = &debug_event.u.DebugString;
                    char buf[4096];
                    if (ods->fUnicode) {
                        WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)ods->lpDebugStringData,
                                           ods->nDebugStringLength, buf, sizeof(buf), NULL, NULL);
                    } else {
                        memcpy(buf, ods->lpDebugStringData,
                               ods->nDebugStringLength < sizeof(buf) ? ods->nDebugStringLength : sizeof(buf)-1);
                        buf[ods->nDebugStringLength] = '\0';
                    }
                    fprintf(stderr, "%s", buf);
                }
                break;

            default:
                break;
        }

        ContinueDebugEvent(debug_event.dwProcessId, debug_event.dwThreadId, continue_status);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

#else  // POSIX

    g_child_pid = fork();

    if (g_child_pid == 0) {
        // 子进程：执行目标程序
        // 请求父进程跟踪
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);

        // 执行目标
        execv(g_target_path, &argv[1]);

        // execv 失败
        perror("execv");
        _exit(1);

    } else if (g_child_pid > 0) {
        // 父进程：调试循环
        fprintf(stderr, "%s[太极调试]%s 子进程已启动，PID: %d\n",
                COLOR_GREEN, COLOR_RESET, g_child_pid);

        g_running = true;
        int status;

        // 等待第一个信号（exec 后的 SIGTRAP）
        waitpid(g_child_pid, &status, 0);
        if (WIFSTOPPED(status)) {
            fprintf(stderr, "%s[太极调试]%s 到达入口点\n", COLOR_GREEN, COLOR_RESET);
            ptrace(PTRACE_CONT, g_child_pid, NULL, NULL);
        }

        // 主循环：监控子进程状态
        while (g_running) {
            pid_t result = waitpid(g_child_pid, &status, WNOHANG);

            if (result == 0) {
                // 子进程仍在运行
                usleep(100000);  // 100ms
                continue;
            } else if (result < 0) {
                perror("waitpid");
                g_running = false;
            } else {
                // 子进程状态变化
                if (WIFEXITED(status)) {
                    fprintf(stderr, "%s[太极调试]%s 子进程已退出，代码: %d\n",
                            COLOR_YELLOW, COLOR_RESET, WEXITSTATUS(status));
                    g_running = false;
                } else if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    fprintf(stderr, "%s[太极调试]%s 子进程被信号 %d 终止\n",
                            COLOR_RED, COLOR_RESET, sig);
                    // 核心转储时快照运行时已处理
                    g_running = false;
                } else if (WIFSTOPPED(status)) {
                    int sig = WSTOPSIG(status);
                    if (sig == SIGTRAP) {
                        fprintf(stderr, "%s[太极调试]%s 断点命中\n",
                                COLOR_YELLOW, COLOR_RESET);
                    }
                    ptrace(PTRACE_CONT, g_child_pid, NULL, NULL);
                }
            }
        }
    } else {
        perror("fork");
        return 1;
    }

#endif

    return 0;
}

// ================================================================
// 交互式调试主循环
// ================================================================
static void debug_main_loop(void) {
    char line[1024];

    fprintf(stderr, "\n%s══════════════════════════════════════════%s\n",
            COLOR_CYAN, COLOR_RESET);
    fprintf(stderr, "%s  太极时光倒流调试器 — 交互模式  %s\n",
            COLOR_BOLD, COLOR_RESET);
    fprintf(stderr, "%s══════════════════════════════════════════%s\n",
            COLOR_CYAN, COLOR_RESET);
    fprintf(stderr, "  输入 h 查看命令列表\n\n");

    fprintf(stderr, "[taiji] > ");
    fflush(stderr);

    while (g_running && fgets(line, sizeof(line), stdin)) {
        // 去除换行
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) {
            fprintf(stderr, "[taiji] > ");
            fflush(stderr);
            continue;
        }

        // 解析命令
        char cmd[32] = {0};
        int arg_i = 0;
        unsigned long long arg_ull = 0;
        sscanf(line, "%31s %d", cmd, &arg_i);
        sscanf(line, "%*s %llx", &arg_ull);

        // === 命令分发 ===

        if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0 ||
            strcmp(cmd, "exit") == 0) {
            fprintf(stderr, "%s[taiji]%s 退出调试器\n", COLOR_YELLOW, COLOR_RESET);
            kill_target();
            g_running = false;
            break;
        }

        else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0) {
            print_help();
        }

        else if (strcmp(cmd, "r") == 0 || strcmp(cmd, "run") == 0) {
            // 继续执行
            fprintf(stderr, "%s[taiji]%s 继续执行...\n", COLOR_GREEN, COLOR_RESET);
#if defined(_WIN32)
            DebugBreakProcess(OpenProcess(PROCESS_ALL_ACCESS, FALSE, g_child_pid));
#endif
        }

        else if (strcmp(cmd, "w") == 0) {
            // 回溯 N 步
            int steps = (arg_i > 0) ? arg_i : 1;
            int done = snapshot_rewind(steps);
            fprintf(stderr, "%s[taiji]%s 已回溯 %d 步，剩余 %d 条快照\n",
                    COLOR_GREEN, COLOR_RESET, done, snapshot_count());
        }

        else if (strcmp(cmd, "wc") == 0) {
            // 回溯到检查点
            int cp = (arg_i >= 0) ? arg_i : -1;
            if (snapshot_rewind_to_checkpoint(cp)) {
                fprintf(stderr, "%s[taiji]%s 已恢复到检查点 #%d\n",
                        COLOR_GREEN, COLOR_RESET, cp);
            } else {
                fprintf(stderr, "%s[taiji]%s 恢复检查点失败\n",
                        COLOR_RED, COLOR_RESET);
            }
        }

        else if (strcmp(cmd, "s") == 0 || strcmp(cmd, "snap") == 0) {
            int show_n = (arg_i > 0) ? arg_i : 20;
            SnapshotSummary summaries[100];
            int n = snapshot_list_snapshots(summaries, show_n < 100 ? show_n : 100);

            fprintf(stderr, "\n══ 最近 %d 条快照 ══\n", n);
            fprintf(stderr, "编号  地址              旧值              PC\n");
            fprintf(stderr, "────  ────────────────  ────────────────  ────────────────\n");
            for (int i = 0; i < n; i++) {
                fprintf(stderr, "%4d  %16p  %16llx  0x%llx [%d]\n",
                        summaries[i].index,
                        summaries[i].addr,
                        (unsigned long long)summaries[i].old_value,
                        (unsigned long long)summaries[i].pc,
                        summaries[i].size);
            }
        }

        else if (strcmp(cmd, "c") == 0 || strcmp(cmd, "check") == 0) {
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

        else if (strcmp(cmd, "i") == 0 || strcmp(cmd, "info") == 0) {
            fprintf(stderr, "\n══ 太极调试器状态 ══\n");
            fprintf(stderr, "  目标程序: %s\n", g_target_path);
            fprintf(stderr, "  子进程PID: %d\n", g_child_pid);
            fprintf(stderr, "  运行状态: %s\n", g_running ? "运行中" : "已停止");
            fprintf(stderr, "  快照系统: %s\n", snapshot_debug_info());
            fprintf(stderr, "  缓冲区使用: %.1f%%\n", snapshot_buffer_usage() * 100.0);
        }

        else if (strcmp(cmd, "p") == 0) {
            if (snapshot_is_paused()) {
                snapshot_resume();
                fprintf(stderr, "%s[taiji]%s 快照记录已恢复\n", COLOR_GREEN, COLOR_RESET);
            } else {
                snapshot_pause();
                fprintf(stderr, "%s[taiji]%s 快照记录已暂停\n", COLOR_YELLOW, COLOR_RESET);
            }
        }

        else {
            fprintf(stderr, "%s[taiji]%s 未知命令: %s (输入 h 查看帮助)\n",
                    COLOR_RED, COLOR_RESET, cmd);
        }

        fprintf(stderr, "\n[taiji] > ");
        fflush(stderr);
    }
}

// ================================================================
// 终止目标进程
// ================================================================
static void kill_target(void) {
    if (g_child_pid <= 0) return;

#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, g_child_pid);
    if (h) {
        TerminateProcess(h, 0);
        CloseHandle(h);
    }
#else
    kill(g_child_pid, SIGKILL);
    waitpid(g_child_pid, NULL, 0);
#endif
    g_child_pid = 0;
}

// ================================================================
// 横幅
// ================================================================
static void print_banner(void) {
    printf("\n");
    printf("%s╔══════════════════════════════════════════════════╗%s\n",
           COLOR_CYAN, COLOR_RESET);
    printf("%s║%s     %s太极时光倒流调试器%s (Taiji Time-Travel Debugger)     %s║%s\n",
           COLOR_CYAN, COLOR_RESET, COLOR_BOLD, COLOR_RESET, COLOR_CYAN, COLOR_RESET);
    printf("%s║%s           版本 %s  编译 %s                %s║%s\n",
           COLOR_CYAN, COLOR_RESET, TAIJI_DEBUG_VERSION, TAIJI_DEBUG_DATE, COLOR_CYAN, COLOR_RESET);
    printf("%s║%s       玄同工作室 — 太极逆执·二进制世界          %s║%s\n",
           COLOR_CYAN, COLOR_RESET, COLOR_CYAN, COLOR_RESET);
    printf("%s╚══════════════════════════════════════════════════╝%s\n",
           COLOR_CYAN, COLOR_RESET);
    printf("\n");
}

// ================================================================
// 帮助
// ================================================================
static void print_help(void) {
    printf("\n");
    printf("══════════ 太极时光倒流调试器 命令 ══════════\n");
    printf("\n");
    printf("  执行控制:\n");
    printf("    r, run        继续执行目标程序\n");
    printf("    q, quit       退出调试器\n");
    printf("\n");
    printf("  时光倒流:\n");
    printf("    w N           回溯 N 步 (例: w 10)\n");
    printf("    wc N          恢复到检查点 N (例: wc 3)\n");
    printf("    s [N]         显示最近 N 条快照 (默认 20)\n");
    printf("    c             显示所有检查点\n");
    printf("\n");
    printf("  快照控制:\n");
    printf("    p             暂停/恢复快照记录\n");
    printf("\n");
    printf("  信息:\n");
    printf("    i, info       显示调试器状态\n");
    printf("    h, help       显示帮助\n");
    printf("\n");
    printf("  快捷键:\n");
    printf("    Ctrl+C        中断目标程序\n");
    printf("\n");
    printf("═══════════════════════════════════════════════\n");
    printf("\n");
}
