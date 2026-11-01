// objects.cpp — 对象查找 + 实例字段检查器
// ============================================================================
// 这一页把修改器从"看类型"变成"看实例"：
//   选类 → 枚举场上所有活实例 → 选中一个 → 展开它的字段 → 直接改数值。
//
// 线程分工（★ 本文件所有复杂度都在这里）：
//   渲染线程 ArkPanelObjects()   只画静态快照 + 投递请求，绝不调 il2cpp
//   Worker   EnumTask            调 liveness 枚举（会停 GC 世界，只能在这跑）
//   Worker   TreeTask            遍历对象图读字段
//   Worker   WriteTask           把 UI 收集的改动真正写下去
//
// ★ 枚举出来的对象**当场就钉住**（gchandle），不等到 UI 选中再钉：
//   中间隔着"下一帧 UI 才读到列表"，那时候可能已经跑过一次 GC。
// ============================================================================
#include "panels/panels.h"

#include "mod_config.h"
#include "core/gcobj.h"
#include "core/guard.h"
#include "core/inspect.h"
#include "core/log.h"
#include "core/reflect.h"
#include "core/taskbus.h"

#include "il2cpp/il2cpp_runtime.h"

#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ir = il2cpp::runtime;

#define ARK_OBJ_MAX 400

// ================================================================ 状态
// 实例表：句柄（已钉住）。Worker 写、渲染线程读，所以只走一次发布。
static unsigned      g_inst[ARK_OBJ_MAX];
static char          g_instNote[ARK_OBJ_MAX][48];   // "静态字段名" 之类的来源说明
static volatile LONG g_instCount = 0;
static volatile LONG g_enumBusy  = 0;
static volatile LONG g_enumErr   = 0;

static Il2CppClass *volatile g_selClass = nullptr;
static char          g_selClassName[192] = { 0 };

// ★ 选中对象要**两个**句柄，别合并成一个：
//   g_thisSel  = 实例列表里的那个句柄，只用来高亮"选中了哪一行"
//   g_thisSelKeep = 独立的一个钉子，专门保护"正在检查的这棵树"
//   为什么分开：重新枚举时列表里所有句柄会被统一释放（否则每枚举一次漏一屏），
//   而正在检查的对象必须活过这一刻。没有独立钉子，g_thisSel 会变成**已释放的句柄号**，
//   之后 ArkGcTarget 读到的可能是被复用的槽位（别的对象）或干脆是空。
static unsigned      g_thisSel = 0;        // 列表句柄（仅用于高亮）
static unsigned      g_thisSelKeep = 0;    // 独立钉子（树的根，跨枚举存活）
static char          g_thisName[192] = { 0 };

// 默认 4：字段(0) → List 头(1) → 元素(2) → 结构体字段(3) → 结构体里的容器(4)。
// 3 的话刚好卡在"能看到 key/value，但点不进 value 那个 List"的边界上。
static int           g_depth = 4;
static int           g_items = 12;
static volatile LONG g_treeGen  = 0;
static volatile LONG g_treeBusy = 0;

static char          g_filter[96] = { 0 };
static int           g_hits[400];
static int           g_hitCount = 0;
static int           g_hitGen = -1;

// 编辑弹窗的暂存（点击时拷贝一份节点，避免重建后索引错位）
static ArkInspNode   g_editNode;
static int           g_editOpen = 0;
static char          g_editText[64] = { 0 };

// ================================================================ 对外共享
Il2CppClass *ArkSelClass()     { return g_selClass; }
const char  *ArkSelClassName() { return g_selClassName; }
unsigned int ArkSelObject()    { return g_thisSelKeep; }

