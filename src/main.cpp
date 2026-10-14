// main.cpp — 框架引导：注入入口 / 日志 / 配置 / detour 探针 / 面板骨架
// ============================================================================
// 启动流程：
//   DllMain(PROCESS_ATTACH)
//     -> 抢进程内单实例互斥体（带 PID）
//     -> CreateThread(Worker)                      ← 绝不在 DllMain 里做重活
//   Worker 线程
//     -> 等 MOD_GAME_MODULE 加载
//     -> il2cpp::runtime::Init()                   解析 IL2CPP 全部导出
//     -> 装 ImGui 覆盖层（不依赖「游戏是否已建好设备」这个时序）
//     -> 等 Unity 域出现 + EnsureAttached()
//     -> 反射自测（枚举程序集）
//     -> 装 detour 探针
//     -> 每秒 tick（心跳 + Mod_OnTick）
//
// ---- 手动映射约束（改动本文件前必读）----
// 本 DLL 走「手动映射（manual mapping）」注入，映射器未实现静态 TLS 模拟。
// 所以这里刻意不用任何依赖静态 TLS 的设施：
//   · 线程一律 Win32 CreateThread（不用 std::thread，避免 libwinpthread-1.dll）
//   · 计数用 volatile LONG + InterlockedIncrement（不用 std::atomic / thread_local）
//   · 字符串用定长 C 缓冲（不用 std::string / std::vector）
//   · 禁用 C++ 异常与 RTTI（见 Makefile 的 -fno-exceptions -fno-rtti）
// 目标是产物不含 .tls 段、不引用 __emutls_*、导入表只剩系统 DLL。
// 违反了不会报错，只会在注入后随机崩 —— 而且崩点离违规处很远，极难定位。
//
// ---- 想加自己的游戏逻辑？----
// 去 src/user_hooks.cpp：那里有 Mod_OnReady / Mod_OnTick / Mod_OnFrame /
// Mod_OnGameThread / Mod_DrawPanelTabs / Mod_OnShutdown 六个扩展点。
// 本文件是框架，业务代码不要往这里塞。
// ============================================================================
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <imgui.h>

#include "il2cpp/il2cpp_runtime.h"
#include "hook/il2cpp_hook.h"
#include "log_buffer.h"
#include "mod_config.h"
#include "probe.h"
#include "ui/log_panel.h"
#include "ui/ui_api.h"

namespace ir = il2cpp::runtime;
namespace ih = il2cpp::hook;

// ---- 用户扩展点（src/user_hooks.cpp）----
void Mod_OnReady();                 // il2cpp 引导完成、域已挂接（Worker 线程）
void Mod_OnTick();                  // Worker 线程每秒
void Mod_OnFrame();                 // 渲染线程每帧（面板关着也跑）
void Mod_OnGameThread();            // 游戏逻辑线程（class_from_name 钩子里），il2cpp 已 attach
void Mod_DrawPanelTabs();           // 在面板 TabBar 里追加你自己的 tab
void Mod_OnShutdown();              // 卸载时清理（在 IL2CPP detour 还原之前）

// ---- 热重载接口（src/hot_reload.cpp）----
extern "C" {
    void ModUnload();                 // 完整卸载（可重复调用，第二次 no-op）
    int  ModIsLoaded();
    int  ModVersion();
    int  ModClaimInstance();          // 单实例守卫；1=抢到，0=本进程已有实例
    void ModSetSelf(HMODULE mod, ULONG_PTR base);
    ULONG_PTR ModSelfBase();
    const wchar_t *ModLockName();
    int  ModAbiVersion();
}

// ---- 工具 DLL（可选，双 DLL 架构，定义在本文件尾部）----
static bool LoadToolDll();
void UnloadToolDll();
static void ToolDrawInPanel();

static HMODULE       g_mod = nullptr;
static CRITICAL_SECTION g_logLock;

// ---- Worker 线程控制（热重载要用）----
static HANDLE           g_workerThread = NULL;   // 句柄保留不关：ShutdownWorkerThread 要 wait 它
static volatile LONG    g_workerStop = 0;        // 置 1 让 Worker 自己退出
static volatile LONG    g_canaryFromName = 0;
static volatile LONG    g_canaryStringNew = 0;
static volatile LONG    g_targetHits = 0;

