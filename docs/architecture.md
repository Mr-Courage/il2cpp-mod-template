# 架构

## 一、设计前提：手动映射

本 DLL **不是**用 `LoadLibrary` 加载的，而是由注入器手动把 PE 映像写进目标进程并修复重定位后，直接跳入口点执行。这带来三个后果，决定了整个框架的形态：

1. **不在 PEB 模块列表里** → `GetModuleFileNameW(g_mod, ...)` 直接失败返回 0
   （所以日志路径必须逐级降级，见 `main.cpp` 的 `PickLogDir`）
2. **没有 `FreeLibrary`** → 热重载必须自己实现完整的逆序清理（见 `hot_reload.cpp`）
3. **映射器未模拟静态 TLS** → 产物不能含 `.tls` 段、`__emutls_*`、libstdc++ EH

第 3 条是最狠的：它**不会报错**，只会在注入后随机崩，而且崩点离违规处很远。所以整个项目刻意避开 STL、异常、RTTI、`std::thread`、`thread_local`、函数内 static 非平凡对象。

## 二、启动流程

```
注入器 ──▶ DllMain(PROCESS_ATTACH)
             ├─ InitializeCriticalSection
             ├─ ih::SetLogger(HookLogAdapter)
             ├─ ModClaimInstance()          ← 带 PID 的命名互斥体，抢不到就放弃
             │                                 （防止同进程两份镜像打架）
             └─ CreateThread(Worker)        ← ★ 绝不在 DllMain 里做重活
                    │
Worker 线程 ─────────┘
  1. WaitForModule(MOD_GAME_MODULE)    等 IL2CPP 模块加载
  2. ir::Init()                        解析 IL2CPP 全部导出
  3. UiSetHooks(...) + UiOverlayInstall()   装 ImGui（不等 domain，界面立刻能看进度）
  4. 轮询等 ir::Domain() != null
  5. ir::EnsureAttached()              把 Worker 挂到 IL2CPP 域
  6. LoadToolDll()（可选）             ★ 必须在 EnsureAttached 之后
  7. ReflectionSelfTest()              枚举程序集/镜像/类，验证反射链路
  8. ReadConfig() + 装三个探针
  9. Mod_OnReady()   ← 用户代码
 10. 主循环：Sleep(1000) → Mod_OnTick() → 心跳

另外（游戏渲染线程，第一次 Present 时）：
     BindToChain() → 初始化 imgui → ★ ExitGuard_AttachToWindow(hwnd)
                                                  子类化窗口过程 + 起看门狗，
                                                  捕获"点 X 退出"（见第四·exit_guard）
```

每一步都检查 `g_workerStop`，否则热重载时 Worker 会卡在等待里不退出。

## 三、线程模型（★ 最容易踩的地方）

进程里有六条线程与我们相关：

| 线程 | 我们在这里做什么 | 能不能调 il2cpp |
| --- | --- | --- |
| **注入器线程** | `ModUnload` 的清理流程 | ✗ |
| **Worker 线程**（我们创建） | 引导、`Mod_OnTick`、反射读取 | ✓ 已 attach 域 |
| **鼠标钩子 pump 线程**（我们创建） | 拥有 `WH_MOUSE_LL`，只做消息泵 + 累加滚轮 | ✗ |
| **游戏渲染线程** | Present 钩子、ImGui 绘制、`Mod_OnFrame` | ✗ |
| **游戏逻辑线程** | `class_from_name` 钩子 → `Mod_OnGameThread` | ✓ 已 attach 域 |
| **游戏退出清理线程**（我们创建） | 点 X / 注销时抢跑清理（`src/exit_guard.cpp`） | ✗ |

前三条是我们自己的，后两条是游戏的，最后一条只在游戏退出时短暂存在。**关键约束：IL2CPP 的 `runtime_invoke` 只能在已 attach 域的线程上调用**，而 ImGui 按钮回调跑在渲染线程 —— 所以在按钮里直接调游戏方法会随机崩。

框架给出的解法是**请求队列**：

```
渲染线程                        游戏逻辑线程
─────────                      ─────────────
按钮回调                        Mod_OnGameThread()
  └─ PostRequest(kind, text)      └─ ConsumeRequest()
       └─ 写 volatile 请求槽  ──▶      └─ 读槽 → 真正执行
```

实现见 `src/user_hooks.cpp`。请求槽用 `volatile` + `InterlockedXxx`，不用 `std::mutex`（会引入静态 TLS）。

### 为什么 `class_from_name` 是「游戏逻辑线程入口」

IL2CPP 的类型解析函数只在 Unity 脚本线程上被调用，天然满足「已 attach 域」。挂个钩子在上面，就等于拿到了一个**安全的游戏线程执行点**。代价是它极高频（每次类型查找都命中），所以 `Mod_OnGameThread()` 里只能做 O(1) 的事。

