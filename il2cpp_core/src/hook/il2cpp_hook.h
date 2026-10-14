// il2cpp_hook.h — IL2CPP 方法挂钩（x64 函数体 detour，零外部依赖）
//
// 与项目既有的 Present 挂钩同思路：只改写目标函数的机器码头部为
// 「mov rax, <detour>; jmp rax」，并把被覆盖的原指令克隆进一段可执行
// trampoline（含相对跳转/调用重定位），返回该 trampoline 用于调用原函数。
// 不改 vtable、不改导入表，因此不触碰反作弊对全局共享结构的篡改检测。
//
// 用法：
//   ih::SetLogger(MyLogger);                                   // 可选
//   void *orig = ih::HookMethod("Torappu.Battle.Character",    // 命名空间.类名
//                               "SomeMethod", 1, MyDetour);    // 方法名 + 参数个数
//   // orig 非空即挂钩成功；(*(T*)orig) 可调用原实现
#pragma once

namespace il2cpp
{
namespace hook
{

// 日志回调：宿主注入，便于写进自己的日志文件。传 nullptr 则静默。
typedef void (*LogFn)(const char *line);
void SetLogger(LogFn fn);

// 通用 x64 函数体 detour。成功返回可调用原函数的 trampoline，失败返回 nullptr。
void *Detour(void *target, void *detour);

// 还原函数体的原始字节（撤掉 detour）。热重载的 EspUnload 必须调用 ——
// 函数体跳板写在目标模块（GameAssembly）的代码页里，不在我们镜像内，
// 镜像被释放后没人还原它。返回 true=已还原。
bool RemoveDetour(void *target);
int  RemoveAllDetours();      // 还原全部已记录的补丁，返回还原了几个

// 解析「命名空间.类名 / 方法名 / 参数个数」对应的方法代码地址（不安装钩子）。
// argsCount 传 -1 表示不限参数个数。解析失败返回 nullptr。
void *ResolveMethod(const char *classFullName, const char *methodName, int argsCount);

// 解析并安装钩子。成功返回 trampoline，失败返回 nullptr。
void *HookMethod(const char *classFullName, const char *methodName, int argsCount, void *detour);

} // namespace hook
} // namespace il2cpp