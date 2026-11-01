// panels.h — 各 tab 的绘制入口
// ============================================================================
// 全部跑在**渲染线程**。这里有一条硬规则：
//   面板只负责"画"和"收集请求"；任何 il2cpp 写操作都要 ArkTaskPost 到游戏线程，
//   连"读字段"都尽量在 Worker 侧刷成缓存再在这里显示 —— 渲染线程碰托管堆是雷区。
//
// 这条规则的成本是"点一下按钮，下一帧才看到结果"，收益是**不崩**。
// ============================================================================
#pragma once

struct Il2CppObject;
struct Il2CppClass;

// ---- 工具 tab ----
void ArkPanelClassBrowser();     // 类浏览器
void ArkPanelWatch();            // 字段监视/编辑
void ArkPanelHotkeys();          // 热键设置
void ArkPanelObjects();          // 对象查找 + 实例字段检查器
void ArkPanelInvoke();           // 方法调用器
void ArkPanelDumper();           // 运行时 Il2CppDumper 式导出

// ---- 对象页 / 调用页共享的"当前选中" ----
// 调用页的类与 this 都跟着对象页走，省得选两次。
Il2CppClass *ArkSelClass();
const char  *ArkSelClassName();
unsigned int ArkSelObject();     // gchandle；0 = 未选
void         ArkSelSetClass(Il2CppClass *k);
void         ArkObjRequestEnum();   // 让对象页对当前类重跑一次枚举

// ---- Watch 的数据侧（Worker 线程调用，不在 UI 里）----
void ArkWatchRefresh();          // 刷新所有 watch 的显示值
void ArkWatchApplyWrites();      // 应用待写入（游戏线程）
bool ArkWatchAdd(const char *classFullName, const char *fieldName,
                 Il2CppObject *instance, bool isStatic);
void ArkWatchClearAll();
int  ArkWatchCount();
