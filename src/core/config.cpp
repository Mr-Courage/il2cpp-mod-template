// config.cpp — 配置注册 / ini 读写 / 自动 UI
#include "core/config.h"
#include "core/log.h"
#include "mod_config.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cfloat>

#include <imgui.h>

static Vec<ArkCfgItem> g_items;      // 零初始化即等价于 Init()
static volatile LONG   g_dirty;

void ArkCfgRegister(ArkCfgItem *items, int count)
{
    if (!items || count <= 0) return;
    for (int i = 0; i < count; ++i)
        g_items.Push(items[i]);
    Log("[cfg] 注册 %d 项（累计 %d）", count, g_items.count);
}

ArkCfgItem *FindItem(const char *key)
{
    if (!key) return nullptr;
    for (int i = 0; i < g_items.count; ++i)
        if (strcmp(g_items.data[i].key, key) == 0)
            return &g_items.data[i];
    return nullptr;
}

// ================================================================ ini
static void Trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

void ArkCfgLoad()
{
    wchar_t path[MAX_PATH];
    if (!ArkPathW(path, MAX_PATH, MOD_TOOLS_INI))
        return;

    FILE *f = _wfopen(path, L"r");
    if (!f)
    {
        Log("[cfg] 没有 %ls，全部使用默认值（首次保存后会生成）", MOD_TOOLS_INI);
        return;
    }

    int applied = 0;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        Trim(k);
        Trim(v);
        if (!k[0]) continue;

        ArkCfgItem *it = FindItem(k);
        if (!it)
        {
            // 不报错：配置项可能在版本迭代中被删掉，旧 ini 留着是正常的。
            continue;
        }
        switch (it->type)
        {
        case CFG_BOOL:
            *(int *)it->ptr = (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0);
            break;
        case CFG_INT:
        case CFG_ENUM:
            *(int *)it->ptr = atoi(v);
            break;
        case CFG_FLOAT:
            *(float *)it->ptr = (float)atof(v);
            break;
        default: break;
        }
        ++applied;
    }
    fclose(f);
    Log("[cfg] 从 %ls 应用 %d 项（共注册 %d）", MOD_TOOLS_INI, applied, g_items.count);
}

