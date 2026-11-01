// watch.cpp — 字段监视与编辑
// ============================================================================
// 线程分工（这是本文件唯一需要看懂的东西）：
//   ArkWatchRefresh()       Worker 线程   → 读值、填缓存。UI 只读缓存，绝不自己碰托管堆
//   ArkWatchApplyWrites()   游戏线程       → 把 UI 收集的待写入真正写下去
//   ArkPanelWatch()         渲染线程       → 只画缓存 + 收集请求
//
// ★ 对象必须用 gchandle 钉住。裸 Il2CppObject* 不是 GC 根，
//   对象被回收后你读到的是一块自由链垃圾（内容非 0 ⇒ 看起来"还活着"），
//   然后就是一次莫名其妙的访问违例。
// ============================================================================
#include "panels/panels.h"
#include "core/log.h"
#include "core/reflect.h"
#include "core/taskbus.h"
#include "core/guard.h"
#include "mod_config.h"
#include "il2cpp/il2cpp_runtime.h"

#include <imgui.h>
#include <cstdio>
#include <cstring>

namespace ir = il2cpp::runtime;

struct ArkWatchEntry
{
    char         classFull[192];
    char         fieldName[96];
    Il2CppClass *klass;
    FieldInfo   *field;
    uint32_t     objHandle;      // gchandle（0 = 静态字段或无对象）
    int          isStatic;
    int          kind;

    char         value[192];     // Worker 刷新，UI 只读
    char         editBuf[64];    // UI 编辑缓冲
    int          editing;

    volatile LONG writePending;
    double        writeValue;
};

static ArkWatchEntry g_watch[MOD_WATCH_MAX];
static volatile LONG g_watchCount;
static volatile LONG g_uiDirty;

int ArkWatchCount() { return (int)InterlockedCompareExchange(&g_watchCount, 0, 0); }

bool ArkWatchAdd(const char *classFullName, const char *fieldName,
                 Il2CppObject *instance, bool isStatic)
{
    if (!classFullName || !fieldName) return false;

    const int n = ArkWatchCount();
    if (n >= MOD_WATCH_MAX)
    {
        Log("[watch] 槽位满（%d）", MOD_WATCH_MAX);
        return false;
    }

    // 去重：同一个 (类,字段,对象) 只留一条
    for (int i = 0; i < n; ++i)
    {
        if (strcmp(g_watch[i].classFull, classFullName) != 0) continue;
        if (strcmp(g_watch[i].fieldName, fieldName) != 0) continue;
        Il2CppApi &api = ir::Api();
        Il2CppObject *cur = (g_watch[i].objHandle && api.il2cpp_gchandle_get_target)
                                ? api.il2cpp_gchandle_get_target(g_watch[i].objHandle) : nullptr;
        if (cur == instance) return true;
    }

    // 元数据解析在渲染线程做是安全的（不分配、不碰 GC），
    // 真正有风险的是装箱值类型，这里只取 FieldInfo*。
    const ArkClassEntry *ce = ArkReflectFindClass(classFullName);
    Il2CppClass *klass = ce ? ce->klass : nullptr;
    FieldInfo *f = klass ? ArkReflectFindField(klass, fieldName, nullptr) : nullptr;
    if (!f)
    {
        Log("[watch] 找不到字段 %s::%s", classFullName, fieldName);
        return false;
    }

    ArkWatchEntry *e = &g_watch[n];
    memset(e, 0, sizeof(*e));
    strcpy_s(e->classFull, sizeof(e->classFull), classFullName);
    strcpy_s(e->fieldName, sizeof(e->fieldName), fieldName);
    e->klass = klass;
    e->field = f;
    e->isStatic = isStatic ? 1 : 0;

    Il2CppApi &api = ir::Api();
    Il2CppType *t = api.il2cpp_field_get_type ? api.il2cpp_field_get_type(f) : nullptr;
    e->kind = (int)ArkReflectKindOf(t);

    if (instance && !isStatic && api.il2cpp_gchandle_new)
    {
        e->objHandle = api.il2cpp_gchandle_new(instance, true);   // pinned
    }

    InterlockedIncrement(&g_watchCount);
    Log("[watch] + %s::%s（%s，obj=%p）", classFullName, fieldName,
        isStatic ? "静态" : "实例", (void *)instance);
    return true;
}