void ArkSelSetClass(Il2CppClass *k)
{
    if (g_selClass == k) return;
    g_selClass = k;
    g_selClassName[0] = 0;
    if (k)
    {
        Il2CppApi &api = ir::Api();
        const char *ns = api.il2cpp_class_get_namespace ? api.il2cpp_class_get_namespace(k) : "";
        const char *nm = api.il2cpp_class_get_name ? api.il2cpp_class_get_name(k) : "?";
        if (ns && ns[0])
            _snprintf_s(g_selClassName, sizeof(g_selClassName), _TRUNCATE, "%s.%s", ns, nm);
        else
            _snprintf_s(g_selClassName, sizeof(g_selClassName), _TRUNCATE, "%s", nm);
    }
    InterlockedExchange(&g_instCount, 0);
}

// ================================================================ 任务
static void EnumTask(void *ud)
{
    Il2CppClass *k = (Il2CppClass *)ud;
    ArkGuardSetAction("obj.enum");

    // 先放掉上一批句柄，否则每枚举一次泄漏一屏 gchandle
    const int old = (int)InterlockedCompareExchange(&g_instCount, 0, 0);
    for (int i = 0; i < old; ++i)
        ArkGcUnpin(g_inst[i]);
    InterlockedExchange(&g_instCount, 0);

    if (!k)
    {
        InterlockedExchange(&g_enumErr, 5);
        InterlockedExchange(&g_enumBusy, 0);
        return;
    }

    // 函数内 static 的 POD 数组：零初始化即合法，不涉及静态构造器
    static Il2CppObject *raw[ARK_OBJ_MAX];
    const int got = ArkGcEnumObjects(k, raw, ARK_OBJ_MAX, 1);

    int w = 0;
    if (got > 0)
    {
        for (int i = 0; i < got && w < ARK_OBJ_MAX; ++i)
        {
            if (!raw[i]) continue;
            const unsigned h = ArkGcPin(raw[i]);
            if (h) { g_instNote[w][0] = 0; g_inst[w++] = h; }
        }
    }
    InterlockedExchange(&g_enumErr, got < 0 ? -got : 0);
    InterlockedExchange(&g_instCount, w);
    InterlockedExchange(&g_enumBusy, 0);

    // 列表句柄全换了 ⇒ 旧的"高亮句柄"已经失效，必须重新认领。
    // 正在检查的对象本身还活着（g_thisSelKeep 钉着），在新列表里按指针找回来即可。
    g_thisSel = 0;
    Il2CppObject *keepObj = ArkGcTarget(g_thisSelKeep);
    if (keepObj)
    {
        for (int i = 0; i < w; ++i)
        {
            if (ArkGcTarget(g_inst[i]) == keepObj) { g_thisSel = g_inst[i]; break; }
        }
    }

    Log("[obj] 枚举 %s：%d 个实例", g_selClassName, w);
}

void ArkObjRequestEnum();   // 前置声明，定义在下面