void ArkCfgSave()
{
    wchar_t path[MAX_PATH];
    if (!ArkPathW(path, MAX_PATH, MOD_TOOLS_INI))
        return;

    FILE *f = _wfopen(path, L"w");
    if (!f)
    {
        Log("[cfg] WARN: 写不了 %ls（%ls）", MOD_TOOLS_INI, path);
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "; arkModify 配置 —— 由程序自动生成，手工改也行，下次启动读回\n");
    fprintf(f, "; 生成于 %04u-%02u-%02u %02u:%02u:%02u\n\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    const char *lastPrefix = nullptr;
    char curPrefix[64] = { 0 };

    for (int i = 0; i < g_items.count; ++i)
    {
        ArkCfgItem &it = g_items.data[i];

        // 按 "xxx.yyy" 的点前部分分节，让 ini 好读
        const char *dot = strchr(it.key, '.');
        int plen = dot ? (int)(dot - it.key) : (int)strlen(it.key);
        if (plen >= (int)sizeof(curPrefix)) plen = sizeof(curPrefix) - 1;

        if (!lastPrefix || strncmp(lastPrefix, it.key, plen) != 0 || (int)strlen(lastPrefix) != plen)
        {
            memcpy(curPrefix, it.key, plen);
            curPrefix[plen] = 0;
            fprintf(f, "\n[%s]\n", curPrefix);
        }

        switch (it.type)
        {
        case CFG_BOOL:
            fprintf(f, "%-28s = %d        ; %s\n", it.key, *(int *)it.ptr ? 1 : 0,
                    it.label ? it.label : "");
            break;
        case CFG_INT:
        case CFG_ENUM:
            fprintf(f, "%-28s = %d        ; %s\n", it.key, *(int *)it.ptr,
                    it.label ? it.label : "");
            break;
        case CFG_FLOAT:
            fprintf(f, "%-28s = %.4f     ; %s\n", it.key, *(float *)it.ptr,
                    it.label ? it.label : "");
            break;
        default: break;
        }
    }
    fclose(f);

    InterlockedExchange(&g_dirty, 0);
}

void ArkCfgMarkDirty() { InterlockedExchange(&g_dirty, 1); }

bool ArkCfgTakeDirty()
{
    return InterlockedExchange(&g_dirty, 0) != 0;
}

// ================================================================ 数值读写
bool ArkCfgGetBool(const char *key)
{
    ArkCfgItem *it = FindItem(key);
    return it && it->type == CFG_BOOL ? (*(int *)it->ptr != 0) : false;
}
int ArkCfgGetInt(const char *key)
{
    ArkCfgItem *it = FindItem(key);
    return (it && (it->type == CFG_INT || it->type == CFG_ENUM)) ? *(int *)it->ptr : 0;
}
float ArkCfgGetFloat(const char *key)
{
    ArkCfgItem *it = FindItem(key);
    return (it && it->type == CFG_FLOAT) ? *(float *)it->ptr : 0.f;
}
void ArkCfgSetBool(const char *key, bool v)
{
    ArkCfgItem *it = FindItem(key);
    if (it && it->type == CFG_BOOL) { *(int *)it->ptr = v ? 1 : 0; ArkCfgMarkDirty(); }
}
void ArkCfgSetInt(const char *key, int v)
{
    ArkCfgItem *it = FindItem(key);
    if (it && (it->type == CFG_INT || it->type == CFG_ENUM)) { *(int *)it->ptr = v; ArkCfgMarkDirty(); }
}
void ArkCfgSetFloat(const char *key, float v)
{
    ArkCfgItem *it = FindItem(key);
    if (it && it->type == CFG_FLOAT) { *(float *)it->ptr = v; ArkCfgMarkDirty(); }
}

// ================================================================ 自动 UI
// 两列表格：左列标签定宽，右列控件铺满剩余宽度。
// ★ 提示文字**不再放行内**。上一版是"控件 + 一长串灰字"，长提示会把整行撑成两行、
//   并把控件宽度挤成随机值 —— 这是战斗页看着乱的主要原因。现在改成悬停气泡。
// ★ label == nullptr 的项是"只落盘、不画控件"：值由模块自己在 onDraw 里用
//   滑块/大按钮画（比如倍速那四个键），但**仍然持久化**。既好看又记得住。
bool ArkCfgDrawGroup(ArkCfgItem *items, int count)
{
    bool changed = false;
    if (!items || count <= 0) return false;

    int visible = 0;
    for (int i = 0; i < count; ++i)
        if (items[i].label) ++visible;
    if (visible <= 0) return false;

    ImGui::PushID(items[0].key);

    const float avail = ImGui::GetContentRegionAvail().x;
    float labelW = avail * 0.40f;
    if (labelW < 72.0f) labelW = 72.0f;
    if (labelW > 150.0f) labelW = 150.0f;

    const ImGuiTableFlags tf = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX |
                               ImGuiTableFlags_NoBordersInBody;
    if (ImGui::BeginTable("##cfgtbl", 2, tf))
    {
        ImGui::TableSetupColumn("##lab", ImGuiTableColumnFlags_WidthFixed, labelW);
        ImGui::TableSetupColumn("##ctl", ImGuiTableColumnFlags_WidthStretch);

        for (int i = 0; i < count; ++i)
        {
            ArkCfgItem &it = items[i];
            if (!it.label) continue;

            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(it.label);
            if (it.hint && *it.hint)
                ImGui::SetItemTooltip("%s", it.hint);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);

            bool itemChanged = false;
            switch (it.type)
            {
            case CFG_BOOL:
            {
                bool v = (*(int *)it.ptr) != 0;
                if (ImGui::Checkbox("##v", &v)) { *(int *)it.ptr = v ? 1 : 0; itemChanged = true; }
                break;
            }
            case CFG_INT:
            {
                int *p = (int *)it.ptr;
                // 声明了范围就用滑块（一眼看得出边界），没范围才用输入框
                if (it.max > it.min)
                {
                    if (ImGui::SliderInt("##v", p, (int)it.min, (int)it.max))
                        itemChanged = true;
                }
                else if (ImGui::InputInt("##v", p, it.step > 0 ? (int)it.step : 1, 10))
                {
                    itemChanged = true;
                }
                break;
            }
            case CFG_FLOAT:
            {
                float *p = (float *)it.ptr;
                if (ImGui::DragFloat("##v", p, it.step > 0 ? it.step : 0.01f,
                                     it.min, it.max, "%.3f"))
                    itemChanged = true;
                break;
            }
            case CFG_ENUM:
            {
                int *p = (int *)it.ptr;
                const char *preview = (it.names && *p >= 0 && *p < it.nameCount) ? it.names[*p] : "?";
                if (ImGui::BeginCombo("##v", preview))
                {
                    for (int n = 0; n < it.nameCount; ++n)
                    {
                        bool sel = (*p == n);
                        if (ImGui::Selectable(it.names[n], sel)) { *p = n; itemChanged = true; }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                break;
            }
            default: break;
            }

            if (itemChanged) changed = true;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::PopID();
    if (changed) ArkCfgMarkDirty();
    return changed;
}

int ArkCfgDrawAll(const char *filterKeyPrefix)
{
    int shown = 0;
    const size_t plen = filterKeyPrefix ? strlen(filterKeyPrefix) : 0;

    for (int i = 0; i < g_items.count; ++i)
    {
        ArkCfgItem &it = g_items.data[i];
        if (plen && strncmp(it.key, filterKeyPrefix, plen) != 0) continue;
        if (ArkCfgDrawGroup(&it, 1)) {}
        ++shown;
    }
    return shown;
}
