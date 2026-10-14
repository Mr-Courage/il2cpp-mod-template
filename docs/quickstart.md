# 从零接到一款新游戏

假设你现在有一款全新的 Unity IL2CPP 游戏，要把它接到本框架上。按下面的顺序走，每一步都有「怎么判断成功」。

---

## 第 0 步：确认它是 IL2CPP

看游戏安装目录里有没有：

- `GameAssembly.dll` — IL2CPP 运行时（绝大多数情况）
- 只有 `UnityPlayer.dll` + `MonoBleedingEdge/` — 那是 Mono，本框架不适用

同时确认它是 **x64**：本模板只编 64 位。用 `dumpbin /headers GameAssembly.dll` 或随便一个 PE 查看工具确认 machine 是 `AMD64`。

有些游戏会把运行时模块改名（混淆或自定义 loader）。用 Process Explorer 看目标进程的模块列表，把真正的 IL2CPP 模块名填进 `MOD_GAME_MODULE`。

---

## 第 1 步：改 `src/mod_config.h`

只有这个文件需要改：

```cpp
#define MOD_GAME_MODULE      "GameAssembly.dll"     // 第 0 步确认的模块名
#define MOD_MAIN_IMAGE       "Assembly-CSharp.dll"  // 游戏逻辑所在的程序集
#define MOD_PANEL_TITLE      "我的游戏 MOD"
```

不确定 `MOD_MAIN_IMAGE`？先保持默认 —— 第 3 步的日志会把进程里**所有**镜像名都打出来，看一眼就知道。

如果你是完全从头开始、还没有注入器，先把第 4 步的接口约定看一遍。

---

## 第 2 步：编译

```powershell
$env:MINGW = "D:/mingw64"     # 不是这个路径就改，或设到系统环境变量里
.\build.ps1
```

看到 `[OK] il2cpp_mod.dll (xxxx KB)` 就成功了。

发布前跑一遍合规校验：

```bash
objdump -p il2cpp_mod.dll | grep -i "DLL Name"   # 只应有 KERNEL32 / msvcrt / SHELL32 / USER32
objdump -p il2cpp_mod.dll | grep -c emutls       # 必须是 0
```

---

## 第 3 步：注入并确认链路通了

按 [injector-abi.md](injector-abi.md) 用你的注入器注入。然后游戏里按 **INSERT** 打开面板，切到「状态」tab：

| 字段 | 期望 | 不对怎么办 |
| --- | --- | --- |
| `已解析导出` | > 100 | IL2CPP 模块名错了，或模块还没加载（游戏还在启动画面） |
| `域状态` | 已获取 | 同上；或游戏用了非标准初始化流程 |
| `引导完成` | 是 | 看日志最后一行 —— 崩在哪一步，最后一条日志就是答案 |
| `镜像数量` | > 10 | 反射没跑起来 |
| `class_from_name` | 持续增长 | 函数体 detour 没生效 |

**日志文件**在 DLL 同目录的 `il2cpp_mod.log`；手动映射时取不到模块路径，会降级到环境变量 `IL2CPP_MOD_DIR` → 进程 CWD → `%TEMP%`。

引导日志里会打印全部镜像名：

```
  image[0] = mscorlib.dll
  image[1] = Assembly-CSharp.dll      ← 游戏逻辑在这里
  image[2] = UnityEngine.CoreModule.dll
  ...
[reflect] 自测 OK: assemblies=42 classes=38120
```

打到这一行，就说明 **IL2CPP 导出解析 → 域挂接 → 反射**整条链路通了。

---

## 第 4 步：找到你要的类

两条路：

**A. 有 `dump.cs`（Il2CppDumper 生成）** — 直接查类名和字段名。

**B. 没有 dump，或者怀疑偏移漂移** — 用模板自带的 dump：

把 `config/mod.ini.example` 复制到 DLL 同目录，改名 `mod.ini`，填：

```ini
dump_class = 命名空间.类名
dump_max_fields = 200
```

重新注入，日志里会出现该类的继承链 + 全部字段（类型名 / 偏移 / 当前值）。**偏移一律以这里打印的为准**，不要信任何文档里的旧偏移 —— IL2CPP 的字段偏移会随游戏版本漂移。

也可以在面板「我的功能」tab 里实时输入类名 dump（见 `src/user_hooks.cpp` 的请求队列示例）。

---

## 第 5 步：写你的功能

全部写在 `src/user_hooks.cpp`：