static void WatchRemoveAt(int i)
{
    const int n = ArkWatchCount();
    if (i < 0 || i >= n) return;

    Il2CppApi &api = ir::Api();
    if (g_watch[i].objHandle && api.il2cpp_gchandle_free)
        api.il2cpp_gchandle_free(g_watch[i].objHandle);

    // 尾部顶上（顺序会变，但 watch 列表的顺序没有语义）
    if (i != n - 1)
        g_watch[i] = g_watch[n - 1];
    memset(&g_watch[n - 1], 0, sizeof(ArkWatchEntry));
    InterlockedDecrement(&g_watchCount);
}

void ArkWatchClearAll()
{
    Il2CppApi &api = ir::Api();
    const int n = ArkWatchCount();
    for (int i = 0; i < n; ++i)
    {
        if (g_watch[i].objHandle && api.il2cpp_gchandle_free)
            api.il2cpp_gchandle_free(g_watch[i].objHandle);
        memset(&g_watch[i], 0, sizeof(ArkWatchEntry));
    }
    InterlockedExchange(&g_watchCount, 0);
}

// ================================================================ Worker 侧
void ArkWatchRefresh()
{
    const int n = ArkWatchCount();
    if (n <= 0) return;

    Il2CppApi &api = ir::Api();
    for (int i = 0; i < n; ++i)
    {
        ArkWatchEntry *e = &g_watch[i];
        if (!e->field) continue;

        Il2CppObject *obj = nullptr;
        if (e->objHandle && api.il2cpp_gchandle_get_target)
            obj = api.il2cpp_gchandle_get_target(e->objHandle);

        if (!e->isStatic && !obj)
        {
            strcpy_s(e->value, sizeof(e->value), "(对象已失效)");
            continue;
        }

        char buf[192];
        const int rc = ArkReflectReadFieldText(e->field, obj, buf, sizeof(buf));
        if (rc == 0)
            strcpy_s(e->value, sizeof(e->value), buf);
        else
            _snprintf_s(e->value, sizeof(e->value), _TRUNCATE, "(读取失败 %d)", rc);
    }
}

void ArkWatchApplyWrites()
{
    const int n = ArkWatchCount();
    for (int i = 0; i < n; ++i)
    {
        ArkWatchEntry *e = &g_watch[i];
        if (InterlockedExchange(&e->writePending, 0) == 0) continue;
        if (!e->field) continue;

        Il2CppApi &api = ir::Api();
        Il2CppObject *obj = nullptr;
        if (e->objHandle && api.il2cpp_gchandle_get_target)
            obj = api.il2cpp_gchandle_get_target(e->objHandle);
        if (!e->isStatic && !obj) continue;

        ArkGuardSetAction("watch.write");
        if (ArkReflectWriteFieldNum(e->field, obj, e->writeValue))
            Log("[watch] 写入 %s::%s = %.6g", e->classFull, e->fieldName, e->writeValue);
        else
            Log("[watch] WARN: %s::%s 类型不支持直接写", e->classFull, e->fieldName);
    }
}

// ================================================================ UI
// 待写入请求：渲染线程写这个结构，游戏线程从队列里取。
struct WriteReq { int index; double value; };
static void WriteTask(void *ud)
{
    WriteReq *r = (WriteReq *)ud;
    if (!r) return;
    const int n = ArkWatchCount();
    if (r->index >= 0 && r->index < n)
    {
        g_watch[r->index].writeValue = r->value;
        InterlockedExchange(&g_watch[r->index].writePending, 1);
        ArkWatchApplyWrites();     // 已经在游戏线程上，直接落
    }
    free(r);
}

