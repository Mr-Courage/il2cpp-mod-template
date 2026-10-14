# il2cpp-mod-template

一个可复用的 **Unity IL2CPP 游戏 MOD 框架模板**：手动映射注入 → IL2CPP 反射 → ImGui 覆盖层 → 热重载。

它只提供**骨架**，不含任何具体游戏的业务逻辑。把 `src/mod_config.h` 里的常量改成你的目标游戏，在 `src/user_hooks.cpp` 的六个扩展点里写自己的功能，就能接到任意一款 Unity IL2CPP 游戏上。

```
DllMain ──CreateThread──▶ Worker 线程
                            ├─ 等目标模块加载
                            ├─ il2cpp_core：解析 IL2CPP 全部导出
                            ├─ 装 ImGui overlay（D3D11 Present vtable hook）
                            ├─ 等 Unity 域 + 挂接线程
                            ├─ 反射自测（枚举程序集/镜像/类）
                            ├─ 装 detour 探针
                            └─ 每秒 tick ──▶ Mod_OnTick()
                                                 │
  渲染线程  Present 钩子 ──▶ ImGui 面板 ──▶ Mod_OnFrame() / Mod_DrawPanelTabs()
                                                 │
  游戏逻辑线程  class_from_name 钩子 ───────────▶ Mod_OnGameThread()   ← 唯一安全的 il2cpp 调用点
```

---

## 它解决什么

给一款 Unity IL2CPP 游戏做运行时分析/修改，通常要先把这几件事全部跑通，每一件都能卡你几天：

