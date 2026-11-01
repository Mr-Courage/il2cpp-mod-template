// gcobj.h — 「类 → 活实例」：GC 活体枚举 + 钉住
// ============================================================================
// 为什么这是分水岭：
//   类浏览器只能看到**类型**，改不了**实例**。真正好用的修改器必须能问出
//   "现在场上有几个 BattleController / 它们的地址是多少"，
//   然后把字段挂到那个对象上去读写。这一层补的就是这个能力。
//
// 官方途径只有一条：Unity 的 liveness 计算（il2cpp_unity_liveness_*）。
//   原理 = 停住 GC 世界 → 从静态根出发标记存活对象 → 把匹配 filter 的报出来。
//   所以：**必须停世界**。这意味着它绝对不能跑在渲染线程（Present 钩子里），
//   否则游戏逻辑线程同时在分配/移动对象，标记结果不可信，且容易把引擎卡死。
//   统一走 ArkTaskPost / Worker。
//
// ★ 裸指针不是 GC 根（旧项目踩过的血泪）：
//   枚举出来就存起来、中间跨了一次 GC ⇒ 对象被回收，你的指针指向已被
//   复用的内存。读它不会立刻崩，只会给出"垃圾但看起来合法"的值，
//   然后把错的数据写回游戏。要长期持有就必须 ArkGcPin。
// ============================================================================
#pragma once

#include "il2cpp/il2cpp_types.h"

// liveness 导出是否齐全（不全说明 Unity 版本对不上，对象查找整体不可用）。
// 结果缓存，首次调用时探测并记一行日志。
bool ArkGcAvailable();

// 枚举 klass 的活实例（superClasses=1 时连同子类一起）。
// 只在**已 attach 域的 Worker / 游戏线程**调用。
// 返回写入 out 的数量（0..maxCount）；失败返回负：
//   -1 = 导出不全  -2 = 没有域  -3 = out 为空
int ArkGcEnumObjects(Il2CppClass *klass, Il2CppObject **out, int maxCount, int superClasses);

// ---- 钉住 ----
// 返回 0 = 失败。0 号句柄在 il2cpp 里是合法的，所以这里用 0 表示失败、
// 内部把真实句柄 +1 存（见 gcobj.cpp）。
unsigned int  ArkGcPin(Il2CppObject *obj);
void          ArkGcUnpin(unsigned int h);
Il2CppObject *ArkGcTarget(unsigned int h);
