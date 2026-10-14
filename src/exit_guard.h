// exit_guard.h — 游戏**主动退出**时的清理触发器（点 X / Alt+F4 / 注销 / 关机）
// ============================================================================
// 为什么需要它（原项目踩过的坑，务必读）：
//   本模块走手动映射，**不在进程的 PEB 模块列表里**。游戏自己退出时走的是
//   ExitProcess -> LdrShutdownProcess：它会先杀掉除主线程外的所有线程，再只通知
//   「加载器登记过的」模块做 DLL_PROCESS_DETACH。我们这份镜像两条都不在名单里：
//     · 它不会收到任何退出通知 —— 所有钩子、线程、vtable 补丁原样留着；
//     · 我们起的 Worker / 鼠标钩子 pump 线程会被**直接砍掉**，
//       而此刻系统可能正等着我们的 WH_MOUSE_LL 回调返回（低级钩子是同步回调）。
//   结果就是退出序列卡住，只能在任务管理器强制结束进程。
//
//   另外注意：**点击 X 本身就是一次鼠标事件**，它必须穿过我们注册的全局
//   WH_MOUSE_LL 钩子才能送到游戏窗口 —— 挂着钩子点关闭，等于在最危险的时刻
//   把系统输入线程和我们的 pump 线程绑在一起。
//
// 对策：在游戏开始拆自己之前，把我们的东西主动拆干净：
//   ① 子类化游戏窗口过程，捕获 WM_CLOSE / WM_DESTROY / WM_ENDSESSION / WM_QUIT
//      —— 这是最早、最精确的「用户要退出了」信号（点击 X 必走 WM_CLOSE）；
//   ② 另起一个看门狗线程轮询 IsWindow(gameHwnd)，兜住「窗口被以别的方式销毁」；
//   ③ 触发后**在独立线程**上执行清理（绝不占着游戏消息线程），
//      第一步就是卸掉全局鼠标钩子 + 还原 dxgi vtable。
//
// 三个信号都只是"触发器"，真正干活的是 hot_reload.cpp 的 ModExitCleanup()
// （它复用 ModUnload 的完整清理，幂等）。
// ============================================================================
#pragma once

#include <windows.h>

// 触发后要执行的回调。**在独立线程上调用**，允许阻塞（内部会有若干秒等待），
// 但绝不能反过来去等游戏消息线程，否则自己造死锁。
typedef void (*ModExitCleanupFn)(void);

// 注册清理回调（必须先于 ExitGuard_AttachToWindow 调用）。
void ExitGuard_SetHandler(ModExitCleanupFn fn);

// 拿到游戏窗口后调用。
// ★ 必须在**游戏窗口线程**上调用 —— 子类化窗口过程要求同线程，否则消息队列的
//   同步（SendMessage 等）会出现时序问题。BindToChain() 跑在游戏渲染线程，
//   而 Unity 的 D3D11 Present 就在主线程上，正好满足。
// 幂等：重复调用只有第一次生效。
void ExitGuard_AttachToWindow(HWND hwnd);

// 拆掉守护（还原窗口过程 + 停看门狗 + 还原消息钩子）。幂等。
// 由 ModUnload() 调用：注入器主动热重载卸载时，不能让守护还挂在那儿。
void ExitGuard_Detach(void);

// 是否已经触发过退出清理（1 = 已触发）。
int ExitGuard_Fired(void);
