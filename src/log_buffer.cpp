// log_buffer.cpp — 无锁环形日志缓冲：Log() 双写（文件 + 缓冲），imgui 面板从中取数据
// ---------------------------------------------------------------------------
// 为什么不用现成方案：
//   Log() 会被 Worker 线程和 Present 钩子（游戏主渲染线程）同时调用，必须无锁。
//   std::mutex 在这里不合适 —— 手动映射的载荷里任何带锁的标准设施都可能引入
//   静态 TLS 或额外 DLL 依赖（这正是本项目要避免的），所以用纯原子索引 + 幂次槽。
//
// 结构：
//   固定 N 个槽，每槽存一行（最长 LOG_LINE_MAX 字节）+ 一个单调递增的序号。
//   写入方：原子取下一个写索引，覆盖最旧的槽（环形覆盖，不阻塞）。
//   读取方（imgui 主渲染线程）：扫描全部槽，只取 seq > lastSeen 的行。
//   槽本身是 POD、无锁；每槽用原子 flag 标记「正在写」避免读到半行。
// ---------------------------------------------------------------------------
#include "log_buffer.h"
#include <string.h>

LogSlot g_logSlots[LOG_SLOT_COUNT];
volatile LONG g_logWriteIdx = 0;

// 单调序号：每写一行 +1。读取方用「我上次看到���序号」作水位线。
static volatile LONG g_logSeq = 0;

void LogBufWrite(const char *text)
{
    // 取槽：原子递增写索引（不做取模运算的除法，用掩码）
    LONG idx = InterlockedIncrement(&g_logWriteIdx) - 1;
    LONG slot = idx & (LOG_SLOT_COUNT - 1);

    LogSlot *s = &g_logSlots[slot];
    // 标记为写中（读者会跳过）
    InterlockedExchange(&s->busy, 1);
    LONG seq = InterlockedIncrement(&g_logSeq);

    size_t n = strlen(text);
    if (n >= LOG_LINE_MAX) n = LOG_LINE_MAX - 1;
    memcpy(s->line, text, n);
    s->line[n] = 0;
    s->len = (int)n;
    s->level = LogRawClassify(text);

    s->seq = seq;
    MemoryBarrier();                 // 保证上面写入对读者可见
    InterlockedExchange(&s->busy, 0); // 清写中标记
}

// 读取：把比 lastSeq 新的行拷进回调。返回读到的最新序号。
LONG LogBufDrain(LONG lastSeq, void (*sink)(const char *, int, void *), void *user)
{
    LONG latest = InterlockedCompareExchange(&g_logSeq, 0, 0);
    for (LONG i = 1; i <= LOG_SLOT_COUNT; ++i) {
        LogSlot *s = &g_logSlots[i & (LOG_SLOT_COUNT - 1)];
        if (InterlockedCompareExchange(&s->busy, 0, 0)) continue;  // 正在写，跳过
        LONG sq = s->seq;
        if (sq <= lastSeq) continue;
        MemoryBarrier();
        if (sq != s->seq) continue;   // 读到一半被覆盖
        if (s->line[0]) sink(s->line, s->level, user);
    }
    return latest;
}