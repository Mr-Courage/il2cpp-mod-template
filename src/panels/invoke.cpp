// invoke.cpp — 方法调用器面板
// ============================================================================
// 用途：类浏览器只能看签名，这一页能**真的调**。做新功能之前先用它试：
//   比如 BattleCostManager.SetCost(99) 能不能生效、会不会立刻被覆盖，
//   在这里点一下就知道，不用改代码重新注入。
//
// 与对象页联动：调用页的"类"就是对象页选中的类；实例方法的 this
// 默认取对象页选中的那个实例（已钉住，安全）。
// ============================================================================
#include "panels/panels.h"

#include "core/gcobj.h"
#include "core/guard.h"
#include "core/invoke.h"
#include "core/log.h"
#include "core/reflect.h"
#include "core/taskbus.h"

#include "il2cpp/il2cpp_runtime.h"

#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ir = il2cpp::runtime;

// ================================================================ 状态
static volatile LONG g_listBusy = 0;
static volatile LONG g_callBusy = 0;

static int  g_selMethod = -1;          // 当前方法在方法表里的下标
static ArkParamSlot g_slots[ARK_INVOKE_MAX_PARAMS];
static int  g_slotCount = 0;

static char g_methodFilter[64] = { 0 };
static bool g_includeParents = false;   // ImGui::Checkbox 要真 bool，别拿 int 强转

static char g_result[768] = { 0 };
static int  g_resultOk = 0;

// ================================================================ 任务
static void ListTask(void *ud)
{
    Il2CppClass *k = (Il2CppClass *)ud;
    ArkGuardSetAction("inv.list");
    const int n = ArkInvokeListMethods(k, g_includeParents ? 1 : 0, 1200);
    Log("[inv] 枚举方法 %s：%d 个", ArkSelClassName(), n);
    g_selMethod = -1;
    g_slotCount = 0;
    InterlockedExchange(&g_listBusy, 0);
}

static void RequestList()
{
    if (InterlockedCompareExchange(&g_listBusy, 1, 0) != 0) return;
    if (!ArkTaskPost(ListTask, (void *)ArkSelClass(), "inv.list"))
        InterlockedExchange(&g_listBusy, 0);
}

static void PrepTask(void *ud)
{
    MethodInfo *m = (MethodInfo *)ud;
    ArkGuardSetAction("inv.prep");
    g_slotCount = ArkInvokePrepare(m, g_slots, ARK_INVOKE_MAX_PARAMS);
    InterlockedExchange(&g_listBusy, 0);
}

static void RequestPrep(MethodInfo *m)
{
    if (InterlockedCompareExchange(&g_listBusy, 1, 0) != 0) return;
    if (!ArkTaskPost(PrepTask, (void *)m, "inv.prep"))
        InterlockedExchange(&g_listBusy, 0);
}

struct CallReq
{
    MethodInfo  *method;
    unsigned     thisHandle;   // 0 = 无 this
    ArkParamSlot slots[ARK_INVOKE_MAX_PARAMS];
    int          n;
};

static void CallTask(void *ud)
{
    CallReq *r = (CallReq *)ud;
    if (!r) { InterlockedExchange(&g_callBusy, 0); return; }

    ArkGuardSetAction("inv.call");
    Il2CppObject *thiz = r->thisHandle ? ArkGcTarget(r->thisHandle) : nullptr;

    // 「用当前选中对象」在这里解析：gchandle 查询是 il2cpp 调用，
    // 渲染线程只搬运句柄号，不该问运行时"它现在指向谁"。
    for (int i = 0; i < r->n; ++i)
    {
        if (r->slots[i].useSel)
            r->slots[i].obj = thiz;
    }

    char out[768] = { 0 };
    const bool ok = ArkInvokeCall(r->method, thiz, r->slots, r->n, out, sizeof(out));

    // 结果字符串由 UI 读，直接覆盖 + 最后置长度即可（固定缓冲，不会重新分配）
    _snprintf_s(g_result, sizeof(g_result), _TRUNCATE, "%s", out);
    g_resultOk = ok ? 1 : 0;

    Log("[inv] 调用结果：%s", out);
    free(r);
    InterlockedExchange(&g_callBusy, 0);
}

static void RequestCall(MethodInfo *m)
{
    if (InterlockedCompareExchange(&g_callBusy, 1, 0) != 0) return;

    CallReq *r = (CallReq *)calloc(1, sizeof(CallReq));
    if (!r) { InterlockedExchange(&g_callBusy, 0); return; }

    r->method = m;
    r->thisHandle = ArkSelObject();
    r->n = g_slotCount;
    if (r->n > ARK_INVOKE_MAX_PARAMS) r->n = ARK_INVOKE_MAX_PARAMS;
    memcpy(r->slots, g_slots, sizeof(ArkParamSlot) * (size_t)r->n);

    if (!ArkTaskPost(CallTask, r, "inv.call"))
    {
        free(r);
        InterlockedExchange(&g_callBusy, 0);
    }
}

