// user_hooks.cpp — **你的代码写在这里**
// ============================================================================
// 框架把六个扩展点留给你，本文件里全是空实现 + 可直接改的示例。
// 主框架（main.cpp / probe.cpp / ui/）不用改。
//
// 六个扩展点与它们的调用线程（★ 线程搞错就是随机崩溃）：
//
//   Mod_OnReady()       Worker 线程，il2cpp 引导完成、域已挂接，只调一次
//                       → 做一次性初始化（找类、拿单例、装你自己的钩子）
//
//   Mod_OnTick()        Worker 线程，每秒一次，已 attach 域
//                       → 周期性地读游戏状态（静态字段、单例里的字段都安全）
//
//   Mod_OnFrame()       游戏**渲染**线程，每帧（面板关着也跑）
//                       → 只做纯绘制/轻量状态推进。★ 不能调 il2cpp
//
//   Mod_OnGameThread()  游戏**逻辑**线程（class_from_name 钩子里），il2cpp 已 attach
//                       → ★ 唯一能安全执行 il2cpp 调用（创建对象、调方法）的地方
//                       ★ 每次类型查找都会命中，开销大的逻辑必须自己限流
//
//   Mod_DrawPanelTabs() 渲染线程，在面板的 TabBar 里 —— 在这里 BeginTabItem 加你自己的 tab
//
//   Mod_OnShutdown()    卸载时（在 IL2CPP detour 还原之前），两种场景都会调：
//                        · 注入器主动卸载 / 热重载（ModUnload）
//                        · **游戏自己退出**（点 X / Alt+F4 / 注销，见 src/exit_guard.h）
//                       → 停你起的线程、还你借的资源
//
// ----------------------------------------------------------------------------
// ★★ 最重要的模式：跨线程请求队列 ★★
//
// ImGui 按钮回调跑在渲染线程，而「创建 Unity 对象 / 调游戏方法」只能在
// 游戏逻辑线程做。在渲染线程里直接调 = 随机崩溃（而且崩点离调用点很远）。
//
// 正确做法（下面给了完整可跑的实现）：
//
//     渲染线程按钮回调  ──写请求──▶  volatile 请求槽
//                                          │
//     游戏逻辑线程 Mod_OnGameThread() ──读请求──▶ 真正执行
//
// 请求槽用 volatile + InterlockedXxx 传递，不要用 std::mutex/std::queue
// （会引入静态 TLS 与 libstdc++ 依赖，见 main.cpp 顶部的手动映射约束）。
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include "il2cpp/il2cpp_runtime.h"
#include "hook/il2cpp_hook.h"
#include "mod_config.h"
#include "probe.h"

namespace ir = il2cpp::runtime;

extern void Log(const char *fmt, ...);   // main.cpp

// ================================================================ 请求队列示例
// 一个槽 = 一个待执行的请求。真实项目里可以扩成定长环形数组。
namespace
{
    // 请求类型：示例里做一个「打印指定类的字段结构」
    enum ReqKind
    {
        REQ_NONE = 0,
        REQ_DUMP_CLASS,      // dump 一个类
        REQ_LOG_HELLO,       // 打一行日志（最小可用的验证）
    };

    struct Request
    {
        volatile LONG kind;
        char          text[128];   // 参数（类名等）
    };

    Request g_req;                       // 单槽。多个并发请求就扩成定长数组 + 原子索引
    volatile LONG g_reqSeq = 0;          // 已提交请求计数（面板显示用）
    volatile LONG g_reqDone = 0;         // 已完成请求计数

    // 上一次 dump 的结果摘要（面板回显）
    char   g_lastResult[256] = { 0 };
    volatile LONG g_hasResult = 0;
}

// 渲染线程侧：提交一个请求（只写槽，不执行）
static void PostRequest(ReqKind kind, const char *text)
{
    // ★ 单槽实现：同时只能挂一个未消费的请求。真实项目改成环形数组。
    strncpy(g_req.text, text ? text : "", sizeof(g_req.text) - 1);
    g_req.text[sizeof(g_req.text) - 1] = 0;
    InterlockedExchange(&g_req.kind, (LONG)kind);
    InterlockedIncrement(&g_reqSeq);
}

// 游戏逻辑线程侧：消费一个请求（真正干活的地方）
static void ConsumeRequest()
{
    LONG kind = InterlockedExchange(&g_req.kind, REQ_NONE);
    if (kind == REQ_NONE) return;

    switch (kind)
    {
    case REQ_LOG_HELLO:
        Log("[user] hello from the game logic thread (tid=%lu)",
            (unsigned long)GetCurrentThreadId());
        snprintf(g_lastResult, sizeof(g_lastResult), "已在游戏逻辑线程打了一条日志");
        break;

    case REQ_DUMP_CLASS:
        // ★ 这里跑在游戏逻辑线程，il2cpp 已 attach —— 可以安全做反射调用
        Log("[user] dump 请求：%s", g_req.text);
        ReflectDumpClass(g_req.text, nullptr, 200);
        snprintf(g_lastResult, sizeof(g_lastResult), "已 dump \"%s\"，详见日志",
                 g_req.text);
        break;

    default:
        break;
    }

    InterlockedExchange(&g_hasResult, 1);
    InterlockedIncrement(&g_reqDone);
}

// ================================================================ 扩展点实现

