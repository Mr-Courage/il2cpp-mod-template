# 踩坑铁律

每一条都是崩过一次换来的。改代码前扫一遍，能省几天。

---

## 一、IL2CPP 调用

### 值类型参数不能走 `il2cpp_runtime_invoke`

`runtime_invoke` 要求参数是 `void**`（指针数组）。传值类型（`int` / `float` / `bool`）时会表现为 **err=0 但参数被写成 0** —— 静默失败，最难查。

值类型参数改用**方法指针直调** `methodPointer`。

### 虚方法**必须**走 `runtime_invoke`

反过来：直调 `*(void**)mi`（方法指针）只适用于**非虚方法**。虚方法直调会拿到基类实现 → 值是垃圾或者直接 AV。

原因：`runtime_invoke` 内部会按对象的**实际类**做虚分发。

### x64 Windows 调用约定

直调方法指针时：

- 参数序：`this=RCX, a1=RDX, a2=R8, a3=R9, ...`
  （注意：如果上面用了 RCX 传 this，那 a1 从 RDX 开始）
- **末尾必须补一个 `MethodInfo*` 参数**
- 返回结构体时：>8 字节的返回值走**隐藏返回指针 RCX**，于是整体右移一位
  （`RCX=buf, RDX=this, R8=a1, ...`，末尾补 `mi`）
- 结构体参数：>8 字节按引用传，≤8 字节按值传

返回值缓冲用 `alignas(8) unsigned char buf[32]` + `memcpy` 取出。

### Unity「要 `Type` 参数」的 API 必须传托管对象

裸 `Il2CppType*` 传进去 → 秒崩。必须先 `il2cpp_type_get_object(...)` 转成 `System.Type` 的托管对象。

---

## 二、线程

### 场景操作绝不能放渲染线程

`Transform` 写入、`GetComponent`、`SetActive`、`Instantiate` —— 任何一个放 Present 钩子里，都会 AV 在 `unityplayer.dll`。

必须放到**游戏逻辑线程的 detour** 里。本模板的入口是 `Mod_OnGameThread()`。

### 每帧日志必须自限流

而且**限流计数器递增的那个点必须真的会被走到**。写过一次「只读不增」的版本，结果限流失效，日志刷到几百 MB 把磁盘写满。

同理：只在值**发生变化**时打日志，不要每秒无脑 `Log()`。写文件是临界区，会拖慢游戏逻辑线程。

---

## 三、Unity 对象

### 存活判断只看 `m_CachedPtr`

`+0x10` 处的 `m_CachedPtr != nullptr` 才是对象还活着。**不要用 `activeSelf` / `name` 判断** —— 对象被销毁后读这些字段是未定义行为。

### 跨场景不缓存 Unity 指针

重开一局 / 切场景后，缓存的指针全部失效。非要缓存就在使用前查存活，并且记下 `klass` 防止内存被回收复用后「看起来还活着」。

### 创建「完整」GameObject 只有两条路

1. 游戏自带的工厂方法
2. `Instantiate` 克隆一个**游戏原生对象**

❌ `il2cpp_object_new` + `AddComponent` —— 造出来的是空壳，缺一大堆引擎侧初始化
❌ `CreatePrimitive` —— 同上

### 托管数组需要「初始化元素」时，绝不 `il2cpp_object_new` 裸造

不调构造函数 → AV。复用游戏对象只改字段。

---

## 四、反射 / 字段定位

### 绝不按裸偏移读运行时结构

`MethodInfo`、`Il2CppClass` 的内部布局随 Unity 版本变。能走官方 API 就走：
`il2cpp_field_get_offset` / `il2cpp_class_get_fields` / `il2cpp_method_get_param_count` ...

### 偏移会漂移，永远以运行时打印的为准

`dump.cs` 是「某一版」的快照。游戏更新后偏移可能全变。**每次排查都重新 dump 一遍**，不要信任何文档里的旧偏移。

### 自动属性的 backing field 名带尖括号

```
<owner>k__BackingField
<ability>k__BackingField
```

不是 `_owner_...`。

### `il2cpp_class_get_fields` 连静态字段一起返回

静态字段用偏移**最高位**（`0x80000000`）标记。过滤时还要跳过编译器生成的字段（`__` 开头，如 `__c__` / `__Hotfix`）。

顺带：实例字段偏移如果落在 `<=0 || >0x2000`，说明拿到的偏移不可信，别按它取值。

### 子串匹配陷阱

`FindFieldOffBySubstring("_attack")` 会命中 `_attackTrigger`。要精确匹配就用 `strcmp`。

### 嵌套类按简单名查不到

用 `il2cpp_field_get_type` → `il2cpp_type_get_class_or_element_class()` 反推类最稳。