// ---- 引导状态（面板「状态」tab 显示）----
static volatile LONG    g_stateReady = 0;        // 1 = il2cpp 引导完成
static int              g_exportsResolved = 0;
static int              g_imageCount = 0;
static wchar_t          g_modulePathW[MAX_PATH] = { 0 };   // 模块路径（宽）
static char             g_modulePath[MAX_PATH] = { 0 };    // 同上，转 UTF-8 后面板显示用

// ---- 工具 DLL 的每帧心跳指针（类型与定义在下方「工具 DLL」段）----
using ToolTickFn = void (*)();
static ToolTickFn g_toolTick = nullptr;

// ---- 覆盖层的全屏叠加绘制钩子（准星 / 世界坐标标注）----
// 想用的话在 Mod_OnReady 里调 UiSetOverlayDrawHook(你的函数)。默认不挂。

// ================================================================ 日志
// 注意：手动映射时本模块**不在 PEB 模块列表里**，
// GetModuleFileNameW(g_mod, ...) 会直接失败返回 0 —— 那样 BuildLogPath 返回 false、
// Log() 第一行就 return，**一行日志都不会有**（踩过一次：注入成功、DllMain=TRUE，
// 但三个可能位置都找不到日志文件）。所以逐级降级：
//   模块路径 -> 环境变量 MOD_DIR_ENV -> 进程 CWD -> %TEMP%
static const wchar_t *PickLogDir(wchar_t *out, int cap)
{
    if (g_mod && GetModuleFileNameW(g_mod, out, cap))
    {
        wchar_t *slash = wcsrchr(out, L'\\');
        if (slash)
        {
            *slash = 0;
            return out;
        }
    }
    if (GetEnvironmentVariableW(MOD_DIR_ENV, out, cap) > 0)
        return out;
    if (GetCurrentDirectoryW(cap, out) > 0)
        return out;
    GetTempPathW(cap, out);
    return out;
}

static bool BuildLogPath(wchar_t *out, int cap)
{
    wchar_t dir[MAX_PATH];
    dir[0] = 0;
    PickLogDir(dir, MAX_PATH);
    if (!dir[0])
        return false;
    swprintf_s(out, cap, L"%s\\%s", dir, MOD_LOG_FILE);
    return out[0] != 0;
}

// 级别判定：给 imgui 面板着色用（log_buffer.cpp 写缓冲时会调它）
int LogRawClassify(const char *t)
{
    if (!t) return LV_INFO;
    if (strstr(t, "FATAL") || strstr(t, "FAILED") || strstr(t, "ERROR"))
        return LV_ERROR;
    if (strstr(t, "WARN") || strstr(t, "LOST") || strstr(t, "skip") || strstr(t, "missing"))
        return LV_WARN;
    if (strstr(t, "ok") || strstr(t, "OK") || strstr(t, "installed") || strstr(t, "resolved"))
        return LV_OK;
    return LV_INFO;
}

// 日志内核：格式化后**双写** —— 文件（事后可查）+ 无锁环形缓冲（面板实时看）。
// 之所以要双写：imgui 面板跑在游戏渲染线程里，在那边做文件 IO 不合适。
// 非 static：其它 TU（probe.cpp / user_hooks.cpp / imgui_overlay.cpp）共用这一份实现
// 与临界区 —— 绝不能各 TU 复制一份，那样临界区不是同一个，日志会交错。
void LogRaw(const char *fmt, ...)
{
    char line[LOG_LINE_MAX];

    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return;

    wchar_t path[MAX_PATH];
    if (BuildLogPath(path, MAX_PATH))
    {
        EnterCriticalSection(&g_logLock);
        FILE *f = _wfopen(path, L"a");
        if (f)
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f, "[%02u:%02u:%02u.%03u][pid=%lu][tid=%lu] ", st.wHour, st.wMinute,
                    st.wSecond, st.wMilliseconds, (unsigned long)GetCurrentProcessId(),
                    (unsigned long)GetCurrentThreadId());
            fputs(line, f);
            fputc('\n', f);
            fclose(f);
        }
        LeaveCriticalSection(&g_logLock);
    }

    LogBufWrite(line);
}

