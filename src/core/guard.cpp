// guard.cpp — 崩溃取证（VEH + 未处理异常过滤器）与模块熔断
#include "core/guard.h"
#include "core/log.h"
#include "mod_config.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

static volatile PVOID g_action;          // 指向 g_actionBuf 或某个静态字符串
static char           g_actionBuf[128];
static volatile LONG  g_crashLogged;

// 熔断计数：按模块 id 指针地址落桶（模块表是静态的，地址稳定）
struct TripBucket { volatile PVOID id; volatile LONG count; };
static TripBucket g_trip[32];

void ArkGuardSetAction(const char *what)
{
    if (!what) return;
    // 短字符串直接拷进缓冲，长字符串就当它本身是稳定的静态字面量。
    size_t n = strlen(what);
    if (n < sizeof(g_actionBuf))
    {
        memcpy(g_actionBuf, what, n + 1);
        g_action = (PVOID)g_actionBuf;
    }
    else
    {
        g_action = (PVOID)what;
    }
}

const char *ArkGuardLastAction()
{
    PVOID a = (PVOID)InterlockedCompareExchangePointer(&g_action, nullptr, nullptr);
    return a ? (const char *)a : "(未标记)";
}

int ArkGuardNoteFailure(const char *moduleId, const char *why)
{
    if (!moduleId) return 0;
    TripBucket *slot = nullptr;
    for (int i = 0; i < 32; ++i)
    {
        PVOID cur = (PVOID)InterlockedCompareExchangePointer(&g_trip[i].id, nullptr, nullptr);
        if (cur == (PVOID)moduleId) { slot = &g_trip[i]; break; }
        if (!cur)
        {
            if (InterlockedCompareExchangePointer(&g_trip[i].id, (PVOID)moduleId, nullptr) == nullptr)
            {
                InterlockedExchange(&g_trip[i].count, 0);
                slot = &g_trip[i];
            }
            else if ((PVOID)InterlockedCompareExchangePointer(&g_trip[i].id, nullptr, nullptr) == (PVOID)moduleId)
            {
                slot = &g_trip[i];
            }
            if (slot) break;
        }
    }
    if (!slot) return 0;

    LONG n = InterlockedIncrement(&slot->count);
    if (n >= 5)
    {
        Log("[guard] 模块 %s 连续失败 %ld 次（%s），已熔断", moduleId, (long)n, why ? why : "?");
        return 1;
    }
    Log("[guard] 模块 %s 失败 %ld/5：%s", moduleId, (long)n, why ? why : "?");
    return 0;
}

void ArkGuardResetFailure(const char *moduleId)
{
    if (!moduleId) return;
    for (int i = 0; i < 32; ++i)
    {
        if ((PVOID)InterlockedCompareExchangePointer(&g_trip[i].id, nullptr, nullptr) == (PVOID)moduleId)
        {
            InterlockedExchange(&g_trip[i].count, 0);
            return;
        }
    }
}

// ================================================================ 崩溃现场写盘
// 独立文件：主日志可能因为路径/锁/被别的线程占着而写不进去，崩溃时不能赌。
static void WriteCrashDump(const char *kind, DWORD code, void *addr, void *info0, void *info1)
{
    if (InterlockedIncrement(&g_crashLogged) > 1)
        return;   // 只记第一次，后面的是连锁反应

    wchar_t path[MAX_PATH];
    if (!ArkPathW(path, MAX_PATH, MOD_CRASH_LOG))
        return;

    FILE *f = _wfopen(path, L"a");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "\n===== %s %04u-%02u-%02u %02u:%02u:%02u.%03u =====\n", kind,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fprintf(f, "  异常码    : 0x%08lX\n", (unsigned long)code);
    fprintf(f, "  异常地址  : %p\n", addr);
    fprintf(f, "  info0     : %p   info1: %p\n", info0, info1);
    fprintf(f, "  最近动作  : %s\n", ArkGuardLastAction());
    fprintf(f, "  线程      : tid=%lu  pid=%lu\n", (unsigned long)GetCurrentThreadId(),
            (unsigned long)GetCurrentProcessId());

    // ★ 把异常码翻译成人话，省掉每次翻 MSDN
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        fprintf(f, "  含义      : 访问违例（%s 地址 %p）\n",
                (info0 == (void *)0) ? "读取" : (info0 == (void *)1) ? "写入" : "执行",
                info1);
        break;
    case 0xE06D7363:
        fprintf(f, "  含义      : C++/托管异常抛到 native 边界（info1 首 qword = Il2CppException*）\n");
        break;
    case EXCEPTION_STACK_OVERFLOW:
        fprintf(f, "  含义      : 栈溢出（无限递归？）\n");
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        fprintf(f, "  含义      : 整数除零\n");
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        fprintf(f, "  含义      : 非法指令（多半是跳到垃圾地址）\n");
        break;
    default:
        break;
    }
    fputc('\n', f);
    fflush(f);
    fclose(f);
}