### 数组长度别用 `il2cpp_array_length`

直读 `max_length`（`+0x18`），数据区在 `+0x20`。`il2cpp_string_chars` 返回的是 **UTF-16**。

### API 返回值一律当可能 null

IL2CPP 的导出函数指针可能没解析到，调用结果也可能是 null。每一层都判。

---

## 五、构建 / 链接

### `Il2CppApi` 头变动必须全量重编

`il2cpp_api.h` 用 `#include` 把函数列表直接展开进 `struct Il2CppApi`，**每个 TU 各自展开一份**。往列表里增删一个成员，所有引用它的 `.o` 的字段偏移都会变。

只重编一部分 → 按旧偏移读，拿到另一个函数的指针 → 一调就崩，**崩点离改动处很远**（表现是「注入后闪退」，日志停在 `overlay installed` 之后、`domain attached` 之前，看起来完全无关）。

`build.ps1` 会做时间戳检查自动拦；用 `make` 就手动 `make clean`。

### 静态 TLS 是隐形炸弹

函数内 `static` 非平凡对象 → `__cxa_guard_acquire/release` → libstdc++ EH → `__emutls_v.*` 静态 TLS → 手动映射未模拟静态 TLS → **必崩**。

危险信号：

```bash
objdump -p il2cpp_mod.dll | grep -c emutls    # 必须是 0
```

### 导入表只应有系统 DLL

```bash
objdump -p il2cpp_mod.dll | grep -i "DLL Name"
# KERNEL32.dll / msvcrt.dll / SHELL32.dll / USER32.dll
```

出现 `libstdc++-6.dll` / `libwinpthread-1.dll` / `libgcc_s_*.dll` / `D3DCOMPILER_47.dll` 就说明约束破了。

### 改了头文件就全量重编

不只是 `il2cpp_api.h` —— 任何被多个 TU 包含的头，改了都要重编全部引用者。否则结构体布局不一致 → 运行时野指针。

### `#if 未定义宏` 不报错，会静默走 0 分支

```cpp
#if MOD_FONT_FULL_CJK          // 如果 MOD_FONT_FULL_CJK 没被定义…
    useFullCjk();
#else
    useCommonOnly();           // ← 编译的是这个，而且零告警
#endif
```

未定义的标识符在 `#if` 里求值为 **0**，这是 C 标准规定的合法行为 —— 编译器不会提醒你
"这个宏不存在"。所以「改了代码、编译通过、行为一点没变」的静默失效就是这么来的。

**2026-10-14 真实踩过**：`src/ui/imgui_overlay.cpp` 用了 `MOD_FONT_FULL_CJK` 做字形范围开关，
但**没 `#include "mod_config.h"`** → 宏未定义 → 一直走 `#else` 的老分支 → 面板缺字没修好，
而编译零 error/零 warning，日志还硬编码写着"用的是常用2500"，完全看不出问题。

两条防线（模板都已加）：

1. **include 之后紧跟一道 `#error` 保险**：
   ```cpp
   #include "mod_config.h"
   #ifndef MOD_FONT_FULL_CJK
   #  error "MOD_FONT_FULL_CJK 未定义 —— mod_config.h 没被包含进来"
   #endif
   ```
2. **日志里打印"实际生效"的模式**，而不是硬编码一句描述。比如字体那行会打
   `字形范围=完整CJK(4E00-9FFF)+符号区`，字形统计那行会打
   `字体图集 4096x4096, 合计 21791 字形` —— 数字对不上就是分支没走到。

> 自查手法：`nm -C build/xxx.o | grep <你期望调用的函数>`。
> 例如这次就是靠 `nm -C build/imgui_overlay.o | grep GlyphRanges` 看到
> `U ImFontAtlas::GetGlyphRangesChineseFull()` 才确认分支真的切过去了。

---

## 六、热重载

### 卸载顺序不能乱

见 [architecture.md](architecture.md) 的卸载流程。两个最容易漏的：

- **`RemoveAllDetours()` 必须在镜像释放前** —— 跳板字节写在 IL2CPP 模块里，不还原的话下次装钩子会装在已改写的字节上，返回 true 但装出残缺钩子。连续几次后命中数会一路掉到 0。
- **`UnloadToolDll()` 必须在 `UiShutdown()` 之后** —— 那时渲染线程已停止调 `Tool_Draw`，工具 DLL 的代码不在任何调用栈上。不卸载的话下次 `LoadLibraryW` 同一路径直接返回旧句柄，新 DLL 内容不生效。

### 使用方自己起的线程必须在镜像释放前退出