void Log(const char *fmt, ...)
{
    char line[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    LogRaw("%s", line);
}

static void HookLogAdapter(const char *line)
{
    Log("[hook] %s", line);
}

// ================================================================ 探针 A：il2cpp_class_from_name
// 这是框架里最有价值的一个钩子：
//   · 启动期类型解析必经它 → 命中即证明「函数体 detour + 原函数转发」整条链路可用
//   · 它**天然跑在游戏逻辑线程**（Unity 脚本线程），il2cpp 已 attach
//     ⇒ 是唯一安全的「在游戏线程上执行 il2cpp 调用」的入口（见 Mod_OnGameThread）
typedef Il2CppClass *(*ClassFromNameFn)(Il2CppImage *, const char *, const char *);
static ClassFromNameFn g_origClassFromName = nullptr;

static Il2CppClass *DetourClassFromName(Il2CppImage *image, const char *ns, const char *name)
{
    LONG n = InterlockedIncrement(&g_canaryFromName);
    if (n <= 40)
        Log("[probe] class_from_name ns=\"%s\" name=\"%s\"", ns ? ns : "", name ? name : "");

    // ★ 游戏逻辑线程上的每帧执行点。il2cpp 已 attach，可以在这里调游戏方法。
    //   你的开销较大的逻辑请在这里自己做限流 —— 这个函数每次类型查找都会命中。
    Mod_OnGameThread();

    return g_origClassFromName(image, ns, name);
}

// ================================================================ 探针 B：il2cpp_string_new
// 热点路径压测用（默认关闭，mod.ini 里 hot_canary=1 才开）
typedef Il2CppString *(*StringNewFn)(const char *);
static StringNewFn g_origStringNew = nullptr;

static Il2CppString *DetourStringNew(const char *s)
{
    LONG n = InterlockedIncrement(&g_canaryStringNew);
    if (n <= 10)
        Log("[probe-hot] string_new #%ld \"%s\"", (long)n, s ? s : "(null)");
    return g_origStringNew(s);
}

// ================================================================ 探针 C：按配置挂钩游戏方法
// x64 调用约定：RCX/RDX/R8/R9 承载前 4 个整型参数（实例方法的 RCX 即 this）。
// 只记录不修改，原样转发；更多参数在栈上，由 trampoline 后的原函数自行读取，不受影响。
typedef void *(*GenericMethodFn)(void *, void *, void *, void *);
static GenericMethodFn g_origTarget = nullptr;

static void *DetourTargetMethod(void *a1, void *a2, void *a3, void *a4)
{
    LONG n = InterlockedIncrement(&g_targetHits);
    if (n <= 20 || n % 500 == 0)
        Log("[target] hit #%ld a1=%p a2=%p a3=%p a4=%p", (long)n, a1, a2, a3, a4);
    return g_origTarget(a1, a2, a3, a4);
}

// ================================================================ 配置
static const int CFG_STR = 256;

static void TrimInPlace(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

struct Config
{
    // ★ 必须默认零初始化！ReadConfig 在配置文件不存在时直接 return false，
    // 若不初始化，cls/method 会保留栈垃圾，导致下面 `if (cfg.cls[0] && ...)`
    // 误判为「有配置」，拿乱码类名去 hook（表现为 probe 刷屏 + class not found）。
    char cls[CFG_STR] = { 0 };        // 要挂钩的类名（"Ns.Name" 或 "Name"）
    char method[CFG_STR] = { 0 };     // 要挂钩的方法名
    int  args = -1;                   // 参数个数（-1 = 不限）
    char dumpClass[CFG_STR] = { 0 };  // 引导后自动 dump 的类（"Ns.Name"）
    int  dumpMaxFields = 200;         // dump 时最多打印多少字段（0 = 不限）
    int  hotCanary = 0;               // 1 = 挂钩 il2cpp_string_new（热点压测）
};

static void CopyStr(char *dst, int cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

// 配置文件放在**模块所在目录**（与日志同目录），找不到就用全默认配置。
static bool ReadConfig(Config &cfg)
{
    wchar_t path[MAX_PATH];
    if (!g_mod || !GetModuleFileNameW(g_mod, path, MAX_PATH))
        return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash)
        wcscpy(slash + 1, L"mod.ini");

    FILE *f = _wfopen(path, L"r");
    if (!f)
        return false;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        TrimInPlace(k);
        TrimInPlace(v);

        if (strcmp(k, "class") == 0)               CopyStr(cfg.cls, CFG_STR, v);
        else if (strcmp(k, "method") == 0)         CopyStr(cfg.method, CFG_STR, v);
        else if (strcmp(k, "args") == 0)           cfg.args = atoi(v);
        else if (strcmp(k, "dump_class") == 0)     CopyStr(cfg.dumpClass, CFG_STR, v);
        else if (strcmp(k, "dump_max_fields") == 0) cfg.dumpMaxFields = atoi(v);
        else if (strcmp(k, "hot_canary") == 0)
            cfg.hotCanary = (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0);
    }
    fclose(f);
    return true;
}

// ================================================================ 引导
static int CountResolvedExports()
{
    void **p = (void **)&ir::Api();
    int n = 0;
    for (size_t i = 0; i < sizeof(Il2CppApi) / sizeof(void *); ++i)
        if (p[i]) ++n;
    return n;
}

// 返回 true = 正常等到；false = 被卸载打断 或 超时
static bool WaitForModule(const char *name, int seconds)
{
    for (int i = 0; i < seconds; ++i)
    {
        if (g_workerStop)
        {
            Log("[hot] WaitForModule: 收到停止标志，提前返回");
            return false;
        }
        if (GetModuleHandleA(name)) return true;
        if (i && i % 10 == 0) Log("... still waiting for %s (%ds)", name, i);
        Sleep(200);   // 0.2s * seconds
    }
    return GetModuleHandleA(name) != nullptr;
}

// ================================================================ 面板
// 三个钩子都跑在**游戏渲染线程**。il2cpp 调用必须走 Mod_OnGameThread / Mod_OnTick。

static void PanelBegin(bool *open)
{
    ImGui::SetNextWindowSize(ImVec2(MOD_PANEL_W, MOD_PANEL_H), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin(MOD_PANEL_TITLE, open,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    // 全局文本换行：长句到右边界自动折行（不再被裁）
    ImGui::PushTextWrapPos(0.0f);
}

// 「状态」tab：一眼看清引导走到哪一步了 —— 排查「注入了但没反应」时先看这里
static void PanelDrawStatus()
{
    ImGui::TextDisabled("引导状态 —— 注入后没反应时先看这里");
    ImGui::Separator();

    ImGui::Text("IL2CPP 模块 : %s", MOD_GAME_MODULE);
    ImGui::Text("模块路径    : %s", g_modulePath[0] ? g_modulePath : "(取不到)");
    ImGui::Text("已解析导出  : %d", g_exportsResolved);
    ImGui::Text("域状态      : %s", ir::Domain() ? "已获取" : "未获取");
    ImGui::Text("引导完成    : %s", g_stateReady ? "是" : "否");
    ImGui::Text("镜像数量    : %d", g_imageCount);
    ImGui::Text("渲染帧数    : %lu", UiFrameCount());
    ImGui::Text("本镜像基址  : 0x%llX", (unsigned long long)ModSelfBase());

    ImGui::Separator();
    ImGui::TextDisabled("detour 命中计数（0 = 钩子没生效）");
    ImGui::Text("  class_from_name : %ld", (long)g_canaryFromName);
    ImGui::Text("  string_new      : %ld", (long)g_canaryStringNew);
    ImGui::Text("  target          : %ld", (long)g_targetHits);

    ImGui::Separator();
    ImGui::TextDisabled("热重载");
    ImGui::Text("  互斥体 : %ls", ModLockName());
    ImGui::Text("  已加载 : %s | ABI v%d | 编译版本 0x%X",
                ModIsLoaded() ? "是" : "否", ModAbiVersion(), ModVersion());

    ImGui::Separator();
    ImGui::TextDisabled("日志同时写入模块目录下的 %ls", MOD_LOG_FILE);
}

static void PanelDraw()
{
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable))
    {
        if (ImGui::BeginTabItem("日志"))
        {
            LogPanelDraw();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("状态"))
        {
            PanelDrawStatus();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("工具"))
        {
            ToolDrawInPanel();
            ImGui::EndTabItem();
        }
        // 用户自己的 tab 追加在这里
        Mod_DrawPanelTabs();
    }
    ImGui::EndTabBar();
}

static void PanelEnd()
{
    // 把窗口矩形上报给 overlay —— 光标可见性判断与「吃掉点击」都依赖它。
    // 必须在本窗口 End() 之前取。
    // 用公开的 GetWindowPos/GetWindowSize；ImGui::GetCurrentWindow() 是内部接口，
    // 跨编译单元用不稳。
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    UiSetPanelRect(wp.x, wp.y, wp.x + ws.x, wp.y + ws.y);
    ImGui::PopTextWrapPos();
    ImGui::End();
}

// 每帧钩子（面板关着也跑）。★ 别把重要心跳放进 drawPanel ——
// 用户一切到别的 tab 或关掉面板，它就停摆了。
static void ModFrameHook()
{
    if (g_toolTick) g_toolTick();
    Mod_OnFrame();
}

// ================================================================ Worker
static DWORD WINAPI Worker(LPVOID)
{
    Log("=== il2cpp mod attached; beginning bootstrap ===");
    if (g_workerStop) { Log("[hot] Worker 启动时即收到停止标志，退出"); return 0; }

    if (!WaitForModule(MOD_GAME_MODULE, MOD_GAME_MODULE_WAIT))
    {
        Log("FATAL: %s never loaded", MOD_GAME_MODULE);
        return 0;
    }
    HMODULE gameMod = GetModuleHandleA(MOD_GAME_MODULE);
    Log("%s @ %p", MOD_GAME_MODULE, (void *)gameMod);

    if (!ir::Init())
    {
        Log("FATAL: il2cpp export resolve failed (Init returned false)");
        return 0;
    }
    g_exportsResolved = CountResolvedExports();
    Log("il2cpp exports resolved: %d", g_exportsResolved);

    // ---- 装 ImGui overlay（在等 domain 之前就装，界面能立刻显示引导进度）----
    // 取 device 的方式见 src/ui/imgui_overlay.cpp：建 dummy swapchain 拿全局唯一的
    // Present 地址再 detour，不依赖「游戏是否已创建 device」这个时序条件。
    UiSetHooks(PanelBegin, PanelDraw, PanelEnd);
    UiSetFrameHook(ModFrameHook);
    if (UiOverlayInstall())
        Log("ui: overlay installed (%s toggles panel)", MOD_TOGGLE_HINT);
    else
        Log("WARN: ui overlay install failed — panel will not show");

    // ---- 等 Unity 域出现 ----
    // 游戏还在启动画面时域可能还没建好，这里轮询等；每步都检查停止标志，
    // 否则热重载时 Worker 会卡在这一直不退出。
    for (int i = 0; i < 240 && ir::Domain() == nullptr; ++i)
    {
        if (g_workerStop) { Log("[hot] 等 domain 时收到停止标志，放弃引导"); return 0; }
        if (i && i % 20 == 0)
            Log("... waiting for Unity domain (%ds)", i / 2);
        Sleep(200);
    }
    if (!ir::EnsureAttached())
    {
        Log("FATAL: thread attach to il2cpp domain failed (domain=%p)", (void *)ir::Domain());
        return 0;
    }
    Log("domain attached: domain=%p thread=%p", (void *)ir::Domain(),
        (void *)ir::Api().il2cpp_thread_current());

    // ---- 加载工具 DLL（可选）----
    // ★ 必须等 domain attach 之后再加载。工具 DLL 的 Init 里通常会
    //   `while (!il2cpp_is_vm_thread(nullptr)) Sleep(1000);` —— 它在**调用线程**上查
    //   「当前线程是不是 VM 线程」。Worker attach 之前查必然 false，工具 DLL 会无限
    //   Sleep 卡死 Worker（表现为注入后毫无反应）。放在 EnsureAttached 之后就没事。
    if (MOD_TOOL_DLL[0] && !LoadToolDll())
        Log("WARN: 工具 DLL 未加载（%ls），[工具] tab 将为空", MOD_TOOL_DLL);

    // ---- 反射自测：枚举程序集 / 镜像 / 类（验证反射链路）----
#if MOD_REFLECT_SELFTEST
    g_imageCount = ReflectionSelfTest();
#endif

    // ---- 配置 ----
    Config cfg;
    bool hasCfg = ReadConfig(cfg);
    Log("config: %s (class=\"%s\" method=\"%s\" args=%d dump_class=\"%s\" hot_canary=%d)",
        hasCfg ? "loaded" : "absent", cfg.cls, cfg.method, cfg.args, cfg.dumpClass,
        cfg.hotCanary);

    // ---- 探针 A ----
    if (ir::Api().il2cpp_class_from_name)
    {
        void *fn = (void *)ir::Api().il2cpp_class_from_name;
        g_origClassFromName = (ClassFromNameFn)ih::Detour(fn, (void *)DetourClassFromName);
        if (g_origClassFromName)
            Log("PROBE-A installed: il2cpp_class_from_name @ %p -> tramp %p",
                fn, (void *)g_origClassFromName);
        else
            Log("PROBE-A FAILED to install on il2cpp_class_from_name @ %p", fn);
    }
    else
    {
        Log("PROBE-A skipped: il2cpp_class_from_name export missing");
    }

    // ---- 探针 B（热点压测，默认关）----
    if (cfg.hotCanary && ir::Api().il2cpp_string_new)
    {
        void *fn = (void *)ir::Api().il2cpp_string_new;
        g_origStringNew = (StringNewFn)ih::Detour(fn, (void *)DetourStringNew);
        if (g_origStringNew)
            Log("PROBE-B installed: il2cpp_string_new @ %p -> tramp %p", fn, (void *)g_origStringNew);
        else
            Log("PROBE-B FAILED to install on il2cpp_string_new @ %p", fn);
    }

    // ---- 探针 C：按配置挂钩游戏方法 ----
    // 必须 hasCfg（配置文件确实存在且解析成功）+ 两个字段都非空才 hook。
    if (hasCfg && cfg.cls[0] && cfg.method[0])
    {
        g_origTarget = (GenericMethodFn)ih::HookMethod(cfg.cls, cfg.method, cfg.args,
                                                       (void *)DetourTargetMethod);
        if (g_origTarget)
            Log("TARGET hooked: %s::%s(args=%d) -> tramp %p", cfg.cls, cfg.method, cfg.args,
                (void *)g_origTarget);
        else
            Log("TARGET hook FAILED: %s::%s(args=%d)", cfg.cls, cfg.method, cfg.args);
    }
    else
    {
        Log("TARGET hook skipped (no class/method in mod.ini)");
    }

    // ---- 可选：按配置 dump 一个类（排查字段偏移用）----
    if (cfg.dumpClass[0])
        ReflectDumpClass(cfg.dumpClass, nullptr, cfg.dumpMaxFields);

    // ---- 引导完成，交给用户代码 ----
    g_stateReady = 1;
    Mod_OnReady();

    // ---- 主循环：每秒 tick + 心跳 ----
    // 注意：用户的周期探测请放在 Mod_OnTick（就是这里调的），**不要**放进
    // class_from_name 的函数体钩子 —— 热重载后旧镜像留在 GameAssembly 代码页上的
    // 补丁不会被清掉，新镜像的 detour 会装在已改写的字节上而静默失效。
    unsigned tick = 0;
    for (;;)
    {
        if (g_workerStop) { Log("[hot] 主循环收到停止标志，退出"); return 0; }
        Sleep(1000);
        Mod_OnTick();
        if (++tick % 10 == 0)
            Log("heartbeat: %us class_from_name=%ld string_new=%ld target=%ld", tick,
                (long)g_canaryFromName, (long)g_canaryStringNew, (long)g_targetHits);
    }
}

// ================================================================ 工具 DLL（可选）
// 双 DLL 架构：本 DLL 手动映射（零 .tls、无 STL、无异常），分析工具走普通
// LoadLibraryW 加载 —— 它由系统加载器管理 TLS/CRT，可以自由用 STL/异常。
//
// 加载时机：必须在 ImGui::CreateContext() 之后（工具要 SetCurrentContext），
// 所以放在 Worker 里 UiOverlayInstall 之后。overlay 初始化是异步的
// （Present 第一次被调用才建 device + context），这里轮询 UiImGuiContext()。
using ToolInitFn     = void (*)(void *, void *);
using ToolDrawFn     = void (*)();
using ToolShutdownFn = void (*)();

static HMODULE        g_toolMod = nullptr;
static ToolDrawFn     g_toolDraw = nullptr;
static ToolShutdownFn g_toolShutdown = nullptr;

static bool LoadToolDll()
{
    if (g_toolMod) return true;

    // 按优先级依次尝试多个候选目录，第一个能加载成功的就用：
    //   1) PickLogDir() —— 日志文件已确定落地的目录，工具 DLL 就该放这儿
    //   2) 环境变量 MOD_DIR_ENV（注入时显式指定）
    //   3) 本进程 CWD
    //   4) %TEMP%
    //
    // 坑：Windows 上 LoadLibraryW 对**路径不存在**也返回 err=126
    // （ERROR_MOD_NOT_FOUND）—— 和「依赖缺失」同一个错误码，极易误判。
    // 所以下面先用 GetFileAttributesW 确认文件在不在，把两种情况区分开。
    wchar_t cands[4][MAX_PATH] = { 0 };
    int nCand = 0;

    wchar_t logDir[MAX_PATH] = { 0 };
    if (PickLogDir(logDir, MAX_PATH) && logDir[0])
        wcscpy_s(cands[nCand++], MAX_PATH, logDir);
    if (GetEnvironmentVariableW(MOD_DIR_ENV, cands[nCand], MAX_PATH) > 0)
        ++nCand;
    DWORD cwdLen = GetCurrentDirectoryW(MAX_PATH, cands[nCand]);
    if (cwdLen > 0 && cwdLen < MAX_PATH)
        ++nCand;
    if (GetTempPathW(MAX_PATH, cands[nCand]) > 0)
        ++nCand;

    for (int i = 0; i < nCand && !g_toolMod; ++i)
    {
        if (!cands[i][0]) continue;
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", cands[i], MOD_TOOL_DLL);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        {
            Log("[tool] 候选 %ls 不存在（跳过）", path);
            continue;
        }
        g_toolMod = LoadLibraryW(path);
        if (g_toolMod)
        {
            Log("[tool] 已加载 %ls @ %p", path, (void *)g_toolMod);
        }
        else
        {
            DWORD e = GetLastError();
            Log("[tool] 候选 %ls 存在但加载失败 (err=%lu%s)", path, e,
                e == 126 ? " MOD_NOT_FOUND=依赖DLL缺失或架构不符" :
                e == 193 ? " BAD_EXE_FORMAT=架构不符" : "");
        }
    }

    if (!g_toolMod)
    {
        Log("[tool] 所有候选目录都没找到可用的 %ls", MOD_TOOL_DLL);
        return false;
    }

    ToolInitFn init = (ToolInitFn)GetProcAddress(g_toolMod, "Tool_Init");
    g_toolDraw      = (ToolDrawFn)GetProcAddress(g_toolMod, "Tool_Draw");
    g_toolShutdown  = (ToolShutdownFn)GetProcAddress(g_toolMod, "Tool_Shutdown");
    g_toolTick      = (ToolTickFn)GetProcAddress(g_toolMod, "Tool_Tick");
    Log("[tool] 导出: Init=%p Draw=%p Shutdown=%p Tick=%p", (void *)init, (void *)g_toolDraw,
        (void *)g_toolShutdown, (void *)g_toolTick);

    if (!init || !g_toolDraw)
    {
        Log("[tool] 导出解析失败 (Init=%p Draw=%p)", (void *)init, (void *)g_toolDraw);
        FreeLibrary(g_toolMod);
        g_toolMod = nullptr;
        return false;
    }

    // 等 overlay 把 imgui 上下文建起来（Present 首次调用后才存在）
    void *ctx = nullptr;
    for (int i = 0; i < 100; ++i)
    {
        ctx = UiImGuiContext();
        if (ctx) break;
        Sleep(100);
    }
    if (!ctx)
        Log("[tool] WARN: 等不到 imgui 上下文，工具仍会 Init（界面可能不显示）");
    else
        Log("[tool] imgui ctx = %p", ctx);

    // 把日志出口也传过去：工具 DLL 自己的 logger 只在内存里，崩了看不到，
    // 转发到本模块的日志文件才能跨 DLL 排查。
    init(ctx, (void *)&UiLogRaw);
    Log("[tool] Tool_Init 已调用");
    return true;
}

