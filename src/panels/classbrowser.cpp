// classbrowser.cpp — 类浏览器（工具 tab）
// ============================================================================
// 这是整个修改器的"眼睛"：不管游戏怎么更新，只要还能枚举类，就能在这里
// 现场把字段/方法找出来，不依赖任何硬编码偏移。
//
// 线程说明：本文件全部跑在渲染线程。
//   · 只做**无分配的读**（读指针、读类型名、读字段元信息）——
//     il2cpp 的元数据访问本身不加锁也不碰 GC，在任意线程都安全。
//   · 绝不调 field_get_value_object 去装箱值类型（那会分配托管对象）。
//     想看值就加进 Watch，由 Worker 线程刷新。
// ============================================================================
#include "panels/panels.h"
#include "core/log.h"
#include "core/reflect.h"
#include "core/taskbus.h"
#include "il2cpp/il2cpp_runtime.h"

#include <imgui.h>
#include <cstdio>
#include <cstring>

namespace ir = il2cpp::runtime;

// ---- 面板状态（静态存储期；char 数组是平凡类型，不涉及静态构造）----
static char  g_search[128] = { 0 };
static char  g_lastSearch[128] = { 0 };
static int   g_hits[1024];
static int   g_hitCount = 0;
static int   g_selClass = -1;          // 索引到 g_hits 的下标
static int   g_selEntry = -1;          // 索引到 ArkReflectClassAt
static char  g_note[256] = { 0 };

static void RedoSearch()
{
    g_hitCount = ArkReflectSearch(g_search, g_hits, (int)(sizeof(g_hits) / sizeof(g_hits[0])));
    strcpy_s(g_lastSearch, sizeof(g_lastSearch), g_search);
}

// 找"本类类型的静态字段"——典型就是 s_instance / Instance 这种单例。
// 只读指针，不装箱，安全。
static Il2CppObject *GuessSingleton(Il2CppClass *k)
{
    Il2CppApi &api = ir::Api();
    if (!k || !api.il2cpp_class_get_fields || !api.il2cpp_field_get_value_object) return nullptr;

    void *iter = nullptr;
    FieldInfo *f = nullptr;
    while ((f = api.il2cpp_class_get_fields(k, &iter)) != nullptr)
    {
        if (!ArkReflectIsStaticField(f)) continue;
        Il2CppType *t = api.il2cpp_field_get_type(f);
        if (!t) continue;
        Il2CppClass *ft = api.il2cpp_class_from_type(t);
        if (ft != k) continue;                        // 类型必须是它自己
        Il2CppObject *o = api.il2cpp_field_get_value_object(f, nullptr);
        if (o) return o;
    }
    return nullptr;
}

static void DrawFieldRow(FieldInfo *f, Il2CppClass *owner, const ArkClassEntry *entry)
{
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_field_get_name || !api.il2cpp_field_get_type) return;

    const char *name = api.il2cpp_field_get_name(f);
    Il2CppType *t = api.il2cpp_field_get_type(f);
    const bool isStatic = ArkReflectIsStaticField(f);
    const size_t off = api.il2cpp_field_get_offset ? api.il2cpp_field_get_offset(f) : 0;

    char tname[256];
    ArkReflectTypeName(tname, sizeof(tname), t);

    ImGui::PushID(f);
    ImGui::TableNextRow();

    ImGui::TableNextColumn();
    if (isStatic)
        ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "S");
    else
        ImGui::TextDisabled("-");

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);

    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", tname);

    ImGui::TableNextColumn();
    if (isStatic)
        ImGui::TextDisabled("static");
    else
        ImGui::TextDisabled("0x%04zX", off);

    ImGui::TableNextColumn();
    if (ImGui::SmallButton("监视"))
    {
        Il2CppObject *inst = nullptr;
        if (!isStatic)
        {
            inst = GuessSingleton(entry->klass);
            if (!inst)
            {
                _snprintf_s(g_note, sizeof(g_note), _TRUNCATE,
                            "「%s」是实例字段，但这个类没有可用的静态单例。"
                            "先让对象存在，再用对象指针手动添加。", name);
            }
        }
        if (isStatic || inst)
        {
            char full[512];
            _snprintf_s(full, sizeof(full), _TRUNCATE, "%s.%s", entry->ns, entry->name);
            if (ArkWatchAdd(full, name, inst, isStatic))
                _snprintf_s(g_note, sizeof(g_note), _TRUNCATE, "已加入监视：%s", name);
        }
    }
    ImGui::PopID();
}