线程跑在我们的镜像里，镜像被 `VirtualFreeEx` 之后它们执行的是已释放的代码 → 崩。清理写在 `Mod_OnShutdown()`。

### 「延时待办」会永远不执行

如果你在某个钩子干了活之后就摘钩，那么任何「3 秒后复查一次」的延时逻辑都会因为钩子已摘掉而永不执行。需要延时就必须显式阻止摘钩。

---

## 七、日志 / 排查

### 被 hook 线程里，日志的**最后一条**就是崩点前最后执行的语句

这是最有效的定位手段。排查时在可疑位置前后密集插日志。

### 诊断代码「先落盘再冒险」

纯算术、不碰内存的日志放在最前面。崩了之后至少能看到它执行过。

### 「删了文件就不写了」不成立

日志是每行 `_wfopen(..., "a")` + `fclose`。**看不到某条日志 = 那条语句真的没执行**，不是文件被删了或者缓冲没刷。

### 两边日志对照

游戏自己的日志（`%USERPROFILE%\AppData\LocalLow\<厂商>\<游戏>\Player.log`）会打印托管侧的异常与完整栈。我们的日志说「我调了什么」，游戏的日志说「游戏怎么反应」—— 排查必须两边对着看。

---

## 八、进程退出

### 游戏自己退出（点 X）会卡死

**症状**：注入后游戏一切正常，点窗口右上角 X（或 Alt+F4）→ 游戏卡住不退出，只能在任务管理器强制结束进程。

**根因**：本模块走手动映射，**不在进程的 PEB 模块列表里**。游戏自己退出走的是
`ExitProcess` → `LdrShutdownProcess`，它：

1. **先杀掉除主线程外的所有线程** —— 我们起的 Worker 线程、鼠标钩子 pump 线程、
   退出看门狗线程，全都在被砍之列；
2. 再**只通知「加载器登记过的」模块**做 `DLL_PROCESS_DETACH` —— 我们这份镜像不在名单里，
   一个退出通知都收不到。

于是退出时我们还留着这些东西，全都指向一份「运行中」的镜像：

| 残留 | 为什么危险 |
| --- | --- |
| 全局 `WH_MOUSE_LL` 钩子 | 低级钩子是**同步回调**：系统输入线程收到鼠标事件后，要等钩子线程把回调跑完才继续投递。而钩子线程马上会被 `LdrShutdownProcess` 砍掉。**点 X 这个动作本身就是一次鼠标事件**，等于把系统输入线程和我们的死线程绑在一起 |
| `dxgi` 共享 vtable[8]/[13] 补丁 | 是**全局**的（所有 swapchain 实例共用一张表）。退出过程中游戏/系统还要调 Present，就会进我们的镜像，去碰一个正在被拆掉的 imgui / D3D 状态 |
| `il2cpp_class_from_name` 等函数体 detour | 写在 `GameAssembly` 的代码页里，镜像不在模块列表 ⇒ 没人替我们还原 |
| 常驻线程 + 我们 AddRef 的 D3D 引用 | 手动映射的镜像没有 CRT 退出路径，线程与 COM 引用都没人回收 |

**修法**（本模板已实现，见 `src/exit_guard.cpp`）：在游戏开始拆自己**之前**主动拆干净。

```
点 X ──▶ WM_CLOSE ──▶ 我们的窗口过程（子类化）──▶ 触发
                                                  │
                          看门狗线程每 100ms 查 IsWindow(hwnd)  ──┘（兜底）
                                                  │
                                     独立清理线程（不占游戏消息线程）
                                                  │
        ┌─────────────────────────────────────────┴──────────────────────┐
        │ ① UiExitFastPath()  卸全局鼠标钩子 + 还原 dxgi vtable（快、幂等）  │
        │ ② ModUnload()       停 Worker / 还原 detour / 用户清理 / 卸工具 DLL│
        └────────────────────────────────────────────────────────────────┘
```

三个要点，缺一不可：

- **必须在独立线程上清理**。触发点（窗口过程）跑在游戏消息线程上 —— 那是游戏退出的关键
  路径，占着它做清理等于换一种方式卡死。
- **必须先卸全局钩子、先还原 vtable**。这两样是"外部可见"的状态；剩下那些等待
  （停线程最多 3s、停鼠标 pump 最多 1s）可以慢慢来。
- **不能等渲染线程**。`UiShutdown()` 是为热重载设计的，会等渲染线程最多 3 秒；游戏退出时
  渲染线程可能已经不 Present 了，等不到。所以退出路径走 `UiExitFastPath()`。

### `UiShutdown()` 超时兜底里必须自己还原 vtable

这是"点 X 卡死"的**第二个独立成因**，与上面无关：

