// paths.cpp — 模块目录探测 / 路径拼接 / 限流日志
// ============================================================================
// 为什么单独一个 TU：这三样东西工具层（core/*、panels/*）与引导层都要用，
// 而引导层（main.cpp）已经持有日志实现 —— 把文件系统那半边切出来，
// 谁都能调，且只有一份状态（目录只探测一次）。
//
// ★ 手动映射时本模块**不在 PEB 模块列表里**：
//   GetModuleFileNameW(HMODULE) 与 __ImageBase 都不可靠（实测都失败）。
//   所以逐级降级，最后兜底 "."，绝不返回空串：
//   空串会让 "%s\\subdir" 的前导反斜杠被 Windows 按**当前盘根**解析
//   （实测写去过 E:\maps）。
// ============================================================================
#include "core/log.h"
#include "log_buffer.h"
#include "mod_config.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

static HMODULE  g_mod = nullptr;
static wchar_t  g_dirW[MAX_PATH];
static char     g_dirA[MAX_PATH];
static volatile LONG g_dirReady = 0;

static void EnsureDir()
{
    if (InterlockedCompareExchange(&g_dirReady, 1, 0) != 0)
    {
        // 别人已经在算了 / 算完了。这里不做自旋等待 —— 调用方都是低频路径，
        // 等一次 Sleep 不值得；最坏情况是两遍结果完全一致（纯读环境变量）。
        if (g_dirW[0])
            return;
    }

    wchar_t dir[MAX_PATH] = { 0 };

    if (g_mod && GetModuleFileNameW(g_mod, dir, MAX_PATH))
    {
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
        else       dir[0] = 0;
    }
    if (!dir[0] && GetEnvironmentVariableW(MOD_DIR_ENV, dir, MAX_PATH) == 0)
        dir[0] = 0;
    if (!dir[0] && GetCurrentDirectoryW(MAX_PATH, dir) == 0)
        dir[0] = 0;
    if (!dir[0] && GetTempPathW(MAX_PATH, dir) == 0)
        dir[0] = 0;

    if (!dir[0])
        wcscpy_s(dir, MAX_PATH, L".");

    wcscpy_s(g_dirW, MAX_PATH, dir);

    char a[MAX_PATH] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, dir, -1, a, MAX_PATH, NULL, NULL);
    if (!a[0])
        strcpy_s(a, MAX_PATH, ".");
    strcpy_s(g_dirA, MAX_PATH, a);
}

void ArkLogInit(HMODULE mod)
{
    if (!g_mod) g_mod = mod;
    EnsureDir();
}

const char    *ArkModuleDirA() { EnsureDir(); return g_dirA; }
const wchar_t *ArkModuleDirW() { EnsureDir(); return g_dirW; }

bool ArkPathW(wchar_t *out, int cap, const wchar_t *rel)
{
    if (!out || cap <= 0) return false;
    EnsureDir();
    int n = _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", g_dirW, rel ? rel : L"");
    return n > 0;
}

bool ArkPathA(char *out, int cap, const char *rel)
{
    if (!out || cap <= 0) return false;
    EnsureDir();
    int n = _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", g_dirA, rel ? rel : "");
    return n > 0;
}

// ================================================================ 限流日志
// 指纹桶：同一个指纹（调用点）连续刷屏时只留前几次 + 每 N 次一行。
// 桶满时**覆盖第一个命中的** —— 宁可丢掉某个指纹的统计，也不能让它溢出。
#define THROTTLE_BUCKETS   32
#define THROTTLE_FIRST     3      // 前 3 次全打
#define THROTTLE_EVERY     50     // 之后每 50 次打一行

struct ThrottleSlot
{
    volatile LONG fp;
    volatile LONG count;
};

static ThrottleSlot g_thr[THROTTLE_BUCKETS];

void ArkLogThrottled(unsigned fingerprint, const char *fmt, ...)
{
    const LONG fp = (LONG)fingerprint;   // 0 保留给"空槽"，实际调用方别给 0

    ThrottleSlot *slot = nullptr;
    for (int i = 0; i < THROTTLE_BUCKETS; ++i)
    {
        const LONG cur = g_thr[i].fp;
        if (cur == fp) { slot = &g_thr[i]; break; }
        if (cur == 0 && InterlockedCompareExchange(&g_thr[i].fp, fp, 0) == 0)
        {
            InterlockedExchange(&g_thr[i].count, 0);
            slot = &g_thr[i];
            break;
        }
    }
    if (!slot)
        slot = &g_thr[0];   // 桶满：退化成"记在第一个桶上"，只为了不丢日志

    const LONG n = InterlockedIncrement(&slot->count);
    if (n > THROTTLE_FIRST && (n % THROTTLE_EVERY) != 0)
        return;

    char line[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) return;

    // n 只用来给读者一个"这是第多少次"的信号，不参与限流判定之外的事。
    LogRaw("[x%ld] %s", (long)n, line);
}