// 兜底通道：不靠 liveness，直接翻这个类的**静态字段**找实例。
// 游戏里绝大多数管理器是"静态自引用字段"（singleton），这条路的命中率不低，
// 而且完全不需要停 GC 世界 —— liveness 导出缺失时，它是唯一还能用的入口。
static void ScanStaticsTask(void *ud)
{
    Il2CppClass *k = (Il2CppClass *)ud;
    ArkGuardSetAction("obj.statics");

    const int old = (int)InterlockedCompareExchange(&g_instCount, 0, 0);
    for (int i = 0; i < old; ++i)
        ArkGcUnpin(g_inst[i]);
    InterlockedExchange(&g_instCount, 0);

    if (!k) { InterlockedExchange(&g_enumBusy, 0); return; }

    Il2CppApi &api = ir::Api();
    int w = 0;
    int hits = 0;

    Il2CppClass *cur = k;
    int guard = 0;
    while (cur && guard++ < 12 && w < ARK_OBJ_MAX)
    {
        void *iter = nullptr;
        FieldInfo *f = nullptr;
        while (api.il2cpp_class_get_fields && (f = api.il2cpp_class_get_fields(cur, &iter)) != nullptr)
        {
            if (!ArkReflectIsStaticField(f)) continue;
            Il2CppType *t = api.il2cpp_field_get_type(f);
            if (!t) continue;
            const ArkValKind kind = ArkReflectKindOf(t);
            if (kind != VK_OBJECT) continue;

            Il2CppObject *v = api.il2cpp_field_get_value_object ? api.il2cpp_field_get_value_object(f, nullptr) : nullptr;
            if (!v) continue;

            Il2CppClass *vk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(v) : v->klass;
            bool match = (vk == k);
            if (!match && api.il2cpp_class_is_assignable_from && vk)
                match = api.il2cpp_class_is_assignable_from(k, vk);
            if (!match) continue;

            const unsigned h = ArkGcPin(v);
            if (!h) continue;

            g_inst[w] = h;
            const char *fn = api.il2cpp_field_get_name ? api.il2cpp_field_get_name(f) : "?";
            _snprintf_s(g_instNote[w], sizeof(g_instNote[w]), _TRUNCATE, "%s", fn ? fn : "?");
            ++w;
            ++hits;
            if (w >= ARK_OBJ_MAX) break;
        }
        cur = api.il2cpp_class_get_parent ? api.il2cpp_class_get_parent(cur) : nullptr;
    }

    InterlockedExchange(&g_enumErr, 0);
    InterlockedExchange(&g_instCount, w);
    InterlockedExchange(&g_enumBusy, 0);
    Log("[obj] 静态字段扫描 %s：%d 个实例", g_selClassName, hits);
}

static void RequestScanStatics()
{
    if (InterlockedCompareExchange(&g_enumBusy, 1, 0) != 0) return;
    if (!ArkTaskPost(ScanStaticsTask, (void *)g_selClass, "obj.statics"))
        InterlockedExchange(&g_enumBusy, 0);
}

static void RequestEnum()
{
    if (InterlockedCompareExchange(&g_enumBusy, 1, 0) != 0)
        return;
    if (!ArkTaskPost(EnumTask, (void *)g_selClass, "obj.enum"))
    {
        InterlockedExchange(&g_enumBusy, 0);
        Log("[obj] 任务队列满，枚举推迟");
    }
}

void ArkObjRequestEnum()
{
    RequestEnum();
}

static void TreeTask(void *ud)
{
    (void)ud;
    ArkGuardSetAction("obj.tree");
    Il2CppObject *root = ArkGcTarget(g_thisSelKeep);
    if (root)
        ArkInspBuild(root, nullptr, g_depth, g_items);
    else
        ArkInspReset();
    InterlockedIncrement(&g_treeGen);
    InterlockedExchange(&g_treeBusy, 0);
}

// 选中实例：由 Worker 建一个**独立的钉子**，然后立刻建树。
// 为什么不在渲染线程直接 gchandle_new：那个 API 必须在已 attach 域的线程上调用。
static void SelectTask(void *ud)
{
    ArkGuardSetAction("obj.select");

    const unsigned listHandle = (unsigned)(uintptr_t)ud;
    Il2CppObject *target = ArkGcTarget(listHandle);

    if (g_thisSelKeep)
    {
        ArkGcUnpin(g_thisSelKeep);
        g_thisSelKeep = 0;
    }
    g_thisSel = listHandle;

    if (target)
    {
        g_thisSelKeep = ArkGcPin(target);
        ArkInspBuild(target, nullptr, g_depth, g_items);
    }
    else
    {
        ArkInspReset();
    }

    InterlockedIncrement(&g_treeGen);
    InterlockedExchange(&g_treeBusy, 0);
}

static void RequestSelect(unsigned listHandle)
{
    if (InterlockedCompareExchange(&g_treeBusy, 1, 0) != 0)
        return;
    if (!ArkTaskPost(SelectTask, (void *)(uintptr_t)listHandle, "obj.select"))
        InterlockedExchange(&g_treeBusy, 0);
}

