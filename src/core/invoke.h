// invoke.h — 方法调用器（按类型装箱 + runtime_invoke）
// ============================================================================
// 这是修改器最锋利也最容易崩的一把刀：直接调游戏自己的方法。
//
// 三个必须守住的点（旧项目全是血泪）：
//   1. **值类型参数必须装箱**。runtime_invoke_convert_args 要的是 Il2CppObject**，
//      每个元素必须是一个 boxed 对象。直接把裸 int/结构体指针塞进去，运行时会
//      当对象解引用 ⇒ 当场 AV。
//   2. **泛型定义没有方法体**。MethodInfo::methodPointer 为 NULL 的那种是泛型定义
//      （例如 Load<T>(string)），调它必然崩。列表里标出来、禁止调用。
//   3. **异常要接住**。托管方法抛异常时 runtime_invoke 返回 NULL 并把异常放进 exc。
//      不接就会静默返回 null，让人以为"方法没返回值"。
// ============================================================================
#pragma once

#include "il2cpp/il2cpp_types.h"

#define ARK_INVOKE_MAX_PARAMS 10

struct ArkParamSlot
{
    int           ok;          // 1 = 这个参数有可用输入
    int           kind;        // ArkValKind
    int           isEnum;
    int           isString;
    int           unsupported; // 1 = 这种类型目前构造不出来（自建结构体等）
    int           useSel;      // 1 = 用"对象页当前选中实例"当这个参数（延迟到调用线程解析）
    char          typeName[96];
    char          name[64];
    char          text[128];   // 用户输入的文本
    Il2CppObject *obj;         // 对象参数（从对象列表里选）
    Il2CppClass  *klass;       // 参数类型对应的类（装箱用）
};

// 读参数签名（Worker）。返回参数个数（0..ARK_INVOKE_MAX_PARAMS）。
// cap 不够时截断并返回实际可写数量。
int ArkInvokePrepare(MethodInfo *m, ArkParamSlot *out, int cap);

// 执行调用。只能在已 attach 的线程调。
// 成功返回 true；out 写结果文本（含异常信息）。
bool ArkInvokeCall(MethodInfo *m, Il2CppObject *thisObj, ArkParamSlot *slots, int n,
                   char *out, int outCap);

// ---- 方法列表（供 UI 显示，Worker 枚举完存静态表）----
struct ArkMethodEntry
{
    MethodInfo *method;
    int         isStatic;
    int         isGenericDef;   // 1 = 泛型定义，无方法体，不能调
    int         paramCount;
    char        label[200];
};

// 枚举 klass 的方法（includeParents=1 连父类）。返回写入数量。
int          ArkInvokeListMethods(Il2CppClass *k, int includeParents, int maxOut);
int          ArkInvokeMethodCount();
ArkMethodEntry *ArkInvokeMethodAt(int i);