static void ToolDrawInPanel()
{
    if (g_toolDraw)
        g_toolDraw();
    else
        ImGui::TextDisabled("未加载工具 DLL（项目根目录 mod_config.h 里 MOD_TOOL_DLL 置空可关掉这个 tab）");
}

// 卸载工具 DLL（热重载用）。
// 为什么必须做这一步：工具 DLL 是 LoadLibraryW 加载的，若不在卸载注入侧时
// FreeLibrary 它，旧 DLL 会**残留**在进程里 —— 下次注入同一路径的 LoadLibraryW
// 会直接返回旧句柄（已加载），新编的 DLL 内容根本不生效，热重载等于白改。
//
// 时机约束：必须在 overlay 卸载（UiShutdown）**之后**调用 —— 那时渲染线程已停止
// 调 Tool_Draw，否则 FreeLibrary 时工具 DLL 的代码还在调用栈上 -> 崩。
void UnloadToolDll()
{
    if (!g_toolMod) return;
    if (g_toolShutdown)
    {
        Log("[tool] 调用 Tool_Shutdown");
        g_toolShutdown();
    }
    // 清掉函数指针，防止渲染线程在 FreeLibrary 后仍调到野指针
    g_toolDraw = nullptr;
    g_toolShutdown = nullptr;
    g_toolTick = nullptr;

    BOOL ok = FreeLibrary(g_toolMod);
    Log("[tool] FreeLibrary(%p) = %d (GetLastError=%lu)", (void *)g_toolMod, (int)ok,
        (unsigned long)GetLastError());
    g_toolMod = nullptr;
}