static void RequestTree()
{
    if (InterlockedCompareExchange(&g_treeBusy, 1, 0) != 0)
        return;
    if (!ArkTaskPost(TreeTask, nullptr, "obj.tree"))
        InterlockedExchange(&g_treeBusy, 0);
}

struct WriteReq
{
    ArkInspNode node;
    double      v;
};

static void WriteTask(void *ud)
{
    WriteReq *r = (WriteReq *)ud;
    if (r)
    {
        ArkGuardSetAction("obj.write");
        if (ArkInspWriteNum(&r->node, r->v))
            Log("[obj] 写入 %s = %.6g（原值 %s）", r->node.name, r->v, r->node.value);
        else
            Log("[obj] 写入失败：%s（类型不支持或字段已失效）", r->node.name);
        free(r);
    }
    InterlockedExchange(&g_treeBusy, 0);
}

static void RequestWrite(const ArkInspNode *n, double v)
{
    if (InterlockedCompareExchange(&g_treeBusy, 1, 0) != 0)
        return;
    WriteReq *r = (WriteReq *)malloc(sizeof(WriteReq));
    if (!r) { InterlockedExchange(&g_treeBusy, 0); return; }
    memcpy(&r->node, n, sizeof(ArkInspNode));
    r->v = v;
    if (!ArkTaskPost(WriteTask, r, "obj.write"))
    {
        free(r);
        InterlockedExchange(&g_treeBusy, 0);
    }
}

// 把当前对象的所有顶层标量字段一次性丢进「字段监视」。
static void WatchTask(void *ud)
{
    (void)ud;
    ArkGuardSetAction("obj.watch");
    Il2CppApi &api = ir::Api();
    const int n = ArkInspCount();
    int added = 0;
    for (int i = 0; i < n; ++i)
    {
        ArkInspNode *nd = ArkInspAt(i);
        if (!nd || nd->depth != 0 || !nd->field || !nd->editable) continue;

        char cls[192] = { 0 };
        const char *ns = api.il2cpp_class_get_namespace ? api.il2cpp_class_get_namespace(nd->owner) : "";
        const char *nm = api.il2cpp_class_get_name ? api.il2cpp_class_get_name(nd->owner) : "?";
        _snprintf_s(cls, sizeof(cls), _TRUNCATE, (ns && ns[0]) ? "%s.%s" : "%s",
                    (ns && ns[0]) ? ns : "", nm);

        if (ArkWatchAdd(cls, nd->name, nd->obj, nd->isStatic != 0))
            ++added;
    }
    Log("[obj] 已加入监视 %d 个字段", added);
    InterlockedExchange(&g_treeBusy, 0);
}

static void RequestWatch()
{
    if (InterlockedCompareExchange(&g_treeBusy, 1, 0) != 0)
        return;
    if (!ArkTaskPost(WatchTask, nullptr, "obj.watch"))
        InterlockedExchange(&g_treeBusy, 0);
}

