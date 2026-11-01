// taskbus.cpp — 跨线程任务队列实现
#include "core/taskbus.h"
#include "core/guard.h"
#include "core/log.h"
#include "mod_config.h"

#include <windows.h>

struct ArkTask
{
    ArkTaskFn   fn;
    void       *ud;
    const char *tag;
};

static ArkTask          g_q[MOD_TASKBUS_CAP];
static volatile LONG    g_head;      // 只增：写位置
static volatile LONG    g_tail;      // 只增：读位置
static volatile LONG    g_executed;
static volatile LONG    g_failed;
static CRITICAL_SECTION g_lock;
static volatile LONG    g_ready;

void ArkTaskBusInit()
{
    if (InterlockedCompareExchange(&g_ready, 1, 0) != 0)
        return;
    InitializeCriticalSection(&g_lock);
    InterlockedExchange(&g_head, 0);
    InterlockedExchange(&g_tail, 0);
    InterlockedExchange(&g_executed, 0);
    InterlockedExchange(&g_failed, 0);
}

bool ArkTaskPost(ArkTaskFn fn, void *ud, const char *tag)
{
    if (!fn) return false;
    if (!InterlockedCompareExchange(&g_ready, 0, 0))
        ArkTaskBusInit();

    bool ok = false;
    EnterCriticalSection(&g_lock);
    const LONG h = g_head;
    const LONG t = g_tail;
    if ((unsigned)(h - t) < (unsigned)MOD_TASKBUS_CAP)
    {
        ArkTask *slot = &g_q[h & (MOD_TASKBUS_CAP - 1)];
        slot->fn = fn;
        slot->ud = ud;
        slot->tag = tag;
        g_head = h + 1;
        ok = true;
    }
    LeaveCriticalSection(&g_lock);

    if (!ok)
        Log("[task] WARN: 队列满（%d），丢弃 %s", MOD_TASKBUS_CAP, tag ? tag : "?");
    return ok;
}

void ArkTaskPump()
{
    if (!InterlockedCompareExchange(&g_ready, 0, 0))
        return;

    for (;;)
    {
        ArkTask t;
        bool got = false;

        EnterCriticalSection(&g_lock);
        if (g_tail != g_head)
        {
            t = g_q[g_tail & (MOD_TASKBUS_CAP - 1)];
            g_tail = g_tail + 1;
            got = true;
        }
        LeaveCriticalSection(&g_lock);

        if (!got) break;

        // 标记动作，崩溃日志里能指到具体是哪一次请求 —— 出锁之后才设，
        // 避免持有锁的时候去碰 Guard 内部状态。
        ArkGuardSetAction(t.tag ? t.tag : "task");

        // ★ 这里不做 SEH 软恢复：mingw 没有 MSVC 的 __try 语法，而我们也不想在
        //   托管异常之后继续跑。做的是"让崩溃带着上下文落盘"（VEH 那侧负责）。
        t.fn(t.ud);
        InterlockedIncrement(&g_executed);
    }
}

int  ArkTaskPending()  { return (int)(InterlockedCompareExchange(&g_head, 0, 0) -
                                      InterlockedCompareExchange(&g_tail, 0, 0)); }
long ArkTaskExecuted() { return (long)InterlockedCompareExchange(&g_executed, 0, 0); }
long ArkTaskFailed()   { return (long)InterlockedCompareExchange(&g_failed, 0, 0); }

// ============================================================================
// 主线程队列 —— 只有 Unity 主线程能消费
// ============================================================================
// 和上面那条队列是**两套独立的环**：主线程任务绝不会被 Worker 顺手执行掉。
// 容量小（256）是故意的：会进这条队列的都是"敲一下按钮"的人手操作，
// 不是批量任务；真塞满了说明调用方在批量刷，那是调用方的 bug。

static ArkTask          g_mq[MOD_MAINQ_CAP];
static volatile LONG    g_mHead;
static volatile LONG    g_mTail;
static volatile LONG    g_mExecuted;
static CRITICAL_SECTION g_mLock;
static volatile LONG    g_mReady;
static volatile LONG    g_mainTid;
static volatile LONG    g_mPumping;   // 主线程泵防重入

static void MainQInitOnce()
{
    if (InterlockedCompareExchange(&g_mReady, 1, 0) != 0)
        return;
    InitializeCriticalSection(&g_mLock);
    InterlockedExchange(&g_mHead, 0);
    InterlockedExchange(&g_mTail, 0);
    InterlockedExchange(&g_mExecuted, 0);
}

bool ArkTaskPostMain(ArkTaskFn fn, void *ud, const char *tag)
{
    if (!fn) return false;
    MainQInitOnce();

    bool ok = false;
    EnterCriticalSection(&g_mLock);
    const LONG h = g_mHead;
    const LONG t = g_mTail;
    if ((unsigned)(h - t) < (unsigned)MOD_MAINQ_CAP)
    {
        ArkTask *slot = &g_mq[h & (MOD_MAINQ_CAP - 1)];
        slot->fn = fn;
        slot->ud = ud;
        slot->tag = tag;
        g_mHead = h + 1;
        ok = true;
    }
    LeaveCriticalSection(&g_mLock);

    if (!ok)
        Log("[task] WARN: 主线程队列满（%d），丢弃 %s", MOD_MAINQ_CAP, tag ? tag : "?");
    return ok;
}

void ArkMainTaskPump()
{
    if (!InterlockedCompareExchange(&g_mReady, 0, 0))
        return;

    // ★ 防重入：任务体里调 il2cpp 会再命中 class_from_name 的 detour，
    //   那里又会看到"主线程 + 有任务" ⇒ 递归。外层循环本来就会接着取，
    //   所以内层直接返回，任务一条都不会丢。
    if (InterlockedCompareExchange(&g_mPumping, 1, 0) != 0)
        return;

    for (;;)
    {
        ArkTask t;
        bool got = false;

        EnterCriticalSection(&g_mLock);
        if (g_mTail != g_mHead)
        {
            t = g_mq[g_mTail & (MOD_MAINQ_CAP - 1)];
            g_mTail = g_mTail + 1;
            got = true;
        }
        LeaveCriticalSection(&g_mLock);

        if (!got) break;

        ArkGuardSetAction(t.tag ? t.tag : "mtask");
        t.fn(t.ud);
        InterlockedIncrement(&g_mExecuted);
    }

    InterlockedExchange(&g_mPumping, 0);
}

int  ArkMainTaskPending()  { return (int)(InterlockedCompareExchange(&g_mHead, 0, 0) -
                                          InterlockedCompareExchange(&g_mTail, 0, 0)); }
long ArkMainTaskExecuted() { return (long)InterlockedCompareExchange(&g_mExecuted, 0, 0); }

void ArkTaskBusSetMainThread(unsigned long tid)
{
    if (tid)
        InterlockedExchange(&g_mainTid, (LONG)tid);
}

unsigned long ArkTaskBusMainThread()
{
    return (unsigned long)InterlockedCompareExchange(&g_mainTid, 0, 0);
}

int ArkTaskBusIsMainThread()
{
    const unsigned long mt = ArkTaskBusMainThread();
    return mt && ((unsigned long)GetCurrentThreadId() == mt);
}