## 四、模块职责

### `src/main.cpp` — 引导

- **日志**：`LogRaw()` 格式化后**双写** —— 文件（事后可查）+ 无锁环形缓冲（面板实时看）。跑在渲染线程时做文件 IO 不合适，所以面板只读缓冲。
- **配置**：定长 C 缓冲解析 `mod.ini`。
- **三个探针**：
  - A：`il2cpp_class_from_name`（常驻）— 命中即证明 detour 链路可用，同时充当游戏线程入口
  - B：`il2cpp_string_new`（默认关）— 热点路径压测
  - C：按 `mod.ini` 配置挂钩任意游戏方法（4 参数通用透传）
- **工具 DLL**：双 DLL 架构的加载侧。

### `il2cpp_core/` — 跨平台反射

`il2cpp_api.cpp` 解析 IL2CPP 导出的函数表；`il2cpp_runtime.cpp` 提供高层封装（`Init` / `EnsureAttached` / `Image` / `FindClass` / `ClassField` / `Invoke`）；`il2cpp_hook.cpp` 提供函数体 detour（含 trampoline 池与 `RemoveAllDetours`）。

定义了 `IL2CPP_CORE_NO_STL` 时，基于 `std::string`/`std::vector` 的函数会被剔除 —— 这正是我们用的模式。

### `src/ui/imgui_overlay.cpp` — 覆盖层

**取 device 的思路**（这是它最巧妙的地方，也是不依赖注入时机的原因）：

```
1. 用系统 d3d11.dll 的 D3D11CreateDeviceAndSwapChain 建一条 dummy swapchain
2. 取 dummy 链 vtable[8]（Present 槽）
   ★ 关键：D3D11 的 Present 实现在 dxgi.dll 里，是**全进程唯一**的一份代码
     不管是 dummy 链还是游戏自己的链，vtable[8] 指向同一个函数地址
3. 改**那张共享 vtable 的 [8] 槽** —— 全局生效，原函数代码一个字都没动
4. 钩子里 sc->GetDevice() 拿到游戏真实的 device
```

**为什么只改 vtable 槽、绝不改函数体**：`dxgi!Present` 的 prologue 是 `mov [rsp+0x08], rbx; mov [rsp+0x10], rsi; push rbp; ...`。函数体钩子末尾的 `return g_origPresent(...)` 编译成 CALL 会把 rsp 压低 8，trampoline 里克隆的 `mov [rsp+0x08], rbx` 就**正好覆盖 CALL 压进去的返回地址** → `ret` 跳到随机地址。改 vtable 槽则 `g_origPresent` 指向完全没被动过的函数，栈天然正确。

其余要点：

- `ResizeBuffers` 同样走 vtable patch
- 渲染前显式把 back buffer 绑成 RTV（游戏主画面阶段会换成中间渲染目标，不修的话 overlay 只在 logo 页可见），画完恢复原绑定
- 每 600 帧校验 vtable[8] 是否仍指向我们
- **不编 `imgui_impl_win32`**：它有函数内 static（`__cxa_guard_*`），会拖进 libstdc++ EH → emutls。输入由本文件用 `GetAsyncKeyState` / `GetCursorPos` 轮询喂给 imgui
- 中文字体：`C:\Windows\Fonts\msyh.ttc` → `simhei.ttf` → `simsun.ttc`，都找不到就退回默认字体（只能显示 ASCII）
- 字形范围默认取 **完整 CJK 表意文字**（`MOD_FONT_FULL_CJK`，见 `src/mod_config.h`）。
  实测代价：21791 字形 → 图集 4096×4096 → RGBA 64MB、构建约 190ms（一次性，发生在第一次
  Present，通常在 logo/加载画面上）。改成 0 则退回「常用 2500 字 + 符号区」，
  3299 字形 / 1024×2048 / 8MB / 30ms，**但 渲、帧、逻、辑 这类字会变 `?`**。