// ================================================================ UI：类选择
static void DrawClassPicker(float w)
{
    ImGui::BeginChild("##clsPicker", ImVec2(w, 0), ImGuiChildFlags_Border);

    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##f", "搜索类（名字或命名空间）", g_filter, sizeof(g_filter)))
        g_hitGen = -1;

    const int gen = ArkReflectIndexGen();
    if (g_hitGen != gen || (g_filter[0] && g_hitCount == 0))
    {
        g_hitGen = gen;
        if (g_filter[0])
            g_hitCount = ArkReflectSearch(g_filter, g_hits, (int)(sizeof(g_hits) / sizeof(g_hits[0])));
        else
            g_hitCount = 0;
    }

    ImGui::TextDisabled("共 %d 个类", ArkReflectClassCount());
    ImGui::Separator();

    ImGui::BeginChild("##clsList", ImVec2(0, 0));
    if (!g_filter[0])
    {
        ImGui::TextDisabled("输入关键字开始搜索。");
        ImGui::TextDisabled("常用：BattleController / Map / Character / Enemy");
    }
    for (int i = 0; i < g_hitCount; ++i)
    {
        const ArkClassEntry *e = ArkReflectClassAt(g_hits[i]);
        if (!e) continue;

        char label[256];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "%s%s###c%d",
                    (e->ns && e->ns[0]) ? e->ns : "", e->name, i);

        const bool sel = (g_selClass == e->klass);
        if (ImGui::Selectable(label, sel))
        {
            g_thisSel = 0;
            g_thisName[0] = 0;
            ArkSelSetClass(e->klass);
            ArkInspReset();
            RequestEnum();
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

// ================================================================ UI：实例列表
static void DrawInstanceList()
{
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s",
                       g_selClassName[0] ? g_selClassName : "（还没选类）");
    ImGui::SameLine();

    const bool busy = InterlockedCompareExchange(&g_enumBusy, 0, 0) != 0;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::SmallButton("枚举全部实例")) RequestEnum();
    ImGui::SameLine();
    if (ImGui::SmallButton("只找静态引用"))
        RequestScanStatics();
    if (busy) ImGui::EndDisabled();

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("不走 GC 活体枚举，只翻这个类（含父类）的静态字段。\n"
                          "liveness 导出缺失时的兜底通道；对单例类往往一条就命中。");

    ImGui::SameLine();
    ImGui::TextDisabled("%d 个实例", (int)InterlockedCompareExchange(&g_instCount, 0, 0));

    const int err = (int)InterlockedCompareExchange(&g_enumErr, 0, 0);
    if (err == 1) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "liveness 导出不可用"); }
    else if (err == 2) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "没有 IL2CPP 域"); }

    ImGui::BeginChild("##inst", ImVec2(0, 150), ImGuiChildFlags_Border);
    const int n = (int)InterlockedCompareExchange(&g_instCount, 0, 0);
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *obj = ArkGcTarget(g_inst[i]);
        char label[160];
        if (obj)
        {
            if (g_instNote[i][0])
                _snprintf_s(label, sizeof(label), _TRUNCATE, "#%d  %p  <%s>###i%d",
                            i, (void *)obj, g_instNote[i], i);
            else
                _snprintf_s(label, sizeof(label), _TRUNCATE, "#%d  %p###i%d", i, (void *)obj, i);
        }
        else
            _snprintf_s(label, sizeof(label), _TRUNCATE, "#%d  (已回收)###i%d", i, i);

        const bool sel = (g_thisSel == g_inst[i]);
        if (ImGui::Selectable(label, sel))
        {
            _snprintf_s(g_thisName, sizeof(g_thisName), _TRUNCATE, "%s", label);
            RequestSelect(g_inst[i]);
        }
    }
    if (n == 0)
        ImGui::TextDisabled("（空。没选类，或这个类当前没有活实例）");
    ImGui::EndChild();
}