```cpp
void Mod_OnReady() {
    // 找类、拿单例、缓存 FieldInfo* / MethodInfo*
    // 这里跑在 Worker 线程，il2cpp 已 attach
}

void Mod_OnTick() {
    // 每秒读一次状态。★ 只在值变化时打日志，别无脑 Log
}

void Mod_OnGameThread() {
    // ★ 唯一能安全调 il2cpp 的地方（创建 Unity 对象、调游戏方法）
    // 每次类型查找都会命中，开销大的逻辑必须自己限流
}

void Mod_DrawPanelTabs() {
    if (!ImGui::BeginTabItem("我的功能")) return;
    // 按钮里只写请求，真正执行放到 Mod_OnGameThread 消费
    ImGui::EndTabItem();
}
```

`src/user_hooks.cpp` 里有一份**完整可跑**的「按钮 → 请求槽 → 游戏逻辑线程执行」实现，直接照着改。

---

## 排障清单

### 注入后完全没反应，也没有日志文件

- 确认注入器真的成功了（err=0）。目标进程**已运行一段时间**的话，反作弊驱动可能已武装，直接对 `--pid` 注入会返回 err=87 —— 要在游戏启动前注入（抢在驱动加载前），或用热重载路径。
- 日志会降级到 `%TEMP%`，去那儿找找。
- 若日志显示 `已有实例在运行，本次注入放弃` —— 上一次的镜像没卸干净，先用注入器的 `--replace` 走一遍完整卸载。

### 有日志，但停在 `overlay installed` 之后、domain attached 之前

- 大概率是 **Il2CppApi ABI 不一致**：改了 `il2cpp_core/src/il2cpp/il2cpp_api_functions.h` 之后没全量重编。`build.ps1` 会自动检测并强制重编，用 `make` 的话手动 `make clean`。
- 表现是随机崩，崩点离改动处很远 —— 因为每个 TU 各自展开一份 `struct Il2CppApi`，只重编一部分会让字段偏移错位。

### overlay 装了但面板不显示

- 确认游戏用的是 **D3D11**。本模板只实现了 DX11 后端；DX12 / Vulkan 需要换 `imgui_impl_*`。
- 窗口不可见时 Present 可能不跑，overlay 也就不会初始化。进到游戏主界面再看。

### 面板显示但操作游戏后崩溃

- 99% 是**线程错了**：在渲染线程（按钮回调 / `Mod_OnFrame`）里调了 il2cpp。正确的做法是把执行挪到 `Mod_OnGameThread`。
- 剩下 1% 见 [pitfalls.md](pitfalls.md)。

### 热重载后钩子失效

- 旧镜像留在 IL2CPP 模块代码页上的跳板字节没被清掉。正常走 `ModUnload` 会调 `RemoveAllDetours()` 还原；如果卸载流程被中断（比如渲染线程没在 3 秒内完成清理），就会残留。
- 残留后新镜像的 detour 会装在已改写的字节上，**返回 true 看起来成功但实际是残缺钩子**。连续几次之后命中数会一路掉到 0。这种情况只能重启游戏。

### 点 X 关游戏 / Alt+F4 时游戏卡死

- 症状：正常玩没问题，点窗口右上角 X 就卡住，只能在任务管理器强制结束进程。
- 本模板已内置处理（`src/exit_guard.cpp`）：子类化游戏窗口过程捕获 `WM_CLOSE`/`WM_DESTROY`，
  外加看门狗轮询 `IsWindow(hwnd)`，触发后在**独立线程**上先卸全局鼠标钩子 + 还原 dxgi vtable，
  再走完整卸载。若仍卡死，看日志里有没有这几行：
  ```
  [exit] 检测到游戏退出信号（WM_CLOSE），启动清理线程
  [ui] 退出抢跑清理完成：全局鼠标钩子已卸、dxgi vtable 已还原
  [exit] ===== 游戏退出清理完成 =====
  ```
  **一行都没有** ⇒ 触发器没装上：确认 `[ui] first Present: imgui init ok` 之后有
  `[exit] 已子类化游戏窗口过程`。没有的话说明游戏窗口不是标准顶层窗口（或渲染不在主线程），
  退化为只靠看门狗 —— 看门狗要等窗口销毁后才触发，会晚一些。
- 如果你关掉的是**另一款**游戏用的旧 DLL（`MOD_LOCK_PREFIX` 相同、PID 不同）：不会有影响，
  互斥体名字带 PID。

---

## 换游戏时的检查清单

- [ ] `mod_config.h`：`MOD_GAME_MODULE` / `MOD_MAIN_IMAGE` / 面板与日志命名
- [ ] 确认目标模块是 x64
- [ ] 确认渲染后端是 D3D11
- [ ] `src/user_hooks.cpp` 里的业务逻辑换掉（模板里的示例只是演示请求队列）
- [ ] `config/mod.ini` 里的 `dump_class` 清空（那是上一个游戏的类名）
- [ ] 重新 `.\build.ps1 -Clean`