// ================================================================ UI
static void DrawMethodList()
{
    ImGui::BeginChild("##methList", ImVec2(340, 0), ImGuiChildFlags_Border);

    const bool busy = InterlockedCompareExchange(&g_listBusy, 0, 0) != 0;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button("枚举方法")) RequestList();
    if (busy) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Checkbox("含父类", &g_includeParents))
        RequestList();

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mf", "过滤方法名", g_methodFilter, sizeof(g_methodFilter));

    ImGui::Separator();

    ImGui::BeginChild("##ml", ImVec2(0, 0));
    const int n = ArkInvokeMethodCount();
    if (n == 0)
    {
        ImGui::TextDisabled("（还没枚举。先在对象页选一个类。）");
        ImGui::TextDisabled("当前类：%s", ArkSelClassName()[0] ? ArkSelClassName() : "未选");
    }

    for (int i = 0; i < n; ++i)
    {
        ArkMethodEntry *e = ArkInvokeMethodAt(i);
        if (!e || !e->method) continue;

        if (g_methodFilter[0])
        {
            const char *hit = strstr(e->label, g_methodFilter);
            if (!hit) continue;
        }

        // 泛型定义没有方法体，点了也调不了 —— 直接标灰，避免反复试错
        if (e->isGenericDef) ImGui::BeginDisabled();
        char label[240];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "%s###m%d", e->label, i);
        if (ImGui::Selectable(label, g_selMethod == i))
        {
            g_selMethod = i;
            RequestPrep(e->method);
        }
        if (e->isGenericDef) ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

static void DrawCaller()
{
    ImGui::BeginChild("##caller", ImVec2(0, 0));

    ArkMethodEntry *e = (g_selMethod >= 0) ? ArkInvokeMethodAt(g_selMethod) : nullptr;
    if (!e || !e->method)
    {
        ImGui::TextDisabled("从左边挑一个方法。");
        ImGui::TextDisabled("提示：先开「战斗 → 结构探测」把类名摸出来，或在对象页搜到目标类。");
        ImGui::EndChild();
        return;
    }

    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", e->label);

    const unsigned selObj = ArkSelObject();
    if (e->isStatic)
        ImGui::TextDisabled("静态方法，不需要 this");
    else if (selObj)
        ImGui::TextDisabled("this = 对象页当前选中的实例");
    else
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "实例方法但没有选 this（去对象页选一个实例）");

    ImGui::Separator();

    if (g_slotCount == 0)
    {
        ImGui::TextDisabled("（无参数）");
    }
    else
    {
        for (int i = 0; i < g_slotCount; ++i)
        {
            ArkParamSlot &s = g_slots[i];
            ImGui::PushID(i);
            ImGui::Text("%s", s.name);
            ImGui::SameLine(120);

            if (s.unsupported)
            {
                ImGui::TextColored(ImVec4(1, 0.55f, 0.45f, 1), "%s  暂不支持构造（自建结构体）", s.typeName);
            }
            else if (s.kind == VK_OBJECT)
            {
                if (selObj)
                {
                    if (ImGui::SmallButton("用当前选中对象"))
                        s.useSel = 1;
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton("null"))
                {
                    s.useSel = 0;
                    s.obj = nullptr;
                }
                ImGui::SameLine();
                if (s.useSel)
                    ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.7f, 1.0f), "%s = 选中对象", s.typeName);
                else
                    ImGui::TextDisabled("%s = %p", s.typeName, (void *)s.obj);
            }
            else
            {
                ImGui::SetNextItemWidth(220);
                ImGui::InputText("##p", s.text, sizeof(s.text));
                ImGui::SameLine();
                ImGui::TextDisabled("%s%s", s.typeName, s.isEnum ? "（可填名字或数字）" : "");
            }
            ImGui::PopID();
        }
    }

    ImGui::Separator();

    const bool busy = InterlockedCompareExchange(&g_callBusy, 0, 0) != 0;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button("调用", ImVec2(160, 0)))
        RequestCall(e->method);
    if (busy) ImGui::EndDisabled();

    if (g_result[0])
    {
        ImGui::Separator();
        ImGui::TextDisabled("结果：");
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
        ImGui::TextColored(g_resultOk ? ImVec4(0.6f, 1.0f, 0.7f, 1.0f) : ImVec4(1.0f, 0.6f, 0.5f, 1.0f),
                           "%s", g_result);
        ImGui::PopTextWrapPos();
    }

    ImGui::EndChild();
}

void ArkPanelInvoke()
{
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "当前类：%s",
                       ArkSelClassName()[0] ? ArkSelClassName() : "（去对象页选一个类）");
    ImGui::Separator();

    DrawMethodList();
    ImGui::SameLine();
    DrawCaller();
}
