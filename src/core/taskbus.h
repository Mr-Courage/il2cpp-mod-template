// taskbus.h — 跨线程任务总线（渲染线程 → 游戏逻辑线程）
// ============================================================================
// 为什么必须有这一层：
//   UI 按钮回调跑在**渲染线程**（Present 钩子里）。而创建 Unity 对象、调游戏方法、
//   写场景 Transform 只能在**游戏逻辑线程**（已 attach 域的线程）做。
//   在渲染线程直接调 = 随机崩溃，而且崩点常常离调用点很远（GC/寻路/下一帧才炸）。
//
// 用法：
//   渲染线程：  ArkTaskPost(MyDoThing, myArg, "godmode.apply");
//   游戏线程：  ArkTaskPump();   // 框架已经在 class_from_name 钩子里每帧调
//
// 约束：
//   · fn 里可以调 il2cpp，但**不要**做长耗时操作（它卡的是游戏逻辑帧）
//   · ud 指向的内存必须活到任务执行完（用静态槽或堆分配 + fn 自己释放）
//   · 队列满时 Post 返回 false，调用方要处理（UI 上会红字提示）
// ============================================================================
#pragma once

typedef void (*ArkTaskFn)(void *ud);

void ArkTaskBusInit();

// 返回 false = 队列满（丢弃）。tag 只用于日志归因，可为空。
bool ArkTaskPost(ArkTaskFn fn, void *ud, const char *tag);

// 消费队列。只能在**游戏逻辑线程**调（或 Worker 线程，它也已 attach）。
// 每个任务用 SEH 包裹：单个任务炸了不会带走整个游戏，但会被记下来。
void ArkTaskPump();

int  ArkTaskPending();          // 排队中
long ArkTaskExecuted();         // 累计成功
long ArkTaskFailed();           // 累计异常

// ============================================================================
// ★ 主线程队列（Unity 主线程专用）
// ============================================================================
// 为什么必须再有这一层：
//   ArkTaskPump 的调用点包括 Worker 线程（main.cpp 主循环每 100ms 泵一次）——
//   于是「游戏线程任务」实际上有一条腿跑在 **Worker 线程**上。
//   纯内存读写（反射、读字段、算数值）在 Worker 上没问题，但**创建 GameObject /
//   Instantiate / 走 Unity 对象池**是主线程专属：Spine 的 SmartMesh 构造器要拿
//   `GraphicsDevice.instance`，在非主线程拿到 null ⇒ 皮肤建不出来 ⇒ 每帧
//   LateUpdate 抛 NRE ⇒ 几秒后整个进程被拖死（实测就是这个死法）。
//
// 语义：这里投递的任务**只有在 Unity 主线程**才会被执行 —— 其它线程泵不到它。
//   主线程入口有两条（任取其一都够，两条都装着更稳）：
//     · BattleController.Update 的 detour（战斗中每帧，最可靠）
//     · class_from_name detour 里判定「当前就是主线程」时才泵（战斗外兜底）
// 代价：战斗外没有 Update，队列会一直躺着 —— 这是**正确**行为，别在 Worker 里绕过去。
bool ArkTaskPostMain(ArkTaskFn fn, void *ud, const char *tag);
void ArkMainTaskPump();          // 只能在 Unity 主线程调
int  ArkMainTaskPending();
long ArkMainTaskExecuted();

// 主线程 id。UiGameHwnd 的所属线程就是 Unity 主线程（窗口在主线程创建），
// 与「渲染线程」不一定同一个（多线程渲染下 Present 在 render thread）。
void          ArkTaskBusSetMainThread(unsigned long tid);
unsigned long ArkTaskBusMainThread();
int           ArkTaskBusIsMainThread();