static void PostWrite(int index, double value)
{
    WriteReq *r = (WriteReq *)malloc(sizeof(WriteReq));
    if (!r) return;
    r->index = index;
    r->value = value;
    if (!ArkTaskPost(WriteTask, r, "watch.write"))
        free(r);
}

static const char *KindName(int k)
{
    switch (k)
    {
    case VK_BOOL:   return "bool";
    case VK_INT:    return "int";
    case VK_LONG:   return "long";
    case VK_FLOAT:  return "float";
    case VK_DOUBLE: return "double";
    case VK_STRING: return "string";
    case VK_OBJECT: return "object";
    case VK_STRUCT: return "struct";
    default:        return "?";
    }
}

void ArkPanelWatch()
{
    ImGui::TextDisabled("值由后台线程每秒刷新；改动立即生效（投递到游戏线程写入）");
    ImGui::SameLine();
    if (ImGui::Button("全部清空")) ArkWatchClearAll();
    ImGui::SameLine();
    ImGui::TextDisabled("共 %d / %d", ArkWatchCount(), MOD_WATCH_MAX);

    // ---- 手动添加 ----
    static char sCls[192] = { 0 };
    static char sFld[96] = { 0 };
    static char sObj[32] = { 0 };

    if (ImGui::CollapsingHeader("手动添加（类.字段 + 可选对象指针）"))
    {
        ImGui::SetNextItemWidth(360);
        ImGui::InputTextWithHint("##cls", "命名空间.类名", sCls, sizeof(sCls));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##fld", "字段名（精确）", sFld, sizeof(sFld));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::InputTextWithHint("##obj", "对象指针 0x...", sObj, sizeof(sObj));

        if (ImGui::Button("添加"))
        {
            Il2CppObject *inst = nullptr;
            if (sObj[0])
            {
                unsigned long long v = 0;
                sscanf_s(sObj, "%llx", &v);
                inst = (Il2CppObject *)(uintptr_t)v;
            }
            if (ArkWatchAdd(sCls, sFld, inst, inst == nullptr))
                sFld[0] = 0;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("不填对象指针 = 按静态字段处理");
    }

    // ---- 列表 ----
    const int n = ArkWatchCount();
    if (n == 0)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("还没有监视项。去「类浏览器」里点某个字段右侧的「监视」。");
        return;
    }

    if (ImGui::BeginTable("##watch", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("目标", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("当前值", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("写入", ImGuiTableColumnFlags_WidthFixed, 210.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 44.f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < n; ++i)
        {
            ArkWatchEntry *e = &g_watch[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", e->classFull);
            ImGui::SameLine(0, 2);
            ImGui::TextColored(ImVec4(0.85f, 0.9f, 1.0f, 1), ".%s", e->fieldName);
            if (e->isStatic)
            {
                ImGui::SameLine(0, 4);
                ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.f, 1), "[S]");
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", KindName(e->kind));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e->value);

            ImGui::TableNextColumn();
            const bool writable = (e->kind == VK_BOOL || e->kind == VK_INT ||
                                   e->kind == VK_LONG || e->kind == VK_FLOAT ||
                                   e->kind == VK_DOUBLE);
            if (!writable)
            {
                ImGui::TextDisabled("只读");
            }
            else if (e->kind == VK_BOOL)
            {
                bool b = (strcmp(e->value, "true") == 0);
                if (ImGui::Checkbox("##b", &b))
                    PostWrite(i, b ? 1.0 : 0.0);
            }
            else
            {
                ImGui::SetNextItemWidth(120);
                if (ImGui::InputText("##v", e->editBuf, sizeof(e->editBuf),
                                     ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_CharsScientific))
                {
                    PostWrite(i, atof(e->editBuf));
                    e->editBuf[0] = 0;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("写"))
                {
                    PostWrite(i, atof(e->editBuf));
                    e->editBuf[0] = 0;
                }
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("×"))
                WatchRemoveAt(i);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