- 键盘轮询用 `GetAsyncKeyState` 的 **高位** 逐键取状态、自己拼 `ToUnicode` 的 key-state 数组；
  **不能用 `GetKeyboardState`**（它读的是调用线程的输入队列，渲染线程上拿不到 Shift，见
  [docs/pitfalls.md](pitfalls.md#输入：present-钩子里不能用依赖线程输入队列的-api)）

### `src/log_buffer.cpp` — 无锁日志环形缓冲

`Log()` 会被 Worker 线程和 Present 钩子（游戏渲染线程）同时调用，必须无锁。`std::mutex` 会引入静态 TLS，所以用**纯原子索引 + 2 的幂次槽**：

- 写入方：原子取下一个写索引，覆盖最旧的槽（环形覆盖，不阻塞）
- 读取方（渲染线程）：扫描全部槽，只取 `seq > lastSeen` 的行
- 每槽用原子 flag 标记「正在写」，读者跳过半行

### `src/hot_reload.cpp` — 单实例与卸载

**握手**：名字带 PID 的命名互斥体 `Local\il2cpp_mod_lock_<PID>`。

> 踩过的坑：最初的名字**不带 PID**（`Local\esp_il2cpp_test_singleton`）。`Local\` 是**会话级**命名空间，同一登录会话下所有进程共享 —— 于是一个进程里的守卫会误伤另一个进程的正常注入。互斥体本身没错，错在名字的作用域。

镜像基址不通过共享内存传（`CreateFileMapping(INVALID_HANDLE_VALUE)` 在目标进程里返回 err=1455），由注入器扫内存找 `ModUnload` 导出名反推。

**卸载顺序**（逆序，一步都不能错）：

```
1. ShutdownWorkerThread()     停 Worker（否则它会在我们还原 vtable 后继续调失效钩子）
2. il2cpp::hook::RemoveAllDetours()   把跳板字节写回 IL2CPP 模块
3. Mod_OnShutdown()           用户清理（★ 早于镜像释放，使用方起的线程跑在镜像里）
4. UiShutdown()               置标志 → 等渲染线程还原 vtable / DestroyContext
5. UnloadToolDll()            ★ 必须在 4 之后，此时工具 DLL 代码不在调用栈上
6. ReleaseLock()
```

第 2 步是关键：跳板写在 IL2CPP 模块的代码页里，不在我们镜像内。不还原的话，下次 `InstallDetour` 会把已改写的字节当正常指令解析 → 解析出的指令数变少 → 装出残缺钩子**且返回 true 看起来成功**。

第 5 步也容易漏：工具 DLL 是 `LoadLibraryW` 加载的，不 `FreeLibrary` 的话下次注入同一路径会直接返回旧句柄，新编的 DLL 内容不生效，热重载等于白改。

### `src/exit_guard.cpp` — 游戏主动退出的守护

上面那套卸载顺序只在**注入器主动卸载**时执行。但用户最常见的动作是**点 X 关游戏**，那时我们
一个字都收不到（手动映射的镜像不在 PEB 模块列表里，`LdrShutdownProcess` 不会通知它）。

于是这个模块负责**检测游戏要退出了**，并把控制权抢回来（完整原理与"卡死"成因见
[docs/pitfalls.md](pitfalls.md#八进程退出)）：

```
① 子类化游戏窗口过程     捕获 WM_CLOSE / WM_DESTROY / WM_ENDSESSION / WM_NCDESTROY
   （在 BindToChain 里装 —— 那时正跑在游戏窗口线程上，子类化要求同线程）
② 看门狗线程            每 100ms 查 IsWindow(gameHwnd)，兜住"窗口被别的方式销毁"
                   ↓ 任一触发
③ 独立清理线程          UiExitFastPath()（卸全局鼠标钩子 + 还原 dxgi vtable）
                   →   ModUnload()（复用完整卸载顺序）
```

两条硬性约束：

- **清理必须在独立线程上做** —— 触发点在游戏消息线程，那是退出的关键路径。
- **`UiExitFastPath()` 只做"快且幂等"的两件事**，不等待渲染线程。`UiShutdown()` 那套
  "置标志 → 等渲染线程 3 秒"在退出场景下等不到（渲染线程可能已经不再 Present）。

## 五、双 DLL 架构（可选）

```
il2cpp_mod.dll   手动映射  ── 零 .tls、无 STL、无异常、无 RTTI
   │                        （硬约束的产物，写起来束手束脚）
   │ LoadLibraryW
   └─▶ il2cpp_tool.dll  系统加载器  ── 可自由用 STL / 异常 / TLS
                                      （分析工具代码几乎原样可用）
```

工具 DLL 需要导出 `Tool_Init(void *imguiCtx, void *logSink)`、`Tool_Draw()`、`Tool_Tick()`、`Tool_Shutdown()`。加载时机有两个约束：

- **必须在 `EnsureAttached()` 之后** —— 工具 DLL 的 Init 里通常有 `while (!il2cpp_is_vm_thread(nullptr)) Sleep(1000);`，它在**调用线程**上查「当前线程是不是 VM 线程」，Worker attach 之前查必然 false，工具 DLL 会无限 Sleep 卡死 Worker。
- **必须在 `ImGui::CreateContext()` 之后** —— 工具要 `SetCurrentContext`。overlay 初始化是异步的（Present 第一次被调用才建 context），所以加载侧轮询 `UiImGuiContext()`。

没有工具 DLL 就把 `mod_config.h` 的 `MOD_TOOL_DLL` 置空。