static void DrawClassDetail(const ArkClassEntry *e)
{
    Il2CppApi &api = ir::Api();
    if (!e || !e->klass) return;

    char full[512];
    if (e->ns && e->ns[0])
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s.%s", e->ns, e->name);
    else
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s", e->name);

    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", full);
    ImGui::SameLine();
    ImGui::TextDisabled("  %p", (void *)e->klass);

    // 继承链
    ImGui::TextDisabled("继承：");
    ImGui::SameLine(0, 4);
    {
        Il2CppClass *k = e->klass;
        int depth = 0;
        char chain[512] = { 0 };
        int pos = 0;
        while (k && depth < 12 && pos < (int)sizeof(chain) - 40)
        {
            const char *nm = api.il2cpp_class_get_name(k);
            if (depth) pos += _snprintf_s(chain + pos, sizeof(chain) - pos, _TRUNCATE, " ← ");
            pos += _snprintf_s(chain + pos, sizeof(chain) - pos, _TRUNCATE, "%s", nm ? nm : "?");
            k = api.il2cpp_class_get_parent(k);
            ++depth;
        }
        ImGui::TextUnformatted(chain);
    }

    // 单例探测结果
    Il2CppObject *singleton = GuessSingleton(e->klass);
    if (singleton)
    {
        ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "检测到静态单例：%p", (void *)singleton);
        ImGui::SameLine();
        ImGui::TextDisabled("（实例字段可直接监视）");
    }

    // 跳到「对象」页：类浏览器只能看类型，要改值必须先拿到**活实例**。
    // 这个按钮把当前类交给对象页，省得在那边再搜一遍。
    if (ImGui::SmallButton("→ 对象（枚举实例）"))
    {
        ArkSelSetClass(e->klass);
        ArkObjRequestEnum();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("到「对象」页枚举它当前的活实例、展开字段树");

    if (api.il2cpp_class_instance_size)
        ImGui::TextDisabled("实例大小 %d 字节", api.il2cpp_class_instance_size(e->klass));

    ImGui::Separator();

    if (ImGui::BeginTabBar("##classdetail"))
    {
        if (ImGui::BeginTabItem("字段"))
        {
            if (ImGui::BeginTable("##fields", 5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("S", ImGuiTableColumnFlags_WidthFixed, 22.f);
                ImGui::TableSetupColumn("字段名", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("偏移", ImGuiTableColumnFlags_WidthFixed, 70.f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 56.f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                int count = ArkReflectEnumFields(e->klass,
                    [](FieldInfo *f, Il2CppClass *owner, int depth, void *ud) {
                        DrawFieldRow(f, owner, (const ArkClassEntry *)ud);
                    }, (void *)e);
                (void)count;

                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("方法"))
        {
            if (ImGui::BeginTable("##methods", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY))
            {
                ImGui::TableSetupColumn("方法名", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("参数", ImGuiTableColumnFlags_WidthFixed, 50.f);
                ImGui::TableSetupColumn("入口", ImGuiTableColumnFlags_WidthFixed, 140.f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                Il2CppClass *k = e->klass;
                int depth = 0;
                while (k && depth < 8)
                {
                    void *iter = nullptr;
                    MethodInfo *m = nullptr;
                    while (api.il2cpp_class_get_methods && (m = api.il2cpp_class_get_methods(k, &iter)))
                    {
                        const char *nm = api.il2cpp_method_get_name ? api.il2cpp_method_get_name(m) : "?";
                        const uint32_t pc = api.il2cpp_method_get_param_count
                                                ? api.il2cpp_method_get_param_count(m) : 0;
                        // methodPointer 在 MethodInfo 头部第一个字段，空 = 泛型定义/未实例化
                        const void *mp = *(void **)m;

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(nm);
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%u", pc);
                        ImGui::TableNextColumn();
                        if (mp)
                            ImGui::TextDisabled("%p", mp);
                        else
                            ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "泛型定义");
                    }
                    k = api.il2cpp_class_get_parent(k);
                    ++depth;
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---- 重建索引：遍历 5 万个类要几十毫秒，绝不能在渲染线程做 ----
static volatile LONG g_rebuilding = 0;
static int           g_seenGen = 0;

static void RebuildIndexTask(void *ud)
{
    (void)ud;
    const int before = ArkReflectClassCount();
    Log("[reflect] 开始重建类索引（原表 %d 个类）…", before);
    const int after = ArkReflectRebuildIndex();
    Log("[reflect] 重建完成：%d 个类（原 %d）", after, before);
    InterlockedExchange(&g_rebuilding, 0);
}

void ArkPanelClassBrowser()
{
    // 索引换代（重建完成）后，搜索缓存和选中项都指向旧表 —— 必须重扫。
    const int gen = ArkReflectIndexGen();
    if (gen != g_seenGen)
    {
        g_seenGen = gen;
        RedoSearch();
        g_selClass = -1;
        g_selEntry = -1;
    }

    if (g_rebuilding)
    {
        ImGui::BeginDisabled();
        ImGui::Button("重建中…");
        ImGui::EndDisabled();
    }
    else if (ImGui::Button("重建索引"))
    {
        // 走任务总线投递到 Worker：遍历几万个类，别在渲染线程干
        if (ArkTaskPost(RebuildIndexTask, nullptr, "reflect.reindex"))
        {
            InterlockedExchange(&g_rebuilding, 1);
            Log("[ui] 已投递：重建类索引");
        }
        else
        {
            Log("[ui] 投递失败：任务队列满了？");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("共 %d 个类 · 输关键字搜索（类名或命名空间子串，不分大小写）",
                        ArkReflectClassCount());

    ImGui::SetNextItemWidth(320);
    if (ImGui::InputText("##search", g_search, sizeof(g_search),
                         ImGuiInputTextFlags_EnterReturnsTrue))
    {
        RedoSearch();
        g_selEntry = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("搜索"))
    {
        RedoSearch();
        g_selEntry = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("清空"))
    {
        g_search[0] = 0;
        RedoSearch();
        g_selEntry = -1;
    }

    if (strcmp(g_search, g_lastSearch) != 0)
    {
        // 边打字边搜（只在文本真的变了时重算，避免每帧 4 万次子串匹配）
        RedoSearch();
    }

    if (g_note[0])
    {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1), "%s", g_note);
        ImGui::SameLine();
        if (ImGui::SmallButton("知道了")) g_note[0] = 0;
    }

    const float avail = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("##list", ImVec2(340, avail), true))
    {
        ImGui::TextDisabled("命中 %d 个", g_hitCount);
        ImGui::Separator();

        ImGuiListClipper clipper;
        clipper.Begin(g_hitCount);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const ArkClassEntry *e = ArkReflectClassAt(g_hits[i]);
                if (!e) continue;

                char label[600];
                if (e->ns && e->ns[0])
                    _snprintf_s(label, sizeof(label), _TRUNCATE, "%s.%s", e->ns, e->name);
                else
                    _snprintf_s(label, sizeof(label), _TRUNCATE, "%s", e->name);

                if (ImGui::Selectable(label, g_selClass == i))
                {
                    g_selClass = i;
                    g_selEntry = g_hits[i];
                }
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("##detail", ImVec2(0, avail), true))
    {
        if (g_selEntry >= 0)
            DrawClassDetail(ArkReflectClassAt(g_selEntry));
        else
            ImGui::TextDisabled("左边选一个类");
    }
    ImGui::EndChild();
}
