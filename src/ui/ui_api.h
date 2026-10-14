// ui_api.h — ImGui 覆盖层（src/ui/imgui_overlay.cpp）对外接口
// ============================================================================
// 三个钩子与调用线程（这是本框架最容易踩的地方，务必看清）：
//
//   beginPanel / drawPanel / endPanel  —— 跑在**游戏渲染线程**（Present 钩子里）
//   frameHook                          —— 跑在**游戏渲染线程**，面板关着也每帧执行
//   overlayDrawHook                    —— 跑在**游戏渲染线程**，NewFrame 之后、Render 之前
//
// 而 IL2CPP 的 runtime_invoke 只能跑在**游戏逻辑线程**（已 attach 域的线程）。
// 两者不是同一个线程 ⇒ **绝不能在按钮回调里直接调游戏方法**。
// 正确做法：回调里只写请求（volatile 标志 / 队列），真正的执行放到
// src/user_hooks.cpp 的 Mod_OnTick（Worker 线程，已 attach）或 Mod_OnFrame。
// ============================================================================
#pragma once

#include <windows.h>

// ---- 生命周期 ----
// 安装 D3D11 Present 钩子。返回 1 = 成功。
// 内部会自建一条 dummy swapchain 取 Present 的真实地址（dxgi!Present 全进程唯一一份
// 代码），然后改**共享 vtable 的 [8] 槽** —— 不动函数体，因此不依赖注入时机。
// 详见 imgui_overlay.cpp 头部注释。
int UiOverlayInstall();

// 卸载：置标志并等渲染线程完成真正的清理（还原 vtable / 销毁 imgui 上下文）。
// 不要在渲染线程上直接调用它。
void UiShutdown();

// ★ 游戏主动退出时的"抢跑清理"（点 X / Alt+F4 / 注销）。
// 只做两件必须抢在进程销毁序列之前完成的事，**快且幂等**（正常 <50ms）：
//   1) 卸掉全局 WH_MOUSE_LL 钩子（否则系统输入线程会等一个马上要被杀掉的线程）
//   2) 还原 dxgi vtable[8]/[13]（否则退出过程中游戏还会进我们的钩子）
// 可在任意线程调用（包括游戏消息线程 —— 它不等待渲染线程）。
// 之后的完整清理仍然交给 ModUnload()。
void UiExitFastPath();

// 渲染线程侧是否已清理完（给卸载流程轮询用）。
extern "C" int UiShutdownCompleted();

// ---- 三个绘制钩子 ----
// open：面板可见性指针，drawPanel 里可写它来做「标题栏 X 关闭」。
void UiSetHooks(void (*beginPanel)(bool *), void (*drawPanel)(), void (*endPanel)());

// 每帧钩子（面板关着也跑）。适合驱动心跳、消费跨线程队列。
void UiSetFrameHook(void (*fn)());

// 全屏叠加绘制钩子（NewFrame 后 / Render 前）。适合画准星、世界坐标标注。
void UiSetOverlayDrawHook(void (*fn)());

// ---- 状态查询 ----
bool UiIsReady();              // overlay 是否已初始化（Present 至少被调用过一次）
unsigned long UiFrameCount();  // 已渲染帧数
HWND UiGameHwnd();             // 游戏窗口句柄
bool UiPanelVisible();
void UiTogglePanel();
bool UiCursorOverPanel();      // 光标是否悬停在面板上
bool UiPointOverPanel(int screenX, int screenY);

// 把面板矩形上报给 overlay —— 光标可见性判断与「吃掉点击」都依赖它。
// 必须在窗口 End() 之前调用。
void UiSetPanelRect(float x0, float y0, float x1, float y1);

// ---- 给工具 DLL 用 ----
void *UiImGuiContext();        // imgui 上下文裸指针
void UiLogRaw(const char *s);  // 日志出口（写本模块日志文件）
