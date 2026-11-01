// log.h — 工具层共用的日志出口声明
// ============================================================================
// 实现分两处，**故意不合并**：
//   · main.cpp            —— Log / LogRaw / LogRawClassify 的**唯一实现**
//                            （文件 + 无锁环形缓冲双写；只有一份，否则临界区不是
//                            同一个、日志会交错）
//   · core/paths.cpp      —— 模块目录探测 + 限流日志 + 路径拼接
//
// 为什么工具层不自己再实现一份日志：会变成两个临界区、两个文件句柄，日志交错
// 且会被并发写坏。
// ============================================================================
#pragma once

#include <windows.h>

// 记录模块句柄（DllMain 里调一次，供目录探测用）。
void ArkLogInit(HMODULE mod);

// ---- 模块目录 ----
// UTF-8 / 宽字符两种形态，**不带结尾反斜杠**，由内部持有，不要 free。
// 全流程保证非空 —— 取不到任何候选时返回 "."，因为拼 "%s\\subdir" 时
// 空串会让前导反斜杠被 Windows 按**当前盘根**解析（实测写去过 E:\maps）。
const char    *ArkModuleDirA();
const wchar_t *ArkModuleDirW();

// 拼出「模块目录/相对路径」。返回 false = 参数非法或拼出来是空。
bool ArkPathW(wchar_t *out, int cap, const wchar_t *rel);
bool ArkPathA(char *out, int cap, const char *rel);

// ---- 输出（实现在 main.cpp）----
void LogRaw(const char *fmt, ...);
void Log(const char *fmt, ...);
int  LogRawClassify(const char *t);

// 同一指纹只打前 N 次，之后每 M 次打一行。
// ★ 不要用「总条数」限流异常日志：引擎自己一次能抛十几条 NRE 就把配额烧光，
//   真正的崩溃信号反而被吞（实测踩过）。
//   fingerprint 随便给个常量（建议 4 字节 ASCII，方便反查是谁打的）。
void ArkLogThrottled(unsigned fingerprint, const char *fmt, ...);