// 只调一次：il2cpp 引导完成、域已挂接（Worker 线程）
void Mod_OnReady()
{
    Log("[user] Mod_OnReady —— 反射已可用，在这里找类 / 拿单例 / 装你自己的钩子");

    // ---- 示例：确认游戏主程序集找得到 ----
    Il2CppImage *img = ir::Image(MOD_MAIN_IMAGE);
    Log("[user] 主程序集镜像 %s = %p", MOD_MAIN_IMAGE, (void *)img);

    // ---- 示例：拿一个泛型单例的实例 ----
    // 很多 Unity 游戏用 SingletonMonoBehaviour<T>，实例存在基类的静态字段 s_instance 上，
    // 且静态字段的槽位在**膨胀后的实例化类**里 —— 所以要沿父类链找。
    //
    //   Il2CppClass *klass = ir::FindClass("Game.Battle", "BattleManager");
    //   if (klass) {
    //       Il2CppClass *owner = nullptr;
    //       FieldInfo *sf = FindStaticFieldInParents(klass, "s_instance", &owner);
    //       if (sf && ir::Api().il2cpp_field_static_get_value) {
    //           void *inst = nullptr;
    //           ir::Api().il2cpp_field_static_get_value(sf, &inst);
    //           Log("[user] 单例实例 = %p", inst);
    //           // ★ 把 inst 存到全局，Mod_OnTick 里周期性读它的字段
    //       }
    //   }
    //
    // 想 dump 一个类看字段偏移：在 mod.ini 里写 dump_class = 命名空间.类名
    // 不用改代码，下次注入就会自动打印（见 probe.cpp 的 ReflectDumpClass）。
}

// Worker 线程，每秒一次（已 attach 域）
void Mod_OnTick()
{
    // ---- 示例：每秒读一次单例里的字段，只在值变化时打日志（★ 必须限流）----
    //
    //   static int s_lastHp = -1;
    //   void *inst = g_mySingleton;              // Mod_OnReady 里缓存的
    //   if (inst && g_fHp) {
    //       int hp = 0;
    //       if (ir::Api().il2cpp_field_get_value)
    //           ir::Api().il2cpp_field_get_value((Il2CppObject *)inst, g_fHp, &hp);
    //       if (hp != s_lastHp) { Log("[user] hp %d -> %d", s_lastHp, hp); s_lastHp = hp; }
    //   }
    //
    // ★ 只在变化时打日志。每帧/每秒无脑 Log() 会把日志文件刷到几百 MB，
    //   而且写文件是临界区，会拖慢游戏逻辑线程。
    //
    // ★ 跨场景/重开一局后别再用缓存的 Unity 对象指针 —— 判存活看
    //   m_CachedPtr(+0x10) != nullptr，别用 activeSelf / name 判断。
}

// 游戏渲染线程，每帧（面板关着也跑）
void Mod_OnFrame()
{
    // ★ 这里不能调 il2cpp（不在游戏逻辑线程上）。只做轻量状态推进。
    // 想画全屏叠加（准星、世界坐标标注）用：
    //   UiSetOverlayDrawHook(MyOverlayDraw);   // 在 Mod_OnReady 里注册一次
}

// 游戏逻辑线程（class_from_name 钩子里），il2cpp 已 attach
void Mod_OnGameThread()
{
    // ★ 这个函数每次游戏做类型查找都会命中 —— 高频！
    // 只做「消费请求」这种 O(1) 的事，别在这里跑循环、别在这里 Log 无意义的东西。
    ConsumeRequest();
}

// 渲染线程，在面板 TabBar 里追加你自己的 tab
void Mod_DrawPanelTabs()
{
    if (!ImGui::BeginTabItem("我的功能"))
        return;

    // ---- 示例：两个按钮 + 结果回显，完整演示「请求队列」模式 ----
    ImGui::TextDisabled("按钮跑在渲染线程，真正执行在游戏逻辑线程（见 user_hooks.cpp）");
    ImGui::Separator();

    static char clsBuf[128] = { 0 };
    ImGui::SetNextItemWidth(320.0f);
    ImGui::InputTextWithHint("##cls", "要 dump 的类（命名空间.类名）", clsBuf, sizeof(clsBuf));

    if (ImGui::Button("① 在游戏逻辑线程打日志", ImVec2(260.0f, 0.0f)))
        PostRequest(REQ_LOG_HELLO, "");
    ImGui::SameLine();
    ImGui::BeginDisabled(!clsBuf[0]);
    if (ImGui::Button("② dump 这个类", ImVec2(180.0f, 0.0f)))
        PostRequest(REQ_DUMP_CLASS, clsBuf);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("已提交 %ld 条，已完成 %ld 条", (long)g_reqSeq, (long)g_reqDone);
    if (g_hasResult)
        ImGui::TextWrapped("上次结果：%s", g_lastResult);

    ImGui::TextDisabled("提示：不想改代码就想 dump 一个类，在 mod.ini 里填 dump_class = ...");

    ImGui::EndTabItem();
}

// 卸载时清理（在 IL2CPP detour 还原**之前**调用）。
// 两种场景都会走到这里：注入器主动卸载/热重载，以及**游戏自己退出**（点 X）。
void Mod_OnShutdown()
{
    Log("[user] Mod_OnShutdown —— 停你起的线程、还你借的资源");
    // 例：置停止标志 + 等你自己 CreateThread 起的线程退出。
    // ★ 镜像随后会被 VirtualFreeEx（注入器卸载）/ 进程即将退出（游戏自己退出），
    //   跑在镜像里的线程必须在此之前退出，否则线程执行已释放的代码 -> 崩。
    // ★ 本函数可能被**游戏退出清理线程**调用（不是注入器线程），
    //   别在这里等游戏消息线程或渲染线程，否则会把退出流程拖住。
}