// ================================================================ 卸载
// 停 Worker：置停止标志 + 等它自己退出（最多 3 秒）。
// 不能强杀 —— Worker 里正在写日志/调 imgui，强行 TerminateThread 会让
// 临界区和文件句柄停在中间状态。
void ShutdownWorkerThread()
{
    if (!g_workerThread) return;
    InterlockedExchange(&g_workerStop, 1);
    if (WaitForSingleObject(g_workerThread, 3000) == WAIT_TIMEOUT)
        Log("[hot] WARN: Worker 未在 3s 内退出（可能卡在等 domain），继续清理");
    else
        Log("[hot] Worker 线程已退出");
    g_workerThread = nullptr;   // 置空避免二次等待
}

// ================================================================ 入口
BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_mod = mod;
        DisableThreadLibraryCalls(mod);
        InitializeCriticalSection(&g_logLock);
        ih::SetLogger(HookLogAdapter);

        // 记下模块路径（面板「状态」tab 显示用）。手动映射时这里常常取不到，
        // 取不到就是 "(取不到)" —— 那意味着日志会降级落到 CWD 或 %TEMP%。
        if (GetModuleFileNameW(mod, g_modulePathW, MAX_PATH) == 0)
        {
            g_modulePathW[0] = 0;
            g_modulePath[0] = 0;
        }
        else
        {
            WideCharToMultiByte(CP_UTF8, 0, g_modulePathW, -1, g_modulePath,
                                MAX_PATH, NULL, NULL);
        }

        // 单实例守卫：本进程已经有实例在跑就放弃本次引导（避免两个 Worker 打架）
        ModSetSelf(mod, (ULONG_PTR)mod);
        if (!ModClaimInstance())
        {
            Log("[hot] 已有实例在运行，本次注入放弃（不启 Worker）");
            return TRUE;
        }

        // 句柄**保留**（不 CloseHandle）：ShutdownWorkerThread 要用它
        // WaitForSingleObject 等线程自己退出。
        g_workerThread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
        if (!g_workerThread)
            Log("[hot] WARN: CreateThread 失败，Worker 未启动");
    }
    return TRUE;
}
