// hot_reload.cpp — 热重载支持：进程内单实例握手 + 完整卸载例程
// ============================================================================
// 手动映射不是 LoadLibrary，没有 FreeLibrary 可用；而直接对同一个进程再注入一份，
// 会撞上四样东西（每一件都会崩）：
//
//   1) **Worker 线程重复创建**：每次注入都 CreateThread，两个 Worker 同时跑 il2cpp
//      引导、同时往同一个 imgui context 塞数据 -> 数据竞争，必崩。
//   2) **ImGui::CreateContext() 重复调用**：imgui 是全局单例 GImGui，第二次调用会
//      覆盖指针，旧 context 及其 GPU 资源（shader/buffer/字体纹理）立刻泄漏。
//   3) **vtable 槽被覆盖**：vtable[8] 已经是「第一次那个 DLL」的 HookPresent 地址，
//      第二次注入会改到新地址 -> 第一次的 Present hook 立刻变野指针。
//   4) **trampoline 池泄漏**：每次 DetourInstall 都 VirtualAlloc 一块新的。
//
// 握手机制：**名字带 PID 的命名互斥体**（Local\il2cpp_mod_lock_<PID>）
//   - 名字带 PID -> 粒度正好是"本进程"，而真正会撞车的正是同一进程里的两份镜像；
//   - 进程退出时内核自动回收，不需要显式清理；
//   - 不消耗页文件后备额度（共享内存那种 CreateFileMapping(INVALID_HANDLE_VALUE)
//     在目标进程里会返回 err=1455，所以互斥体反而更稳）。
//
// 踩过的坑（重要，别改回去）：最初的名字**不带 PID**（Local\esp_il2cpp_test_singleton）。
// `Local\` 是**会话级**命名空间 —— 同一登录会话下所有进程共享，于是一个进程里的
// 守卫会误伤另一个进程的正常注入。实测日志里反复出现：
//     singleton already exists -> refuse to double-init
// 而目标游戏进程里根本没有任何人持有那个名字（是另一个游戏实例占的）。
// **互斥体本身没错，错在名字的作用域。**
//
// 镜像基址**不通过共享内存传**：CreateFileMapping 的页文件后备用法在目标进程里
// 报 err=1455。注入器侧靠扫内存找 ModUnload 导出名反推基址。
//
// 导出（给注入器的 --replace 用）：
//   ModUnload()      —— 执行清理（可重复调用，第二次 no-op）
//   ModIsLoaded()    —— 1 表示本 DLL 当前已加载
//   ModVersion()     —— 编译标记
//   ModLockName()    —— 返回互斥体名（含 PID），供注入器确认实例存在
//   ModSelfBase()    —— 本镜像基址
//
// 另有一组 Esp* 别名转发到上面的 Mod*（历史命名兼容），见文件末尾。
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "mod_config.h"
#include "ui/ui_api.h"
#include "exit_guard.h"

// main.cpp / user_hooks.cpp 提供
extern void ShutdownWorkerThread();
extern void UnloadToolDll();
extern void Mod_OnShutdown();

// il2cpp_hook.h：还原函数体 detour。
// **热重载的关键一步**：函数体跳板写在 IL2CPP 模块的代码页里，不在我们镜像内。
// 不还原的话：(a) 旧钩子仍指向已释放镜像，继续生效；
//           (b) 下一次 InstallDetour 把已改写的字节当正常指令解析
//              -> insns 变少 -> 装出残缺 hook 且**返回 true 看起来成功**。
// 实测：连续三次热重载后 class_from_name 命中数 1569 -> 777 -> 0。
// 注意它声明在 il2cpp::hook 命名空间里，别用 `extern int RemoveAllDetours();`
// 手写全局声明 —— 那样链接期找不到符号。
#include "hook/il2cpp_hook.h"

#define MOD_ABI_VERSION 1

namespace
{

// 握手句柄。名字**带 PID** —— 这是关键，见文件头注释。
HANDLE         g_lock = NULL;
HMODULE        g_selfMod = NULL;
ULONG_PTR      g_selfBase = NULL;
volatile LONG  g_unloaded = 0;
const int      g_version = 0x10001;
wchar_t        g_lockName[64] = L"";

// 卸载阶段不能依赖主模块的 LogRaw（那时可能已经在销毁流程里），写自己的文件。
void LogU(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, MAX_PATH, MOD_UNLOAD_LOG);
    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"a") == 0 && f)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02u:%02u:%02u.%03u][pid=%lu] %s\n", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, (unsigned long)GetCurrentProcessId(), line);
        fclose(f);
    }
}

