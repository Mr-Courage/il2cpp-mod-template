// hotkeys.cpp — 热键设置面板（工具 tab）
// ============================================================================
// 这块 UI 只做两件事：把已注册热键列出来、把用户按下的键写回注册表。
// 真正的轮询在 core/hotkey.cpp 的 ArkHotkeyPoll()，跑在渲染线程每帧。
//
// ★ "按下想绑的键"这条流不是监听键盘消息，而是**轮询 GetAsyncKeyState**
//   逮第一个从 0 变 1 的键 —— 因为我们的界面根本收不到键盘消息
//   （imgui 的输入是我们自己喂的，见 ui/imgui_overlay.cpp）。
// ============================================================================
#include "panels/panels.h"
#include "core/hotkey.h"

#include <imgui.h>
#include <cstdio>

void ArkPanelHotkeys()
{
    ImGui::TextDisabled("按下想绑的键即可（面板开关是内置的，不在这里）");
    ImGui::Separator();

    // 正在等待按键的槽位。-1 = 没在等。
    static int sCapturing = -1;

    const int n = ArkHotkeyCount();
    if (n == 0)
    {
        ImGui::TextDisabled("还没有注册热键。调用 ArkHotkeyRegister() 就会出现在这里。");
        ImGui::Spacing();
        ImGui::TextDisabled("例：ArkHotkeyRegister(\"my.aim\", \"瞄准\", VK_F2, OnAimToggle);");
        return;
    }

    if (ImGui::BeginTable("##hotkeys", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("功能", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("键位", ImGuiTableColumnFlags_WidthFixed, 140.f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < n; ++i)
        {
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(ArkHotkeyNameAt(i));

            char keyName[32];
            ArkHotkeyKeyName(ArkHotkeyVkAt(i), keyName, sizeof(keyName));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(keyName);

            ImGui::TableNextColumn();
            if (sCapturing == i)
            {
                ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "等待按键…");
                const int vk = ArkHotkeyCapture();
                if (vk)
                {
                    if (!ArkHotkeySetVk(ArkHotkeyIdAt(i), vk))
                        ImGui::OpenPopup("##conflict");
                    sCapturing = -1;
                }

                // 冲突提示：这个键已经被别的功能占了。不做"抢过来"，
                // 因为那会让用户丢一个已有绑定且毫无提示。
                if (ImGui::BeginPopup("##conflict"))
                {
                    ImGui::TextColored(ImVec4(1, 0.42f, 0.42f, 1), "这个键已被别的热键占用");
                    ImGui::EndPopup();
                }
            }
            else if (ImGui::SmallButton("改键"))
            {
                sCapturing = i;
                ArkHotkeyBeginCapture(i);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("解绑"))
                ArkHotkeySetVk(ArkHotkeyIdAt(i), 0);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
