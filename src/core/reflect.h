// reflect.h — 反射便利层（类索引 / 字段枚举 / 类型名 / 值读写）
// ============================================================================
// 这是"修改器能自己活下去"的地基：
//   · 游戏更新后偏移全变 → 靠类索引 + 字段枚举**运行时自己找**，不硬编码 dump.cs
//   · 新功能还没写 → 用类浏览器 + Watch 现场改字段先试出来
//
// ★ 铁律：一切偏移必须运行时反射拿。dump.cs 只当"名字/签名"参考。
//   硬编码旧偏移的结果是**静默写错字节**，比崩溃更难查。
// ============================================================================
#pragma once

#include "il2cpp/il2cpp_types.h"

struct ArkClassEntry
{
    const char  *ns;
    const char  *name;
    Il2CppClass *klass;
    Il2CppImage *image;
};

// 建立类索引（枚举全部镜像）。只在 Worker 线程调一次。
// 返回索引到的类数量。超过 MOD_CLASS_INDEX_MAX 会被截断并记日志。
int ArkReflectBuildIndex();

// 重建索引（内部双缓冲，建完整体换代）。供「重建索引」按钮在 Worker 上调用。
// 失败（0 个类）时保留旧表不动。返回当前表里的类数量。
int ArkReflectRebuildIndex();

// 换代计数：每次索引替换 +1。UI 拿它判断"表换过了，要重扫搜索缓存"。
int ArkReflectIndexGen();

int  ArkReflectClassCount();
const ArkClassEntry *ArkReflectClassAt(int i);

// 子串模糊搜索（不区分大小写）。命中结果的**索引**写进 outIdx。
// 返回实际写入数量（可能小于命中总数，看 maxOut）。
int ArkReflectSearch(const char *needle, int *outIdx, int maxOut);

// 按 "Ns.Name" 或 "Name" 在索引里找。找不到返回 nullptr。
const ArkClassEntry *ArkReflectFindClass(const char *fullOrShort);

// ---- 字段 ----
// 遍历字段（**含父类链**）。il2cpp_class_get_fields 只返回本类字段，
// 想拿基类的必须自己往上走 —— 这是最常踩的坑之一。
// 返回遍历到的字段数。cb 为空的字段（name 为空）会被跳过。
typedef void (*ArkFieldVisitor)(FieldInfo *f, Il2CppClass *owner, int depth, void *ud);
int ArkReflectEnumFields(Il2CppClass *klass, ArkFieldVisitor cb, void *ud);

// 沿父类链精确找字段（strcmp，**不要**用子串匹配 —— "_attack" 会命中 "_attackTrigger"）。
FieldInfo *ArkReflectFindField(Il2CppClass *klass, const char *name, Il2CppClass **ownerOut);

bool ArkReflectIsStaticField(FieldInfo *f);

// ---- 类型描述 ----
// 把类型写成可读字符串："System.Int32" / "Torappu.Battle.Character[]" / "System.Single"
void ArkReflectTypeName(char *out, int cap, Il2CppType *t);
// 类给人看的名字：泛型实例会带上实参（List`1<Xxx>），其余是 "命名空间.类名"。
void ArkReflectClassDisplayName(char *out, int cap, Il2CppClass *k);
// 是不是"泛型实例"（List<int> / KeyValuePair<K,V> 这种实参定死的类）。
// 泛型实例上的方法拿到的是**泛型定义/未膨胀**的 MethodInfo，
// methodPointer 不保证是真代码 ⇒ 绝不能直接调、也不能拿去 runtime_invoke。
bool ArkReflectIsGenericInstance(Il2CppClass *k);

// 判断类型的"值种类"（用于决定 UI 用什么控件）
enum ArkValKind
{
    VK_UNKNOWN = 0,
    VK_BOOL,
    VK_INT,        // i1/i2/i4/u1/u2/u4
    VK_LONG,       // i8/u8
    VK_FLOAT,
    VK_DOUBLE,
    VK_STRING,
    VK_OBJECT,     // 引用类型（class / array / genericinst）
    VK_STRUCT,     // 值类型（enum / valuetype）
};
ArkValKind ArkReflectKindOf(Il2CppType *t);

// 值类型/枚举的实际大小（字节）。取不到返回 0。
int ArkReflectSizeOf(Il2CppType *t);

// ---- 值读写 ----
// 读一个字段并填成字符串预览。instance 为空时读静态字段。
// 返回 0 = 成功，负 = 失败原因（-1 判空 / -2 类型不支持 / -3 取不到值）
int ArkReflectReadFieldText(FieldInfo *f, Il2CppObject *instance, char *out, int cap);

// 从字符串写入字段（只支持数值类）。成功返回 true。
bool ArkReflectWriteFieldText(FieldInfo *f, Il2CppObject *instance, const char *text);

// 直接按数值写入（UI 的拖动条走这条，避免字符串来回转换的精度损失）
bool ArkReflectWriteFieldNum(FieldInfo *f, Il2CppObject *instance, double v);

// 读数值（供 UI 拖动条初始化）。失败返回 false。
bool ArkReflectReadFieldNum(FieldInfo *f, Il2CppObject *instance, double *out);

// 字段地址（仅实例字段且非值类型装箱场景下有直接意义）。仅供高级诊断用。
void *ArkReflectFieldAddr(FieldInfo *f, Il2CppObject *instance);
