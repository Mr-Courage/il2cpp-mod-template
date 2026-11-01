// panels/dumper.cpp — 「Dumper」页：运行时导出 dump.cs / script.json
// ============================================================================
// 全部状态读 volatile 原子值，按钮只做一次 ArkDumperStart() —— 面板线程
// 不碰任何 il2cpp。dump 本体在 Worker（core/dumper.cpp），渲染线程零风险。
// ============================================================================
#include <imgui.h>
#include <windows.h>

#include "core/dumper.h"
#include "core/log.h"
#include "core/reflect.h"

void ArkPanelDumper()
{
    const int st = ArkDumperState();

    ImGui::TextUnformatted("把当前进程的类/字段/方法导出成 Il2CppDumper 兼容格式。");
    ImGui::TextDisabled("产物：dump.cs（伪 C# 声明 + 偏移/RVA）与 script.json（IDA 脚本用）");
    ImGui::Separator();

    // ---- 状态 ----
    const char *stText = st == 1 ? "导出中" : st == 2 ? "已完成"
                        : st == 3 ? "失败" : "空闲";
    ImGui::Text("状态：");

    ImGui::SameLine();
    if (st == 1)          ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s — %s", stText, ArkDumperPhase());
    else if (st == 2)     ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "%s", stText);
    else if (st == 3)     ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s — %s", stText, ArkDumperPhase());
    else                  ImGui::TextUnformatted(stText);

    if (st == 1)
    {
        const int pm = ArkDumperPermille();
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "%d.%d%%", pm / 10, pm % 10);
        ImGui::ProgressBar(pm / 1000.0f, ImVec2(-1, 0), overlay);
    }

    // ---- 统计 ----
    ImGui::Text("类 %d    方法 %d    字段 %d",
                ArkDumperClasses(), ArkDumperMethods(), ArkDumperFields());
    ImGui::TextDisabled("输出目录：%s", ArkDumperOutDir());

    ImGui::Spacing();

    // ---- 按钮 ----
    const bool hasIndex = ArkReflectClassCount() > 0;
    if (st == 1)
    {
        ImGui::Button("导出中…（Worker 被占用，其它任务会排队）");
    }
    else if (ImGui::Button(st == 2 ? "重新导出" : "开始导出"))
    {
        if (ArkDumperStart())
            Log("[dump] 已开始（面板可看进度，日志有结果路径）");
    }
    if (!hasIndex)
        ImGui::TextDisabled("⚠ 类索引为空 —— 先到「类浏览器」页触发一次索引构建");

    ImGui::Separator();
    ImGui::TextDisabled("说明");
    ImGui::BulletText("dump 在 Worker 线程执行，只读元数据，不碰 Unity 对象。");
    ImGui::BulletText("方法 VA 用 PE 首选基址计算，直接可喂给 Il2CppDumper 的 IDA 脚本。");
    ImGui::BulletText("不含 string literal / 枚举常量值（运行时导出表拿不到，不影响定位）。");
    ImGui::BulletText("游戏更新后立刻 dump 一次，就不会再用过期偏移了。");
}