// ================================================================ UI：字段树
static void DrawNodeRow(int i, ArkInspNode *n)
{
    ImGui::PushID(i);

    // 缩进 + 展开箭头
    if (n->depth > 0)
        ImGui::Dummy(ImVec2(n->depth * 11.0f, 0));
    else
        ImGui::Dummy(ImVec2(0, 0));
    ImGui::SameLine(0, 0);

    if (n->expandable)
    {
        if (ImGui::SmallButton(n->expanded ? "v" : ">"))
        {
            ArkInspToggle(n);
            RequestTree();
        }
    }
    else
    {
        ImGui::Dummy(ImVec2(ImGui::GetFrameHeight() * 0.6f, 0));
    }
    ImGui::SameLine(0, 4);

    // 名字：按值种类上色，扫一眼就能分辨数字/对象/字符串
    ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
    switch (n->valKind)
    {
    case VK_INT: case VK_LONG: col = ImVec4(0.65f, 0.85f, 1.0f, 1.0f); break;
    case VK_FLOAT: case VK_DOUBLE: col = ImVec4(0.55f, 0.95f, 0.75f, 1.0f); break;
    case VK_BOOL: col = ImVec4(0.95f, 0.85f, 0.45f, 1.0f); break;
    case VK_STRING: col = ImVec4(0.95f, 0.78f, 0.55f, 1.0f); break;
    case VK_OBJECT: col = ImVec4(0.75f, 0.75f, 0.95f, 1.0f); break;
    default: break;
    }
    ImGui::TextColored(col, "%s%s", n->name, n->isStatic ? " (static)" : "");

    ImGui::SameLine();
    ImGui::TextDisabled("  %s", n->type);

    ImGui::SameLine();
    ImGui::TextUnformatted("=");
    ImGui::SameLine();

    if (n->editable)
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.7f, 1.0f), "%s", n->value);
        ImGui::SameLine();
        if (ImGui::SmallButton("改"))
        {
            g_editNode = *n;
            _snprintf_s(g_editText, sizeof(g_editText), _TRUNCATE, "%.10g", n->num);
            g_editOpen = 1;
        }
    }
    else
    {
        ImGui::TextUnformatted(n->value);
    }

    ImGui::PopID();
}

static void DrawTree()
{
    ImGui::Separator();
    ImGui::Text("对象：%s", g_thisName[0] ? g_thisName : "（在下面列表里选一个实例）");
    ImGui::SameLine();

    const bool busy = InterlockedCompareExchange(&g_treeBusy, 0, 0) != 0;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::SmallButton("刷新")) RequestTree();
    if (busy) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::SmallButton("监视全部标量")) RequestWatch();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    // 上限吃 ARK_INSP_DEPTH_MAX：一层容器要吃掉两层（容器头 + 元素），
    // 8 层只够看进一层嵌套，20 层才能一路下到纯数值。
    if (ImGui::SliderInt("##depth", &g_depth, 1, ARK_INSP_DEPTH_MAX, "深度 %d"))
        RequestTree();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::SliderInt("##items", &g_items, 1, 512, "条目 %d"))
        RequestTree();

    // 横向滚动：深度 20 时缩进吃掉 220px，长类型名会把行推出可视区
    ImGui::BeginChild("##tree", ImVec2(0, 0), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const int n = ArkInspCount();
    if (n == 0)
        ImGui::TextDisabled("（没有内容。选一个实例，或点「刷新」）");

    // ★ 逐行画，每行都重新取指针：Worker 可能正在重建（固定缓冲，不会崩），
    //   这样最多看到一帧半新半旧。
    for (int i = 0; i < n; ++i)
    {
        ArkInspNode *nd = ArkInspAt(i);
        if (!nd) break;
        DrawNodeRow(i, nd);
    }
    ImGui::EndChild();
}

// ================================================================ 入口
void ArkPanelObjects()
{
    DrawClassPicker(280.0f);
    ImGui::SameLine();

    ImGui::BeginChild("##objRight", ImVec2(0, 0));
    DrawInstanceList();
    DrawTree();
    ImGui::EndChild();

    // ---- 编辑弹窗（放在最后，不占布局）----
    if (g_editOpen)
    {
        ImGui::OpenPopup("##editValue");
        g_editOpen = 0;
    }
    if (ImGui::BeginPopupModal("##editValue", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s  (%s)", g_editNode.name, g_editNode.type);
        ImGui::TextDisabled("当前：%s", g_editNode.value);
        ImGui::SetNextItemWidth(220);
        const bool enter = ImGui::InputText("##v", g_editText, sizeof(g_editText),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter || ImGui::Button("写入", ImVec2(100, 0)))
        {
            const double v = strtod(g_editText, nullptr);
            RequestWrite(&g_editNode, v);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
