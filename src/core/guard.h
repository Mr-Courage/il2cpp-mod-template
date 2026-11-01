// guard.h — 崩溃取证与熔断
// ============================================================================
// 这里**不做**"捕获异常然后继续跑"。在 IL2CPP 进程里软恢复现场（栈已歪、
// 托管异常挂在 native 边界上）几乎是必崩二次，而且会把一次干净的崩溃变成
// 一个飘忽不定的鬼故事。
//
// 这里做两件事：
//   1) **留下现场**：VEH + 未处理异常过滤器，把崩溃地址、最近动作、模块状态
//      写进 arkmodify.log，并刷一次盘。下次看到闪退就能立刻知道是我们哪一行。
//   2) **熔断**：同一模块连续失败 N 次就强制关掉它，别让一次错误在整个会话里
//      反复把游戏拖死。
// ============================================================================
#pragma once

void ArkGuardInstall();

// 标记"当前正在做什么"。崩溃日志里会带上它。
// ★ 热路径慎用：内部只有一次指针赋值，但别每帧塞字符串格式化。
void ArkGuardSetAction(const char *what);
const char *ArkGuardLastAction();

// 记录一次模块失败；超过阈值返回 1 = 已熔断。
int ArkGuardNoteFailure(const char *moduleId, const char *why);

void ArkGuardResetFailure(const char *moduleId);