void ReleaseLock()
{
    if (g_lock)
    {
        ReleaseMutex(g_lock);
        CloseHandle(g_lock);
        g_lock = NULL;
    }
}

} // namespace

// 游戏主动退出时的清理入口（定义在本文件下方，ModClaimInstance 里要注册它）
void ModExitCleanup();

// ---------------------------------------------------------------------------
extern "C" {

void ModSetSelf(HMODULE mod, ULONG_PTR base)
{
    g_selfMod = mod;
    g_selfBase = base;
}

// 进程内守卫。返回 1 = 抢到（正常，继续引导）；0 = 本进程里已有一份镜像（本次放弃）。
int ModClaimInstance()
{
    if (g_unloaded) return 0;              // 已卸载过，别复活
    if (g_lock)     return 0;              // 本进程已经有一份

    // 名字**带 PID**，粒度是"本进程"。
    // 互斥体只做"进程内互斥"，不承担"传递镜像基址"（那需要共享内存，
    // 但 CreateFileMapping(INVALID_HANDLE_VALUE) 这种页文件后备用法在目标
    // 进程里会返回 err=1455）。基址改由注入器扫内存拿。
    swprintf_s(g_lockName, L"%s%lu", MOD_LOCK_PREFIX, (unsigned long)GetCurrentProcessId());

    g_lock = CreateMutexW(NULL, TRUE, g_lockName);
    if (!g_lock)
    {
        // 建不起来就**不拦着**，让主程序继续跑 —— 宁可重复初始化也不要静默失效。
        LogU("[lock] CreateMutex('%ls') failed err=%lu -> continue anyway",
             g_lockName, (unsigned long)GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // 本进程里已有一份活着的实例 -> 放弃（重复注入必然撞车）
        LogU("[lock] instance already active in this pid -> refuse");
        ReleaseLock();
        return 0;
    }
    LogU("[lock] claimed '%ls' (base=0x%llX)", g_lockName, (unsigned long long)g_selfBase);

    // 注册「游戏自己退出」的清理回调（点 X / Alt+F4 / 注销 / 关机）。
    // 必须在 overlay 装好之前注册 —— 守护是靠 BindToChain 拿到窗口后才真正生效的，
    // 但回调要先就位，否则窗口一到就触发会落空。
    ExitGuard_SetHandler(&ModExitCleanup);
    return 1;
}

// ---------------------------------------------------------------------------
// 游戏**主动退出**时的清理入口（由 src/exit_guard.cpp 在独立线程上调用）。
// 定义放在文件末尾（extern "C" 块之外）—— 它只需要被取地址，不参与 C ABI 导出。
// ---------------------------------------------------------------------------
// 卸载：逆序清理全部全局状态。可重复调用（第二次 no-op）。
void ModUnload()
{
    if (InterlockedExchange(&g_unloaded, 1) == 1)
    {
        LogU("[unload] already unloaded, no-op");
        return;
    }
    LogU("[unload] ===== begin unload (base=0x%llX) =====", (unsigned long long)g_selfBase);

    // 0) 先拆掉"游戏退出守护"（还原窗口过程 + 停看门狗）。
    //    必须最先做：它还挂着游戏窗口过程，且看门狗会在窗口消失时再触发一次清理。
    ExitGuard_Detach();

    // 1) 先停 Worker —— 否则它可能在我们还原 vtable 后继续调已失效的钩子。
    //    注意这会在**注入器线程**上，最多等 3 秒。
    ShutdownWorkerThread();

    // 2) 还原所有函数体 detour（必须在镜像被释放之前）
    //    顺序很关键：先让 Worker 停手（不再有新 detour 装上去），
    //    再把已写入 IL2CPP 模块的跳板擦掉。
    {
        int n = il2cpp::hook::RemoveAllDetours();
        if (n > 0)
            LogU("[unload] 还原 %d 处函数体 detour（原字节已写回目标模块）", n);
        else
            LogU("[unload] 无函数体 detour 需要还原");
    }

    // 3) 用户清理钩子（src/user_hooks.cpp 的 Mod_OnShutdown）。
    //    ★ 必须早于镜像释放：使用方自己 CreateThread 起的线程跑在我们的镜像里，
    //      镜像被 VirtualFreeEx 之后它们执行的是已释放的代码 -> 崩。
    Mod_OnShutdown();

    // 4) overlay 清理 —— UiShutdown 内部只"置卸载标志"然后**等**渲染线程执行
    //    真正的清理（还原 vtable / DestroyContext / 释放资源）。
    //    为什么必须这样：imgui context 正被游戏渲染线程每帧读写，
    //    在注入器线程上直接 DestroyContext 是数据竞争 -> 实测必崩。
    UiShutdown();

    if (!UiShutdownCompleted())
        LogU("[unload] WARN: 渲染线程侧清理未在 3s 内完成（游戏窗口可能不可见）");
    else
        LogU("[unload] 渲染线程侧清理完成");

    // 5) 卸载工具 DLL。
    //    顺序必须在 UiShutdown **之后**：此时渲染线程已停止调 Tool_Draw，工具 DLL
    //    的代码不在任何调用栈上，FreeLibrary 才安全。
    //    不做这步的话，旧的工具 DLL 会残留在进程里，下次注入同路径 LoadLibraryW
    //    直接返回旧句柄 -> 新编的 DLL 内容不生效 -> 热重载失效。
    LogU("[unload] 卸载工具 DLL ...");
    UnloadToolDll();
    LogU("[unload] 工具 DLL 卸载完成");

    // 6) 释放进程内互斥（下一次注入就能重新抢到）
    ReleaseLock();
    LogU("[lock] released");

    LogU("[unload] ===== end unload OK =====");
}

int ModIsLoaded()       { return g_unloaded == 0 ? 1 : 0; }
int ModVersion()        { return g_version; }
ULONG_PTR ModSelfBase() { return g_selfBase; }

// 返回互斥体名（含 PID）。注入器可用它确认"本进程里确实有实例在跑"，
// 但**不能**用它拿镜像基址（互斥体不存数据）—— 基址仍靠扫内存找 ModUnload 导出。
const wchar_t *ModLockName()
{
    if (!g_lockName[0])
        swprintf_s(g_lockName, L"%s%lu", MOD_LOCK_PREFIX, (unsigned long)GetCurrentProcessId());
    return g_lockName;
}

int ModAbiVersion() { return MOD_ABI_VERSION; }

// ---------------------------------------------------------------------------
// Esp* —— 历史命名兼容别名（原项目里 ESP = "external script payload"）。
// 老版本的注入器按 Esp 前缀找导出，这里转发一份，两边都能用。
// 想彻底改名：删掉这个区块，并同步 mod_template.def。
void EspUnload()                              { ModUnload(); }
int  EspIsLoaded()                            { return ModIsLoaded(); }
int  EspVersion()                             { return ModVersion(); }
int  EspClaimInstance()                       { return ModClaimInstance(); }
void EspSetSelf(HMODULE mod, ULONG_PTR base)  { ModSetSelf(mod, base); }
ULONG_PTR EspSelfBase()                       { return ModSelfBase(); }
const wchar_t *EspLockName()                  { return ModLockName(); }
int  EspAbiVersion()                          { return ModAbiVersion(); }

} // extern "C"

// ---------------------------------------------------------------------------
// 游戏**主动退出**时的清理入口（由 src/exit_guard.cpp 在独立线程上调用）。
//
// 为什么需要：手动映射的镜像不在 PEB 模块列表里，游戏走 ExitProcess 时
// 我们收不到任何 DLL_PROCESS_DETACH，而我们的全局鼠标钩子 / dxgi vtable 补丁 /
// 两条常驻线程都还活着 —— 退出序列会卡住，只能在任务管理器强制结束进程。
//
// 拆成两步：
//   ① UiExitFastPath()  —— 快、幂等、不等任何线程：卸全局 WH_MOUSE_LL + 还原 vtable。
//      这两样必须**抢在** ExitProcess 杀线程之前完成。
//   ② ModUnload()       —— 完整清理（停 Worker / 还原 detour / 用户清理 /
//      卸工具 DLL / 释放 GPU 引用）。它本身幂等，注入器稍后再调也无副作用。
//
// 定义放在 extern "C" 块之外：它只被取地址交给 ExitGuard，不参与 ABI 导出，
// 用 C++ 链接即可（放进 extern "C" 块会与上面的前向声明链接不一致）。
// ---------------------------------------------------------------------------
void ModExitCleanup()
{
    LogU("[exit] ===== 游戏退出，开始清理 =====");
    UiExitFastPath();
    ModUnload();
    LogU("[exit] ===== 游戏退出清理完成 =====");
}
