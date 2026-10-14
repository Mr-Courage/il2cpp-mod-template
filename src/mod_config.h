// mod_config.h — 接入一款新游戏时，你**唯一**需要修改的源文件
// ============================================================================
// 这里集中了全部「与目标游戏耦合」的常量。换游戏时改这里，其余代码不用动：
//
//   1. MOD_GAME_MODULE      —— 目标进程的 IL2CPP 运行时模块名
//   2. MOD_MAIN_IMAGE       —— 游戏主程序集镜像名（反射自测 / 找类用）
//   3. MOD_PANEL_TITLE 等   —— 面板外观
//   4. MOD_LOG_FILE 等      —— 日志文件命名
//   5. MOD_LOCK_PREFIX      —— 单实例互斥体前缀（热重载握手，必须带 PID 后缀）
//
// 构建相关的常量（编译单元清单、输出 DLL 名、mingw 路径）在 Makefile / build.ps1。
//
// 关于导出名前缀 Esp*（EspUnload / EspClaimInstance …）：
//   这是历史命名（原项目里 ESP 指 "external script payload"），与「透视挂」无关。
//   模板同时导出 Mod* 与 Esp* 两组名字，后者是转发别名 ——
//   你的注入器认哪一套都能用。想彻底改名就全局替换 src/hot_reload.cpp 里的
//   Esp 前缀，并同步 mod_template.def。
// ============================================================================
#pragma once

// ---------------------------------------------------------------- 1. 目标进程
// IL2CPP 运行时所在的模块。绝大多数 Unity IL2CPP 游戏都是 GameAssembly.dll；
// 少数老版本是 UnityPlayer.dll + mono，或自定义名（用 Process Explorer 看模块列表）。
#define MOD_GAME_MODULE      "GameAssembly.dll"

// 等它加载出来的秒数。游戏进程刚起来时 GameAssembly 可能还没 LoadLibrary，
// 从启动瞬间注入（--launch）时这个值要留够。
#define MOD_GAME_MODULE_WAIT 90

// 游戏逻辑所在的程序集镜像名。反射自测默认遍历这个镜像；
// 也用于 Mod_OnReady 里 ir::Image(MOD_MAIN_IMAGE) 找类。
#define MOD_MAIN_IMAGE       "Assembly-CSharp.dll"

// ---------------------------------------------------------------- 2. 面板
#define MOD_PANEL_TITLE      "il2cpp mod"
#define MOD_TOGGLE_HINT      "INSERT"   // 面板开关热键提示文字（键位在 imgui_overlay.cpp）
#define MOD_PANEL_W          900.0f     // 首次出现时的窗口尺寸
#define MOD_PANEL_H          560.0f

// ---------------------------------------------------------------- 3. 日志
// 主日志落在「模块所在目录」；手动映射时取不到模块路径，会依次降级到
// 环境变量 MOD_DIR_ENV -> 进程 CWD -> %TEMP%（见 main.cpp 的 PickLogDir）。
#define MOD_LOG_FILE         L"il2cpp_mod.log"
// 卸载阶段的日志，固定写 %TEMP%（那时主日志可能已不可用）。
#define MOD_UNLOAD_LOG       L"il2cpp_mod_unload.log"

// ---------------------------------------------------------------- 4. 热重载握手
// ★ 名字**必须带 PID 后缀**（代码里自动拼）。踩过的坑：不带 PID 时用了
//   Local\ 会话级命名空间，同会话下另一个进程的实例会误伤本次注入。
#define MOD_LOCK_PREFIX      L"Local\\il2cpp_mod_lock_"

// ---------------------------------------------------------------- 5. 工具 DLL（可选）
// 双 DLL 架构：本 DLL 手动映射（零 .tls、无 STL），分析工具走普通 LoadLibrary，
// 由系统加载器管理 TLS/CRT，可以自由用 STL/异常。没有工具 DLL 就留空字符串。
#define MOD_TOOL_DLL         L"il2cpp_tool.dll"

// ---------------------------------------------------------------- 6. 目录覆盖
// 注入时可用该环境变量显式指定「模块目录」，覆盖自动探测。
#define MOD_DIR_ENV          L"IL2CPP_MOD_DIR"

// ---------------------------------------------------------------- 7. 行为开关
// 反射自测：引导完成后自动枚举程序集并打印（关掉可省掉一次全量遍历）。
#define MOD_REFLECT_SELFTEST 1
// 自测时最多打印多少个镜像名（避免刷屏）。
#define MOD_SELFTEST_IMAGES  30

// ---------------------------------------------------------------- 8. 字体字形覆盖
// ★ 必须覆盖「完整 CJK 表意文字」，不能只用 ImGui 的
//   GetGlyphRangesChineseSimplifiedCommon()。
//
//   那个函数只含 **2500 个最常用字**，像 渲 / 帧 / 逻 / 辑 都不在里面
//   （实测：面板上的「渲染帧数」会显示成「?染?数」、「逻辑」变「??」）。
//
//   1（默认）= 完整 CJK 表意文字 + 符号区（圈号/箭头/几何/勾叉/全角）
//              实测 21791 字形 → 图集 4096×4096 → RGBA 64MB，构建约 190ms（一次性，
//              发生在第一次 Present，通常在 logo/加载画面上，感知不到）
//   0        = 只覆盖「常用简体 2500 + 符号区」
//              实测 3299 字形 → 图集 1024×2048 → RGBA 8MB，构建约 30ms
//              ★ 缺点：稀有字显示成 "?"，且日志里有这类字时也会变 "?"
//
//   64MB 显存在 PC 上通常可忽略；若目标平台显存紧张（比如低端核显）再改成 0。
//   也可以不改文件，直接在编译命令里加 -DMOD_FONT_FULL_CJK=0 覆盖。
#ifndef MOD_FONT_FULL_CJK
#define MOD_FONT_FULL_CJK    1
#endif