`UiShutdown()` 等渲染线程 3 秒；等不到时会直接 `ReleaseOwnedResources()`。而
`ReleaseOwnedResources()` 会把 `g_origPresent` 置空 —— 此时 vtable[8] 还指着 `HookPresent`，
于是之后**每一帧 Present 都退化成「返回 S_OK 但不真的 Present」**，画面冻结、窗口无响应，
看症状和上一节一模一样，但根因完全不同。

正确做法：超时分支里先 `RestoreVtables()` 再释放（模板已改）。

### 热重载用的快速退场路径不能读到空指针

`HookPresent` 末尾 `return g_origPresent(...)` 读的是**全局**变量，不是栈上的副本。
注入器线程可能刚把它清掉，而这个帧已经越过了开头的快速路径判断 —— 直接调空指针就是渲染线程
当场崩。结尾处也要判空（模板已改），代价只是丢一帧。

---

## 九、界面与输入

### 字形：`GetGlyphRangesChineseSimplifiedCommon` 不是「简体中文全覆盖」

名字很有迷惑性，它其实只含 **2500 个最常用字**。像 **渲、帧、逻、辑** 都不在里面：

```
面板「渲染帧数」→ "?染?数"
面板「在游戏逻辑线程打日志」→ "在游戏??线程打日志"
```

而且 `GetGlyphRangesChineseFull()` 也**不含 U+2460 圈号**（①②③），所以还得自己拼符号区间。

实测（msyh.ttc，18px）：

| 字形范围 | 字形数 | 图集 | RGBA | 构建耗时 |
| --- | --- | --- | --- | --- |
| Common + 符号区 | 3299 | 1024×2048 | 8MB | 29ms |
| **Common + 符号 + CJK 整块(4E00–9FFF)** | 21791 | 4096×4096 | 64MB | 187ms |
| `ChineseFull` | 21628 | 4096×4096 | 64MB | 186ms |

模板默认用完整 CJK（`src/mod_config.h` 的 `MOD_FONT_FULL_CJK`），代价是 64MB 显存 +
一次性 ~190ms 构建（发生在第一次 Present，在 logo/加载画面上，感知不到）。显存紧张就改成 0。

顺带一个坑：拼 ranges 数组时**缓冲区要够大**。common 那份列表实测有 **5012 个 ImWchar
（2506 个区间对）**，用 256 的小数组拼接会静默回退成 common —— 表现是"改了代码但没生效"。

### 输入：Present 钩子里不能用依赖「线程输入队列」的 API

**症状**：输入框里按 Shift 打不出大写（Shift+A 出 `a`），Shift+`-` 出的也是 `-` 不是 `_`。

**根因**：`ToUnicode` 要靠 `lpKeyState` 数组判断 Shift 是否按下，而代码里用的是

```cpp
BYTE ks[256] = {};
GetKeyboardState(ks);          // ← 错
int n = ToUnicode(vk, 0, ks, wcs, 8, 0);
```

`GetKeyboardState` 返回的是**调用线程输入队列**的键盘状态 —— 只有该线程自己拥有焦点、
并且处理过对应键盘消息之后它才更新。而这段代码跑在 **Present 钩子（游戏渲染线程）** 里，
那个线程很可能既不持有窗口焦点也不处理键盘消息，于是拿到的是一份「什么都没按下」的状态
⇒ `ToUnicode` 看不到 Shift ⇒ **永远小写**。

**修法**：`GetAsyncKeyState` 是全局的、与线程无关，逐键取高位自己拼：

```cpp
BYTE ks[256] = {};
for (int v = 0; v < 256; ++v)
    if (GetAsyncKeyState(v) & 0x8000) ks[v] = 0x80;
if (s_caps) ks[VK_CAPITAL] |= 0x01;    // CapsLock 是"锁定"不是"按下"，低位表示开/关
int n = ToUnicode(vk, 0, ks, wcs, 8, 0);
```

**推广**：凡是「读取当前输入状态」的 API（`GetKeyboardState`、`GetKeyState`、
`GetMessage`、`PeekMessage`）都只对**拥有输入队列的那个线程**有意义。
Present 钩子不在消息循环里，这些一律不可信 —— 只用 `GetAsyncKeyState` / `GetCursorPos`
这类全局查询。CapsLock 的锁定态因此也得自己跟踪按下沿翻转（初值用 `GetKeyState` 猜一次）。

顺带：字符键**别维护白名单**。之前手写了"能打字的键"列表，漏了 `VK_OEM_PLUS`，
`+` 和 `=` 一直打不出来。全键扫描（0..255，靠 `< 0x20` 过滤控制字符）最省心，
每帧 256 次 `GetAsyncKeyState` 的开销可以忽略。