// ================================================================ 已知噪声
// ★ 游戏自己在 ArknightsBase.dll 某处每帧触发一次"写 NULL"的访问违例，它自己的 VEH 会接住
//   （游戏从没因此崩过）。我们的 VEH 装得比它早 ⇒ 每次都抢先写一份**假的** crash log，
//   还会把 g_crashLogged 那唯一的"只记第一次"名额占掉 —— 真崩溃反而写不进去。
//   白名单在这里挡掉：只认「写 NULL」这一个组合，同地址的其它类型故障照常记录。
static const unsigned long long kNoiseRva[] = {
    0x1D6512EULL,   // ArknightsBase.dll 每帧噪声（2026-10-25 定案）
};

static bool IsKnownNoise(EXCEPTION_RECORD *r)
{
    if (!r || r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return false;
    if (r->NumberParameters < 2) return false;
    // ★ Windows 的 ExceptionInformation[0] 是**访问类型**：0=读, 1=写, 8=执行。
    //   这里必须写 1。上一版误以为 0=写，条件永远不成立 ⇒ 白名单等于没装，
    //   噪声照样写进 crash log 并吃掉那唯一的名额（2026-11-01 实测踩中）。
    if (r->ExceptionInformation[0] != 1) return false;   // 只认"写"
    if (r->ExceptionInformation[1] != 0) return false;   // 写地址必须正好是 NULL

    HMODULE base = GetModuleHandleW(L"ArknightsBase.dll");
    if (!base) return false;

    const unsigned long long b = (unsigned long long)(uintptr_t)base;
    const unsigned long long a = (unsigned long long)(uintptr_t)r->ExceptionAddress;
    if (a < b || a - b > 0x4000000ULL) return false;      // 不在这个模块的合理体积内

    const unsigned long long rva = a - b;
    for (size_t i = 0; i < sizeof(kNoiseRva) / sizeof(kNoiseRva[0]); ++i)
        if (rva == kNoiseRva[i]) return true;
    return false;
}

static LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    EXCEPTION_RECORD *r = ep->ExceptionRecord;

    if (IsKnownNoise(r))
    {
        static volatile LONG noted = 0;
        if (InterlockedCompareExchange(&noted, 1, 0) == 0)
            Log("[guard] 已忽略已知噪声：ArknightsBase+0x1D6512E 写 NULL（游戏自处理的每帧异常）");
        return EXCEPTION_CONTINUE_SEARCH;   // 直接放行，不写 crash log、不占名额
    }
    // 只关心"致命"的那几类。C++ 异常（0xE06D7363）托管侧会自己处理，
    // 但落到这里说明已经在 native 边界上炸了 —— 值得记一笔。
    switch (r->ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case 0xE06D7363:
        WriteCrashDump("VEH",
                       r->ExceptionCode,
                       r->ExceptionAddress,
                       r->NumberParameters > 0 ? (void *)r->ExceptionInformation[0] : nullptr,
                       r->NumberParameters > 1 ? (void *)r->ExceptionInformation[1] : nullptr);
        break;
    default:
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;   // 绝不吞：让游戏/系统按原样处理
}

static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord)
        WriteCrashDump("UEF", ep->ExceptionRecord->ExceptionCode,
                       ep->ExceptionRecord->ExceptionAddress, nullptr, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

void ArkGuardInstall()
{
    static volatile LONG installed = 0;
    if (InterlockedCompareExchange(&installed, 1, 0) != 0)
        return;

    // ★ 顺序讲究：VEH 先装（系统会最先调它），UEF 后装。
    //   注意游戏自己（CrashSight 之类）会在启动后**重新顶掉** UEF，
    //   所以 UEF 那一路随时可能失效；VEH 是主路径。
    AddVectoredExceptionHandler(1, VectoredHandler);
    SetUnhandledExceptionFilter(UnhandledFilter);

    ArkGuardSetAction("boot");
    Log("[guard] 崩溃取证已安装（VEH + UEF），现场写 %ls", MOD_CRASH_LOG);
}