| 环节 | 本模板的做法 |
| --- | --- |
| 注入 | 手动映射，产物刻意做到**零静态 TLS**、导入表只有 4 个系统 DLL |
| 拿 device | 自建 dummy swapchain 取 `dxgi!Present`（全进程唯一代码），改**共享 vtable[8]**，不依赖注入时机 |
| 反射 | 内置 `il2cpp_core`，全部走官方 API 拿偏移，不硬编码结构体布局 |
| 跨线程 | 明确区分渲染线程 / 游戏逻辑线程 / Worker 线程，并给出请求队列范式 |
| 看日志 | 文件 + 无锁环形缓冲 + 面板实时日志（分级着色、过滤、自动滚动） |
| 改代码 | 热重载：无需重启游戏，改完重新注入即可（含完整的逆序清理） |
| 退出游戏 | ★ **游戏自己退出也有清理**：子类化窗口过程捕获 `WM_CLOSE`/`WM_DESTROY` + 看门狗兜底，抢在进程销毁序列前卸掉全局钩子并还原 vtable（见 [docs/pitfalls.md](docs/pitfalls.md#八进程退出)） |

---

## 目录结构

```
il2cpp-mod-template/
├─ src/
│  ├─ mod_config.h        ★ 接入新游戏唯一要改的源文件（常量集中在这里）
│  ├─ main.cpp              框架引导：注入入口 / 日志 / 配置 / 探针 / 面板骨架
│  ├─ probe.cpp             反射自测 + 类结构 dump
│  ├─ user_hooks.cpp      ★ 你的业务代码写这里（六个扩展点 + 请求队列示例）
│  ├─ log_buffer.h/.cpp     无锁日志环形缓冲
│  ├─ exit_guard.h/.cpp  ★ 游戏主动退出（点 X / Alt+F4）时的清理触发器
│  ├─ hot_reload.cpp        进程内单实例握手 + 卸载例程
│  └─ ui/
│     ├─ imgui_overlay.cpp  D3D11 Present hook + imgui 后端 + 中文字体
│     ├─ ui_api.h           overlay 对外接口（含线程模型说明）
│     └─ log_panel.h/.cpp   日志面板
├─ il2cpp_core/             跨平台 IL2CPP 反射库（内置，无需外部依赖）
├─ third_party/imgui/       ImGui 1.91.0（MIT，仅保留 DX11 后端，含本地补丁）
├─ config/mod.ini.example   运行时配置示例（挂钩方法 / dump 类 / 热点压测）
├─ mod_template.def         精确导出表
├─ Makefile                 有 make 就用这个
├─ build.ps1                推荐入口（含 Il2CppApi ABI 陈旧自动重编）
└─ docs/                    architecture / quickstart / pitfalls / injector-abi
```

---

## 快速开始

### 1. 编译

依赖 **mingw-w64**（默认 `D:/mingw64`，可用环境变量 `MINGW` 覆盖）。

```powershell
.\build.ps1            # 增量构建
.\build.ps1 -Clean     # 清掉 build/ 全量重编
```

产物 `il2cpp_mod.dll`（约 2.2 MB）。有 `make` 也可以用 `make` / `make clean`。

发布前建议跑一遍合规校验：

```bash
objdump -p il2cpp_mod.dll | grep -i "DLL Name"   # 只应有 KERNEL32/msvcrt/SHELL32/USER32
objdump -p il2cpp_mod.dll | grep -c emutls       # 必须是 0
```

### 2. 接入你的游戏

改 **`src/mod_config.h`** 一处即可：

| 常量 | 说明 |
| --- | --- |
| `MOD_GAME_MODULE` | IL2CPP 运行时模块名，绝大多数是 `GameAssembly.dll` |
| `MOD_MAIN_IMAGE` | 游戏主程序集镜像名，一般是 `Assembly-CSharp.dll` |
| `MOD_PANEL_TITLE` / `MOD_TOGGLE_HINT` | 面板标题与热键提示 |
| `MOD_LOG_FILE` / `MOD_UNLOAD_LOG` | 日志文件名 |
| `MOD_LOCK_PREFIX` | 单实例互斥体前缀（**必须带 PID 后缀**，代码自动拼） |
| `MOD_TOOL_DLL` | 可选的分析工具 DLL，没有就留空 |

然后重新 `.\build.ps1`。详细的分步流程见 [docs/quickstart.md](docs/quickstart.md)。

### 3. 注入

本模板产出的是「手动映射载荷」，需要一个支持手动映射的注入器。注入器与本 DLL 的接口约定见 [docs/injector-abi.md](docs/injector-abi.md)。

### 4. 确认它活了

游戏里按 **INSERT** 打开面板，切到「状态」tab：

- `已解析导出` > 0 → IL2CPP 导出解析成功
- `域状态 = 已获取` + `引导完成 = 是` → 反射链路通了
- `detour 命中计数 > 0` → 函数体钩子生效

日志同时写在 DLL 同目录的 `il2cpp_mod.log`。手动映射时取不到模块路径，会依次降级到环境变量 `IL2CPP_MOD_DIR` → 进程 CWD → `%TEMP%`。

---

## 六个扩展点

全部在 `src/user_hooks.cpp`，默认空实现 + 可跑的示例。**线程搞错就是随机崩溃**，务必按表来：

| 扩展点 | 调用线程 | 用途 |
| --- | --- | --- |
| `Mod_OnReady()` | Worker（已 attach 域），一次 | 找类、拿单例、装你自己的钩子 |
| `Mod_OnTick()` | Worker（已 attach 域），每秒 | 周期性读游戏状态 |
| `Mod_OnFrame()` | **渲染线程**，每帧 | 轻量状态推进。★ 不能调 il2cpp |
| `Mod_OnGameThread()` | **游戏逻辑线程**，每次类型查找 | ★ 唯一能安全调 il2cpp 的地方，必须自己限流 |
| `Mod_DrawPanelTabs()` | 渲染线程 | 在面板 TabBar 里加你自己的 tab |
| `Mod_OnShutdown()` | 注入器线程，卸载时 | 停你起的线程、还你借的资源 |

### 最重要的模式：跨线程请求队列

ImGui 按钮跑在渲染线程，而「创建 Unity 对象 / 调游戏方法」只能在游戏逻辑线程做。在按钮回调里直接调，崩点会离调用点很远，极难定位。

`src/user_hooks.cpp` 给了完整实现，核心是三段：

```
渲染线程按钮回调 ──写请求──▶ volatile 请求槽 ──▶ 游戏逻辑线程 Mod_OnGameThread() 消费执行
```

请求槽用 `volatile` + `InterlockedXxx`。**不要用 `std::mutex` / `std::queue`** —— 它们会引入静态 TLS 与 libstdc++ 依赖，直接违反下面的硬约束。

---

## 硬约束（改代码前必读）

本 DLL 走手动映射注入，映射器未实现静态 TLS 模拟。所以：

- **不用** `std::thread` / `thread_local` —— 线程一律 `CreateThread`
- **不用** `std::atomic` —— 计数用 `volatile LONG` + `InterlockedIncrement`
- **不用** `std::string` / `std::vector` —— 用定长 C 缓冲（`il2cpp_core` 里基于 STL 的函数已用 `IL2CPP_CORE_NO_STL` 剔除）
- **不用** 函数内 `static` 非平凡对象 —— 会生成 `__cxa_guard_*`，拖进 libstdc++ EH → `__emutls_v.*` 静态 TLS
- 全静态链接，禁用异常与 RTTI（`-fno-exceptions -fno-rtti`）
- 图形侧只链 `d3d11/dxgi/gdi32/user32`，**不链 `d3dcompiler`**（imgui DX11 后端已本地打补丁改成运行时 `GetProcAddress`）
- 不编 `imgui_impl_win32` —— 它有函数内 static，会引入 emutls；输入由 `imgui_overlay.cpp` 自己轮询喂

违反了**不会报错**，只会在注入后随机崩，而且崩点离违规处很远。

更多实战踩坑（值类型参数不能走 `runtime_invoke`、虚方法必须走 `runtime_invoke`、Unity 对象存活判断等）见 [docs/pitfalls.md](docs/pitfalls.md)。

---

## 文档

- [docs/architecture.md](docs/architecture.md) — 启动流程、线程模型、各模块职责
- [docs/quickstart.md](docs/quickstart.md) — 从零接到一款新游戏（分步 + 排障清单）
- [docs/pitfalls.md](docs/pitfalls.md) — 踩坑铁律，每一条都是崩过一次换来的
- [docs/injector-abi.md](docs/injector-abi.md) — 与手动映射注入器的接口约定

---

## 关于 `Esp*` 导出名

`mod_template.def` 同时导出 `Mod*` 与 `Esp*` 两组名字，后者是转发别名。原项目里 ESP 指 "external script payload"，与「透视挂」无关；保留别名是为了让老注入器不用改。想彻底去掉，删掉 `src/hot_reload.cpp` 末尾的转发区块并同步 `.def` 即可。

---

## 许可与免责

- `third_party/imgui/` — ImGui 1.91.0，MIT License，版权归 Omar Cornut 及贡献者。见 `third_party/imgui/imgui-1.91.0/LICENSE.txt`。
- 其余代码按 MIT License 发布，见 [LICENSE](LICENSE)。

**免责声明**：本项目是逆向工程与运行时分析的技术框架，仅供学习研究、以及对**你拥有或已获授权**的软件进行调试。使用者需自行确保行为符合所在地法律法规与目标软件的用户协议。作者不对任何滥用行为负责，也不对使用本框架造成的存档损坏、账号封禁或任何数据损失负责。请勿用于联网多人游戏的作弊。
