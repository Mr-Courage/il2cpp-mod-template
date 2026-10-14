// exit_guard.cpp — 见 exit_guard.h 的说明。
// ============================================================================
// 本文件只做"检测游戏要退出了"这件事，且必须满足手动映射的硬约束：
//   · 不用 STL / 异常 / RTTI；线程用 CreateThread；状态用 volatile LONG + Interlocked
//   · 不碰游戏的对象状态（除了 SetWindowLongPtr 换窗口过程这一处写操作）
// ============================================================================
#include "exit_guard.h"

#include <windows.h>

// main.cpp 的日志内核（双写 文件 + 面板环形缓冲）
extern void LogRaw(const char *fmt, ...);

namespace
{

ModExitCleanupFn g_handler = nullptr;

volatile LONG g_installed  = 0;   // 1 = 窗口过程已替换
volatile LONG g_fired      = 0;   // 1 = 清理已触发（一次性）
volatile LONG g_watchStop  = 0;   // 1 = 让看门狗退出

HWND    g_hwnd      = nullptr;
WNDPROC g_origProc  = nullptr;    // 游戏自己的窗口过程（我们要转发给它）
HANDLE  g_watch     = nullptr;
DWORD   g_watchTid  = 0;

// 清理线程体：跑真正的清理（内部会有若干秒等待，所以绝不能放在游戏消息线程上）
DWORD WINAPI CleanupThread(LPVOID)
{
    LogRaw("[exit] 清理线程启动 tid=%lu", (unsigned long)GetCurrentThreadId());
    if (g_handler)
        g_handler();
    LogRaw("[exit] 清理线程结束");
    return 0;
}

// 触发清理：**只做一次**。真正的清理放独立线程 ——
// 绝不能占用游戏消息线程（那是游戏退出的关键路径，占用它 = 换一种方式卡死）。
void Trigger(const char *why)
{
    if (InterlockedExchange(&g_fired, 1) == 1)
        return;                                  // 已经触发过，直接返回
    LogRaw("[exit] 检测到游戏退出信号（%s），启动清理线程", why ? why : "?");

    if (!g_handler)
    {
        LogRaw("[exit] WARN: 未注册清理回调，无法自动清理");
        return;
    }
    HANDLE th = CreateThread(nullptr, 0, CleanupThread, nullptr, 0, nullptr);
    if (!th)
        LogRaw("[exit] WARN: 清理线程创建失败 err=%lu", (unsigned long)GetLastError());
    // 句柄不关：让它跑完自然回收（进程马上就要没了，不做 CloseHandle 也无害）
    (void)th;
}

// 替换后的窗口过程。**任何分支都必须转发给原过程**，否则游戏自己的窗口逻辑坏掉。
LRESULT CALLBACK ExitWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CLOSE:            Trigger("WM_CLOSE");            break;  // 点 X / Alt+F4
    case WM_DESTROY:          Trigger("WM_DESTROY");          break;
    case WM_QUERYENDSESSION:  Trigger("WM_QUERYENDSESSION");  break;  // 注销/关机
    case WM_ENDSESSION:       Trigger("WM_ENDSESSION");       break;
    case WM_NCDESTROY:        Trigger("WM_NCDESTROY");        break;  // 窗口彻底销毁（最后一道）
    default: break;
    }
    if (g_origProc)
        return CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 看门狗：兜住"窗口被以别的方式销毁"（比如游戏自己 DestroyWindow 而不发 WM_CLOSE）。
// 只读 IsWindow，对游戏零侵入。
DWORD WINAPI WatchdogThread(LPVOID)
{
    g_watchTid = GetCurrentThreadId();
    LogRaw("[exit] 看门狗启动（tid=%lu，监视 hwnd=%p）", (unsigned long)g_watchTid, (void *)g_hwnd);

    for (;;)
    {
        if (g_watchStop) break;
        HWND h = g_hwnd;
        if (h && !IsWindow(h))
        {
            Trigger("窗口已销毁(IsWindow=false)");
            break;
        }
        Sleep(100);
    }
    LogRaw("[exit] 看门狗退出");
    g_watchTid = 0;
    return 0;
}

} // namespace

void ExitGuard_SetHandler(ModExitCleanupFn fn) { g_handler = fn; }

void ExitGuard_AttachToWindow(HWND hwnd)
{
    if (!hwnd)
    {
        // 窗口句柄拿不到（BindToChain 里 GetDesc/GetForegroundWindow 都失败）。
        // 不打日志的话，表现是"点 X 还是卡死但看不出为什么"，所以这里必须喊一声。
        LogRaw("[exit] WARN: hwnd 为空 —— 退出守护未安装（点 X 仍可能卡死）");
        return;
    }
    if (InterlockedExchange(&g_installed, 1) == 1) return;   // 幂等

    g_hwnd = hwnd;

    // ① 子类化：保存原过程，换成我们的。
    //    用 SetWindowLongPtrW，取返回值前先清 GetLastError，因为"原过程恰好是 0"
    //    与"调用失败"都返回 0，只能靠 GetLastError 区分。
    SetLastError(0);
    LONG_PTR prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&ExitWndProc);
    if (!prev && GetLastError() != 0)
    {
        LogRaw("[exit] WARN: 子类化窗口过程失败 err=%lu（退化为只看门狗）",
               (unsigned long)GetLastError());
        g_origProc = nullptr;
    }
    else
    {
        g_origProc = (WNDPROC)prev;
        LogRaw("[exit] 已子类化游戏窗口过程 %p -> %p（捕获 WM_CLOSE/WM_DESTROY）",
               (void *)prev, (void *)&ExitWndProc);
    }

    // ② 看门狗
    g_watchStop = 0;
    g_watch = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    if (!g_watch)
        LogRaw("[exit] WARN: 看门狗线程创建失败 err=%lu", (unsigned long)GetLastError());
}

void ExitGuard_Detach(void)
{
    if (InterlockedExchange(&g_installed, 0) == 0 && !g_watch)
        return;

    g_watchStop = 1;
    if (g_watch)
    {
        // 不在看门狗线程自等（正常调用点是 ModUnload，不会撞上；保险起见判一下）
        if (g_watchTid != GetCurrentThreadId())
            WaitForSingleObject(g_watch, 500);
        CloseHandle(g_watch);
        g_watch = nullptr;
    }

    if (g_hwnd && g_origProc && IsWindow(g_hwnd))
    {
        SetLastError(0);
        LONG_PTR prev = SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
        if (prev || GetLastError() == 0)
        {
            LogRaw("[exit] 已还原游戏窗口过程");
            g_origProc = nullptr;
        }
        else
        {
            // 还原失败：**绝不能**把 g_origProc 置空 —— 窗口过程还是我们的，
            // 一旦置空，之后任何消息都会退化成 DefWindowProc，游戏窗口逻辑直接坏掉。
            LogRaw("[exit] WARN: 还原游戏窗口过程失败 err=%lu（继续用原过程转发）",
                   (unsigned long)GetLastError());
        }
    }
    else
    {
        g_origProc = nullptr;
    }
    g_hwnd = nullptr;
}

int ExitGuard_Fired(void) { return g_fired ? 1 : 0; }
