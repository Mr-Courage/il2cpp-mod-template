# 注入器接口约定

本模板产出的是**手动映射载荷**，需要一个支持手动映射的注入器来投递。这份文档描述两边必须对齐的全部约定。

如果你已经有注入器，对照这张表改 `mod_config.h` 与 `mod_template.def` 即可。

---

## 一、载荷约束（注入器侧假设）

注入器可以放心地假设：

| 约束 | 值 | 校验方式 |
| --- | --- | --- |
| 架构 | x86-64 only | — |
| 静态 TLS | **无**（无 `.tls` 段、无 `__emutls_*`） | `objdump -p x.dll \| grep -c emutls` → 0 |
| 导入表 | 仅 `KERNEL32.dll` / `msvcrt.dll` / `SHELL32.dll` / `USER32.dll` | `objdump -p x.dll \| grep -i "DLL Name"` |
| 异常 / RTTI | 未启用（`-fno-exceptions -fno-rtti`） | — |
| 入口点 | `DllMainCRTStartup`（已导出，也可直接跳 PE 头里的 AddressOfEntryPoint） | — |
| 重定位 | 有（`-shared` 产物默认带 `.reloc`） | — |

**不要**为载荷远程 `LoadLibraryW` 任何非系统 DLL —— 那条路径在部分目标进程里不可靠（实测返回 NULL / err=87）。这也是 imgui DX11 后端被改成运行时 `GetProcAddress` 取 `D3DCompile` 的原因。

---

## 二、导出函数

`mod_template.def` 里精确导出以下符号（`--exclude-all-symbols` 会隐藏其它一切）。

### 主接口

| 导出 | 签名 | 说明 |
| --- | --- | --- |
| `ModClaimInstance` | `int ()` | **引导时由载荷自己调用**。返回 1 = 抢到（继续引导）；0 = 本进程已有一份镜像（放弃）。底层是带 PID 的命名互斥体 |
| `ModUnload` | `void ()` | 远程调用，执行完整卸载。可重复调用，第二次是 no-op |
| `ModIsLoaded` | `int ()` | 1 = 本镜像仍在工作 |
| `ModVersion` | `int ()` | 编译标记（`0x10001`） |
| `ModAbiVersion` | `int ()` | 接口版本（当前 1） |
| `ModLockName` | `const wchar_t* ()` | 返回互斥体名 `Local\il2cpp_mod_lock_<PID>`，供注入器确认实例存在 |
| `ModSelfBase` | `ULONG_PTR ()` | 本镜像基址 |
| `ModSetSelf` | `void (HMODULE, ULONG_PTR)` | 由载荷的 `DllMain` 自己调用，记录句柄与基址 |
| `UiShutdownCompleted` | `int ()` | 渲染线程侧是否已清理完（卸载流程轮询用） |
| `DllMainCRTStartup` | — | 入口点，方便按名字定位 |

### `Esp*` 兼容别名

`EspUnload` / `EspIsLoaded` / `EspVersion` / `EspClaimInstance` / `EspSetSelf` / `EspSelfBase` / `EspLockName` / `EspAbiVersion` —— 内部转发到上面的 `Mod*`。

> 原项目里 ESP 指 "external script payload"，与「透视挂」无关。保留别名是为了让老注入器不用改。想彻底去掉：删掉 `src/hot_reload.cpp` 末尾的转发区块，并同步 `mod_template.def`。

---

## 三、注入时序

### 首次注入

```
注入器
  1. 读 PE、分配 RWX/RX 映像、搬节、修重定位
  2. 修导入表（只有 4 个系统 DLL，都在目标进程里已加载）
  3. 跳入口点（ APC / CreateRemoteThread / 新线程 ）
        │
        ▼
  载荷 DllMain → ModClaimInstance() → CreateThread(Worker)
```

★ **时机很重要**：目标进程**已经跑了一段时间**的话，反作弊驱动可能已武装，此时直接对 `--pid` 注入会返回 **err=87**。要么抢在游戏启动前注入（`--watch` / `--launch` 模式，在驱动加载前落地），要么走下面的热重载路径。

