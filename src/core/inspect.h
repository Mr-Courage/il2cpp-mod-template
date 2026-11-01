// inspect.h — 实例字段树（对象检查器）
// ============================================================================
// 把"一个活对象"展平成一串**可渲染的节点**，UI 只负责画。
//
// 为什么先展平再画：
//   · 渲染线程绝不能调 il2cpp（碰托管堆 = 随机崩）。所以整棵树在 Worker 上一次性
//     建好，UI 只读 POD 数组。
//   · 树形递归画在 ImGui 里要维护开合状态，而开合状态一变就得重新读值；
//     展平后"重建一次 = 数组换一代"，UI 用代数号判断要不要重画。
//
// ★ 环路：对象图里几乎一定有环（parent ↔ child、manager ↔ singleton）。
//   所以必须同时有深度上限、节点上限、以及**已访问指针集合**，三者缺一不可。
//   只靠深度上限的话，广而深的图会把节点数打爆，UI 直接卡死。
// ============================================================================
#pragma once

#include "il2cpp/il2cpp_types.h"

enum ArkInspNodeKind
{
    IKN_FIELD = 0,   // 普通字段（含静态）
    IKN_ELEM  = 1,   // 数组元素
    IKN_ITEM  = 2,   // List<T> 元素
};

// 下钻层数上限。一层容器要吃掉两层（容器头 + 元素），所以"看进两层嵌套"
// 实际要 8 层左右。UI 的滑块和 ArkInspBuild 的夹取都用这一个常量。
#define ARK_INSP_DEPTH_MAX 20

struct ArkInspNode
{
    int           depth;
    int           kind;
    FieldInfo    *field;      // IKN_FIELD 才有
    Il2CppClass  *owner;      // 声明该字段的类
    Il2CppObject *obj;        // 该字段所属实例（静态字段为 nullptr）
    Il2CppObject *child;      // 引用类字段的值（要下钻就是它）
    int           isStatic;
    int           expandable; // 1 = 有内容可以下钻
    int           expanded;   // 1 = 当前展开
    int           valKind;    // ArkValKind
    int           editable;   // 1 = 标量，可以改
    int           index;      // 数组/List 下标
    double        num;        // 数值（可编辑时用；不可编辑为 0）
    char          name[80];
    char          type[96];
    char          value[240];
};

// 清空（换对象时调）
void ArkInspReset();

// 重建整棵树。maxDepth 为下钻层数上限（1~8），maxItems 为数组/List 每层最多展示几个。
// 只在已 attach 的 Worker / 游戏线程调用。
// 返回节点数。
int ArkInspBuild(Il2CppObject *root, Il2CppClass *rootClass, int maxDepth, int maxItems);

int            ArkInspCount();
ArkInspNode   *ArkInspAt(int i);

// 展开/收起一个节点（按 obj + field + index 记，跨重建有效）。
void ArkInspToggle(const ArkInspNode *n);

// 改一个标量字段。返回 true = 已写入。
bool ArkInspWriteNum(const ArkInspNode *n, double v);

// 把任意对象写成一行摘要（"0x.. Torappu.Battle.Map"），供对象列表用。
void ArkInspectSummary(Il2CppObject *obj, char *out, int cap);
