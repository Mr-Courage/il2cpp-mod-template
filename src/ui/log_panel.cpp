// log_panel.cpp — 日志面板：从无锁环形缓冲拉新行，按级别着色显示
// ============================================================================
// 面板侧缓存：把环形缓冲里的行**拷贝**进自己的定长数组。
// 不能直接持有槽指针 —— 环形缓冲是覆盖式的，槽内容随时会被下一行改写。
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include "log_buffer.h"
#include "mod_config.h"
#include "ui/ui_api.h"

#define PANEL_MAX_LINES 512

namespace
{

struct PanelState
{
    char lines[PANEL_MAX_LINES][LOG_LINE_MAX];
    int levels[PANEL_MAX_LINES];
    int count;          // 当前有效行数（<= PANEL_MAX_LINES）
    int totalSeen;      // 累计读到的行数（用于显示「共 N 条」）
    LONG lastSeq;       // 水位线：只拉 seq 大于它的行
    bool autoScroll;
    int filterLevel;    // 只显示 >= 该级别的行（0=全部）
};

// 全局（非函数内）static POD —— 走 .bss，不会产生 __cxa_guard。
// ★ 千万别挪进函数里变成 `static PanelState p;`：
//   非平凡初始化会引入 __cxa_guard_acquire/release -> libstdc++ EH -> __emutls_v.*
//   静态 TLS，而手动映射未模拟静态 TLS，那样必崩。
PanelState g_panel;

void PanelSink(const char *text, int level, void *user)
{
    PanelState *p = (PanelState *)user;

    // 满了就丢最旧的：整体前移一格
    if (p->count >= PANEL_MAX_LINES)
    {
        memmove(p->lines[0], p->lines[1], sizeof(p->lines[0]) * (PANEL_MAX_LINES - 1));
        memmove(p->levels, p->levels + 1, sizeof(p->levels[0]) * (PANEL_MAX_LINES - 1));
        p->count--;
    }

    size_t n = strlen(text);
    if (n >= LOG_LINE_MAX) n = LOG_LINE_MAX - 1;
    memcpy(p->lines[p->count], text, n);
    p->lines[p->count][n] = 0;
    p->levels[p->count] = level;
    p->count++;
    p->totalSeen++;
}

} // namespace

void LogPanelClear()
{
    g_panel.count = 0;
    g_panel.totalSeen = 0;
    g_panel.lastSeq = 0;   // 重置水位线 —— 下一帧会重新把缓冲里的全量拉回来
}

void LogPanelDraw()
{
    PanelState *p = &g_panel;

    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), MOD_TOGGLE_HINT);
    ImGui::SameLine(0, -1);
    ImGui::TextDisabled("开关面板 (当前: %s)", UiPanelVisible() ? "开" : "关");

    ImGui::Separator();
    ImGui::Text("显示 %d 行 / 共收到 %d 条   |   级别过滤:", p->count, p->totalSeen);
    ImGui::SameLine();
    const char *lvNames[4] = { "全部", "INFO+", "WARN+", "ERROR" };
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##lv", &p->filterLevel, lvNames, 4);
    ImGui::SameLine();
    ImGui::Checkbox("自动滚动", &p->autoScroll);
    ImGui::SameLine();
    if (ImGui::Button("清空")) LogPanelClear();
    ImGui::SameLine();
    ImGui::TextDisabled("overlay: %s | frame=%lu",
                        UiIsReady() ? "ready" : "未初始化", UiFrameCount());
    ImGui::Separator();

    // 拉取新行（只拷贝比水位线新的部分）
    p->lastSeq = LogBufDrain(p->lastSeq, PanelSink, p);

    ImGui::BeginChild("log", ImVec2(0, -1), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (int i = 0; i < p->count; i++)
    {
        if (p->levels[i] < p->filterLevel) continue;

        ImVec4 c(0.82f, 0.84f, 0.86f, 1.0f);                   // INFO 灰白
        if (p->levels[i] == LV_OK)         c = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);  // 绿
        else if (p->levels[i] == LV_WARN)  c = ImVec4(1.0f, 0.82f, 0.35f, 1.0f);   // 黄
        else if (p->levels[i] == LV_ERROR) c = ImVec4(1.0f, 0.42f, 0.40f, 1.0f);   // 红

        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextUnformatted(p->lines[i]);
        ImGui::PopStyleColor();
    }
    if (p->autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}