### 热重载（`--replace`）

```
注入器
  1. 扫目标进程内存，找 "ModUnload"（或 "EspUnload"）这个导出名字符串
     → 反推出旧镜像的基址
     （不用共享内存传基址：CreateFileMapping(INVALID_HANDLE_VALUE) 在目标
       进程里返回 err=1455）
  2. 远程调用 旧镜像的 ModUnload()
       → Worker 停、detour 还原、user 清理、overlay 清理、工具 DLL 卸载、释放互斥体
  3. 等 UiShutdownCompleted() == 1（渲染线程侧清理完成）
  4. VirtualFreeEx 旧镜像
  5. 映射新镜像、跳入口点
```

`ModUnload()` 内部会等渲染线程完成清理（最多 3 秒）。如果超时，`UiShutdownCompleted()` 会一直是 0 —— 通常意味着游戏窗口不可见（Present 不跑），这种情况下 overlay 资源会泄漏，但剩下的清理仍然会做完。

---

## 四、为什么卸载流程这么长

直接 `VirtualFreeEx` 掉旧镜像会导致四样东西同时炸：

1. **Worker 线程重复** —— 两个 Worker 同时跑引导、同时往同一个 imgui context 塞数据 → 数据竞争
2. **`ImGui::CreateContext()` 重复调用** —— imgui 是全局单例 `GImGui`，第二次调用会覆盖指针，旧 context 及其 GPU 资源立刻泄漏
3. **vtable[8] 槽被覆盖** —— 它已经是「第一次那个 DLL」的 HookPresent 地址，第二次注入会改到新地址 → 第一次的 Present hook 立刻变野指针
4. **trampoline 池泄漏** —— 每次 `DetourInstall` 都 `VirtualAlloc` 一块新的

所以 `ModUnload()` 必须完整逆序清理。见 [architecture.md](architecture.md) 的卸载顺序。

---

## 五、目录约定

载荷会按以下顺序找「模块目录」，日志、配置文件 `mod.ini`、工具 DLL 都在这个目录里：

```
1. 模块所在目录（GetModuleFileNameW 成功时）
   ★ 手动映射时这一步通常失败（载荷不在 PEB 模块列表里）
2. 环境变量 IL2CPP_MOD_DIR          ← 推荐：注入时用这个显式指定
3. 进程当前工作目录（CWD）
4. %TEMP%
```

注入器可以在注入前设置 `IL2CPP_MOD_DIR`，或者在创建远程线程前把目标进程的 CWD 切到载荷所在目录。

产物文件：

| 文件 | 位置 | 说明 |
| --- | --- | --- |
| `il2cpp_mod.log` | 模块目录（或上面的降级路径） | 主日志，append 模式，每行一次 open/close |
| `il2cpp_mod_unload.log` | `%TEMP%` | 卸载阶段日志（那时主日志可能已不可用） |
| `mod.ini` | 模块目录 | 可选配置，见 `config/mod.ini.example` |
| `il2cpp_tool.dll` | 模块目录 | 可选工具 DLL，见 `MOD_TOOL_DLL` |

---

## 六、最小注入器需求清单

如果你要自己写一个：

- [ ] 手动映射：搬节 + 修重定位 + 修导入表（只需处理 4 个系统 DLL）
- [ ] 入口点执行：APC 或远程线程，传 `DLL_PROCESS_ATTACH`
- [ ] 扫内存找 `ModUnload` 导出名 → 反推旧镜像基址
- [ ] 远程调用 `ModUnload()`，轮询 `UiShutdownCompleted()`
- [ ] `VirtualFreeEx` 后重新映射
- [ ] 支持在创建远程线程前设置 `IL2CPP_MOD_DIR` / 切换 CWD
- [ ] （可选）`--watch` / `--launch` 模式：在游戏进程刚起来、反作弊驱动尚未武装时注入
