// uikit.h — 面板里反复用到的小排版件
// ============================================================================
// 全部是 inline + POD，没有全局对象（手动映射不跑静态构造器，见 core/vec.h 那段）。
// 设计约束只有一条：**一屏要装得下尽量多的信息**。
//   · 统计值一律 "暗标签 + 亮数值" 并排，不各占一行
//   · 一组统计用 4 列网格（标签/值/标签/值），两行装四项
//   · 说明文字进悬停气泡，不占行
// ============================================================================
#pragma once

#include <imgui.h>

namespace ui {

// 主题色（与 module.cpp 里那套保持一致）
inline const ImVec4 kText (0.92f, 0.93f, 0.95f, 1.00f);
inline const ImVec4 kDim  (0.60f, 0.61f, 0.64f, 1.00f);
inline const ImVec4 kOk   (0.45f, 0.85f, 0.55f, 1.00f);
inline const ImVec4 kWarn (0.95f, 0.70f, 0.30f, 1.00f);
inline const ImVec4 kBad  (1.00f, 0.42f, 0.42f, 1.00f);
inline const ImVec4 kAccent(0.45f, 0.72f, 0.98f, 1.00f);

// 暗色小字（标签、单位）
inline void Dim(const char *text)
{
    ImGui::TextDisabled("%s", text);
}

// "标签 值" —— 并排，不换行。用于卡片里那种一格里塞两个信息的场景。
inline void Stat(const char *label, const char *value, const ImVec4 &col = kText)
{
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(0, 6);
    ImGui::TextColored(col, "%s", value);
}

// 带小圆点的状态：● 战斗中 / ● 已停用。（● 在中文全字库里，不会出豆腐块）
inline void Dot(const ImVec4 &col)
{
    ImGui::TextColored(col, "\xe2\x97\x8f");
}

// 强调按钮：主操作用它，一眼能在一排灰按钮里认出来
inline bool Primary(const char *label, const ImVec2 &size = ImVec2(0, 0))
{
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.20f, 0.44f, 0.30f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.55f, 0.37f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.36f, 0.25f, 1.00f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

// 次级按钮：正常灰，尺寸统一，避免一排按钮长短不一
inline bool Secondary(const char *label, const ImVec2 &size = ImVec2(0, 0))
{
    return ImGui::Button(label, size);
}

// 四列统计网格。用法：
//   ui::GridBegin("##g");
//     ui::GridRow(); ui::GCellLabel("阶段"); ui::GCellText("PLAYING"); ...
//   ui::GridEnd();
// 列宽按内容比例分，面板拖窄也不会把值挤到行外（不写死像素）。
inline bool GridBegin(const char *id, float labelW = 52.0f)
{
    const ImGuiTableFlags f = ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_PadOuterX |
                              ImGuiTableFlags_NoBordersInBody;
    if (!ImGui::BeginTable(id, 4, f)) return false;
    ImGui::TableSetupColumn("##l1", ImGuiTableColumnFlags_WidthFixed, labelW);
    ImGui::TableSetupColumn("##v1", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("##l2", ImGuiTableColumnFlags_WidthFixed, labelW);
    ImGui::TableSetupColumn("##v2", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    return true;
}

inline void GridEnd() { ImGui::EndTable(); }

inline void GridRow() { ImGui::TableNextRow(); }

inline void GridLabel(const char *t)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", t);
}

inline void GridText(const char *t, const ImVec4 &col = kText)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col, "%s", t);
}

inline void GridNone()
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("—");
}

} // namespace ui
