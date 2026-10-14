// log_buffer.h — 无锁日志环形缓冲（共享给日志写入与 imgui 面板）
#pragma once

#include <windows.h>

#define LOG_SLOT_COUNT   512          // 必须 2 的幂（用掩码取模）
#define LOG_LINE_MAX     1024         // 单行最长字节

enum LogLevel
{
    LV_INFO  = 0,   // 普通信息（灰）
    LV_OK    = 1,   // 成功 / 已安装（绿）
    LV_WARN  = 2,   // 警告（黄）
    LV_ERROR = 3,   // 错误 / FATAL（红）
};

struct LogSlot
{
    volatile LONG busy;   // 1 = 正在写，读者应跳过
    LONG seq;             // 单调序号（越大越新）
    int  len;
    int  level;
    char line[LOG_LINE_MAX];
};

extern LogSlot g_logSlots[LOG_SLOT_COUNT];

void LogBufWrite(const char *text);
LONG LogBufDrain(LONG lastSeq, void (*sink)(const char *, int, void *), void *user);

// 根据文本判级别（给面板着色用）。在主文件的 LogRaw 里调用。
int  LogRawClassify(const char *text);