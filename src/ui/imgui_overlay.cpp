// imgui_overlay.cpp — ImGui 界面层（D3D11 Present hook），手动映射场景专用
// ---------------------------------------------------------------------------
// 与 port/win_injector/esp_dll/esp_overlay.cpp 的关系：
//   那份是给「代理 DLL 注入」用的 —— 靠替换游戏目录的 d3d11.dll，Loader 原生加载，
//   DllMain 一跑就等着游戏调 D3D11 创建设备，设备/swapchain 天然可得。
//   但那条路已被废弃（劫持 DLL 会被检测），所以把 overlay 移植到**手动映射**场景。
//
// 取 device 的思路（与代理版的关键差异）：
//   代理版：DllMain 里 hook d3d11 导出 → 等游戏创建设备 → 拿到 swapchain。
//   手动映射版：注入时机由我们决定，可能是游戏已跑起来 30 秒，device 早就创建完了，
//   等不到也拦不到。所以换一条不依赖时序的路：
//     1) 自己用**系统 d3d11.dll** 的 D3D11CreateDeviceAndSwapChain 建一个 dummy 链；
//     2) 取 dummy 链 vtable[8]（Present 槽）。
//        ★ 关键：D3D11 的 Present 实现在 dxgi.dll 里，是**全进程唯一**的一份代码。
//          不管是 dummy 链还是游戏自己的链，vtable[8] 指向同一个函数地址。
//     3) 改**那张共享 vtable 的 [8] 槽**为我们的钩子 —— 全局生效，
//        游戏自己的 Present 也进钩子，而原函数代码一个字都没动（重要！见下方说明 1）；
//     4) 钩子里 sc->GetDevice() 拿到**游戏真实的** device + sc->GetDesc() 拿游戏窗口。
//   这样注入早晚期都能工作。
//
// 实现说明（关键决策都在这，别改回去）：
//
// 1) **Present 只改 vtable[8] 槽，绝不改函数体**。
//    原因：dxgi!Present 的 prologue 是
//        mov [rsp+0x08], rbx ; mov [rsp+0x10], rsi ; push rbp ; push rdi ; ...
//    钩子末尾写 `return g_origPresent(...)` 编译成 CALL，会把 rsp 压低 8，于是 trampoline
//    里克隆的 `mov [rsp+0x08], rbx` 实际写到 (R-8)+8 = R，**正好覆盖 CALL 压进去的返回地址**；
//    两个 push 也整体错位 8 字节。函数最后 ret 弹出 rbx 值 -> 跳到随机地址 -> 卡住/闪退。
//    （实测日志正好断在 "绘制完成，调原 Present" 之后。）
//    改 vtable 槽后，g_origPresent 指向**完全没被动过**的函数，return 是普通 call，栈天然正确。
//    dxgi 所有 swapchain 实例共用同一张 vtable（在 dxgi.dll .rdata），改一处全局生效。
//
// 2) **ResizeBuffers 同样走 vtable patch**（它本来就不能做函数体 detour）。
//
// 3) 渲染前显式把 back buffer 绑成 RTV：游戏主画面阶段会把 RTV 换成中间渲染目标，
//    不修的话 overlay 只在 logo 页可见；画完恢复原绑定（含 DSV）。
//
// 4) 每 600 帧校验 vtable[8] 槽是否仍指向我们，反作弊改回就留证。
//
// 输入（il2cpp_core 那套依赖在别的文件里）：
//   LogUi()      —— 打日志（同时进文件与界面环形缓冲）
//   EscPollMouse —— 每帧轮询原始鼠标输入（Present 不在消息循环里，imgui 收不到 WM_MOUSEMOVE）
// ---------------------------------------------------------------------------
#include <windows.h>
// MinGW 的 dxgi.h 只到 IDXGIFactory1；v2+ 的定义要显式包含 dxgi1_2.h。
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdint>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

// 自己的对外接口（加进来是为了让编译器校验签名与 ui_api.h 一致 —— 之前各写各的，
// 少参数/参数类型不一致都发现不了）
#include "ui/ui_api.h"
#include "exit_guard.h"

// ★★ 必须包含 mod_config.h ★★
//   踩过的坑（2026-10-14，白测了一轮）：本文件用了 MOD_FONT_FULL_CJK 做字形范围开关，
//   但当时**没包含 mod_config.h**，宏自然未定义。而 `#if 未定义宏` 在 C 里是**合法**的 ——
//   求值为 0，于是静默走了 `#else` 的老分支：改了代码、编译零告警、行为一点没变。
//   所以下面加一道 #error 保险：只要有人再用本项目之外的 MOD_* 宏而忘了 include，
//   直接编不过，不会再出现"静默无效"。
#include "mod_config.h"
#ifndef MOD_FONT_FULL_CJK
#  error "imgui_overlay.cpp: MOD_FONT_FULL_CJK 未定义 —— mod_config.h 没被包含进来"
#endif

// 由主文件提供
extern void LogRaw(const char *fmt, ...);
static void LogUi(const char *fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    LogRaw("%s", buf);
}

typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *ResizeBuffersFn)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
enum { PRESENT_VTABLE_INDEX = 8, RESIZEBUFFERS_VTABLE_INDEX = 13 };

static volatile bool g_uiReady = false;      // imgui 已初始化
static PresentFn g_origPresent = nullptr;
static ResizeBuffersFn g_origResize = nullptr;
static ID3D11Device *g_uiDev = nullptr;      // 保留引用，防止 game 侧 d3d 提前卸载
static ID3D11DeviceContext *g_uiCtx = nullptr;
static IDXGISwapChain *g_uiSc = nullptr;     // 当前绑定（游戏自己的链）
static void *g_patchedPresentAddr = nullptr;
static void **g_presentVtable = nullptr;   // 被改的那张 vtable（存活校验用它）
static void **g_resizeVtable = nullptr;    // ResizeBuffers 被改的那张 vtable（卸载时还原）
static bool  g_shutdownDone = false;      // 卸载幂等标记

// ---- 跨线程卸载请求 --------------------------------------------------------
//
// EspUnload() 是被 map_dll 的远程调用在**注入器线程**上执行的，而 imgui context
// 正被**游戏渲染线程**每帧读写。在注入器线程里直接 DestroyContext 就是数据竞争
// -> 实测必崩（日志停在 "begin unload" 没有 end，目标进程消失）。
//
// 解法：**卸载动作全部推迟到 Present 钩子里执行**，也就是只在游戏渲染线程上碰 imgui。
// 注入器线程只置一个标志然后返回；渲染线程看到标志后：
//   1) 销毁 imgui context（仍在钩子内，安全）
//   2) 释放自有资源（D3D state / 纹理等）
//   3) **最后**才还原 vtable[8]（此后本镜像不再被游戏调用），再置完成标志
//
// ★ 顺序不能反：若先还原 vtable，下一帧就没人进钩子了，后续清理永远等不到帧
//   —— 那会导致每热重载一次泄漏一整套 GPU 资源（详见下方函数注释）。
static volatile LONG g_unloadRequested = 0;   // 注入器线程置 1
static volatile LONG g_unloadFrameCount = 0;   // 还原后已过的帧数（用于等待）
static ULONGLONG   g_unloadDoneTick = 0;      // 全部清理完成的时刻

// 原始 vtable 槽值（被改之前的），还原时用它而不是 g_origPresent ——
// 后者在首次还原后可能已被别的实例改动。
static void* g_origPresentSlot = nullptr;
static void* g_origResizeSlot = nullptr;
static ID3D11RenderTargetView *g_backRTV = nullptr;
static bool g_panelVisible = false;          // 面板显隐
static unsigned long g_frame = 0;
static HWND g_gameHwnd = nullptr;
static ULONGLONG g_lastFrameTime = 0;   // 算 DeltaTime 用（win32 backend 被砍了，得自己来）

// 光标只在面板上显示
static bool  g_cursorOverPanel = false;
// 面板矩形（屏幕坐标），供光标命中测试与"吃掉点击"使用
static float g_panelScreenX0 = 0, g_panelScreenY0 = 0, g_panelScreenX1 = 0, g_panelScreenY1 = 0;

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain *, UINT, UINT);
// 卸载时序驱动：在 HookPresent 开头调用（必须早于任何 imgui 操作）
static void PumpUnloadIfRequested();

// =====================================================================
//  轻量 x64 函数体 detour（移植自 esp_overlay.cpp，已验证）
//  只改函数体代码，不改 IDXGIFactory / IDXGISwapChain 全局共享 vtable，
//  因此不触发反作弊对"全局 vtable 篡改"的检测。
// =====================================================================
static int ModrmLen(const BYTE *p, int i) {
    BYTE m = p[i]; int mod = m >> 6, rm = m & 7, end = i + 1;
    if (mod != 3 && rm == 4) end++;                 // SIB
    if (mod == 1) end += 1;
    else if (mod == 2) end += 4;
    else if (mod == 0 && rm == 5) end += 4;          // RIP-rel disp32
    return end;
}

static int InsnLen(const BYTE *c, int i, bool *rel) {
    const int LIM = 15;
    int j = i;
    *rel = false;
    while (j < i + LIM) {
        BYTE pr = c[j];
        if (pr == 0xF0 || pr == 0xF2 || pr == 0xF3 || pr == 0x2E || pr == 0x36 ||
            pr == 0x3E || pr == 0x26 || pr == 0x64 || pr == 0x65 || pr == 0x66 || pr == 0x67) { j++; continue; }
        break;
    }
    bool rexW = false, has66 = false;
    if (j < i + LIM && c[j] >= 0x40 && c[j] <= 0x4F) { if (c[j] & 8) rexW = true; j++; }
    for (int k = i; k < j; ++k) if (c[k] == 0x66) has66 = true;
    if (j >= i + LIM) return -1;
    int o = j; BYTE b = c[o];
    if (b == 0xE8 || b == 0xE9) { *rel = true; return o + 5 - i; }
    if (b == 0xEB) { *rel = true; return o + 2 - i; }
    if (b >= 0x70 && b <= 0x7F) { *rel = true; return o + 2 - i; }
    if (b == 0x0F) { if (c[o + 1] >= 0x80 && c[o + 1] <= 0x8F) { *rel = true; return o + 6 - i; } return -1; }
    switch (b) {
        case 0xC3: case 0xC9: case 0x90: case 0xF4: case 0xF8: case 0xF9: case 0xCC: case 0xF5:
        case 0x98: case 0x99: case 0x9B: case 0x9E: case 0x9C: case 0x9D: return o + 1 - i;
        case 0xC2: case 0xCA: return o + 3 - i;
        case 0x6A: return o + 2 - i;
        case 0x68: return o + 1 + (has66 ? 2 : 4) - i;
        case 0x6B: return (ModrmLen(c, o + 1) - i) + 1;
        case 0x69: return (ModrmLen(c, o + 1) - i) + (has66 ? 2 : 4);
        case 0x80: case 0x82: case 0x83: case 0xC0: case 0xC1: case 0xC6:
            return (ModrmLen(c, o + 1) - i) + 1;
        case 0x81: case 0xC7:
            return (ModrmLen(c, o + 1) - i) + (has66 ? 2 : 4);
        default: break;
    }
    if (b >= 0x50 && b <= 0x5F) return o + 1 - i;        // push/pop reg
    if (b >= 0x91 && b <= 0x96) return o + 1 - i;        // xchg reg,rax
    if (b >= 0xB8 && b <= 0xBF) return o + 1 + (rexW ? 8 : (has66 ? 2 : 4)) - i;
    switch (b) {
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E:
        case 0x8F: case 0x63: case 0xFF: case 0x01: case 0x03: case 0x29: case 0x2B:
            return ModrmLen(c, o + 1) - i;
        default: break;
    }
    return -1;
}

// 指令分类：返回 rsp 净变化（push=-8, pop=+8, sub rsp,+imm, add rsp,-imm, 其他 0）；
// 不可克隆的（call/ret/call-indirect）返回 INT_MIN 哨兵。
static int ClassifyInsn(const BYTE *c, int i, int *lenOut) {
    int k = i;
    for (;;) {
        BYTE p = c[k];
        if (p == 0xF0 || p == 0xF2 || p == 0xF3 || p == 0x2E || p == 0x36 ||
            p == 0x3E || p == 0x26 || p == 0x64 || p == 0x65 || p == 0x66 || p == 0x67) { k++; continue; }
        break;
    }
    if (c[k] >= 0x40 && c[k] <= 0x4F) k++;          // REX
    BYTE op = c[k];
    if (op >= 0x50 && op <= 0x57) { *lenOut = k + 1 - i; return -8; }   // push
    if (op >= 0x58 && op <= 0x5F) { *lenOut = k + 1 - i; return 8; }    // pop
    if (op == 0xE8) { *lenOut = k + 5 - i; return 0; }  // call rel32（克隆时重定位）
    if (op == 0xC3 || op == 0xC2 || op == 0xCA || op == 0xCB) return INT_MIN;
    if (op == 0xFF && (((c[k + 1] >> 3) & 7) == 2)) return INT_MIN;     // call-indirect
    if (op == 0x83 || op == 0x81) {                                     // add/sub rsp,imm
        BYTE m = c[k + 1]; int reg = (m >> 3) & 7;
        if (reg == 0 || reg == 5) {                                     // add(0) / sub(5)
            int mod = m >> 6, rm = m & 7;
            bool isRsp = false;
            if (rm == 4) {
                if (mod == 3) isRsp = true;                             // mod=3,rm=4 -> rsp 寄存器（无 SIB）
                else { BYTE sib = c[k + 2]; isRsp = ((sib & 7) == 4); } // SIB base=rsp
            }
            if (isRsp) {
                bool is32 = (op == 0x81);
                *lenOut = k + 1 + (is32 ? 5 : 2) - i;
                int imm = is32 ? *(int *)(c + k + 2) : (signed char)c[k + 2];
                return (reg == 5) ? -imm : imm;                         // sub rsp -> rsp 减少
            }
        }
    }
    *lenOut = 0;
    return 0;
}

static void *g_trampPool = nullptr;
static int  g_trampUsed = 0;
#define TRAMP_POOL_SIZE 0x4000
#define TRAMP_MAX (TRAMP_POOL_SIZE - 512)

static bool DetourInstall(void *target, void *detour, void **outTramp) {
    BYTE *src = (BYTE *)target;
    int i = 0, netRsp = 0, nInsn = 0;

    // ---- 逐条指令解析：任何一步都不许"静默失败"，否则调用方只能看到"卡住了" ----
    LogUi("[ui] DetourInstall: target=%p detour=%p", target, detour);
    while (i < 12) {
        bool rel = false;
        int len = InsnLen(src, i, &rel);
        LogUi("[ui]   insn @%d len=%d rel=%d bytes=%02X %02X %02X %02X",
              i, len, (int)rel, src[i], src[i + 1], src[i + 2], src[i + 3]);
        if (len <= 0 || i + len > 12) { LogUi("[ui] undecodable @%d; skip", i); return false; }
        if (rel) { LogUi("[ui] rel insn @%d; skip (no reloc)", i); return false; }
        int l2 = 0;
        int cl = ClassifyInsn(src, i, &l2);
        if (cl == INT_MIN) { LogUi("[ui] call/ret @%d; skip", i); return false; }
        netRsp += cl; i += len; nInsn++;
    }
    if (nInsn < 2) { LogUi("[ui] prologue too short (%d insns); skip", nInsn); return false; }
    int kLen = i;
    LogUi("[ui]   parsed %d insns, kLen=%d netRsp=%d", nInsn, kLen, netRsp);

    if (!g_trampPool) {
        g_trampPool = VirtualAlloc(NULL, TRAMP_POOL_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_trampPool) return false;
        g_trampUsed = 0;
    }
    if (g_trampUsed + kLen + 32 > TRAMP_MAX) { LogUi("[ui] trampoline pool full"); return false; }
    BYTE *tramp = (BYTE *)g_trampPool + g_trampUsed;

    int off = 0;
    for (int p = 0; p < kLen;) {
        int len2 = 0;
        ClassifyInsn(src, p, &len2);      // 返回值这里不用，只要长度
        if (len2 <= 0) break;
        memcpy(tramp + off, src + p, len2);
        if (src[p] == 0xE8) {   // call rel32 -> 重定位到新位置
            int rel = *(int *)(src + p + 1);
            *(int *)(tramp + off + 1) = rel + (int)((src + p + 5) - (tramp + off + 5));
        }
        off += len2;
        p += len2;
    }
    // 结尾：mov rax, <src+kLen>; jmp rax
    tramp[off] = 0x48; tramp[off + 1] = 0xB8; *(UINT64 *)(tramp + off + 2) = (UINT64)(src + kLen);
    tramp[off + 10] = 0xFF; tramp[off + 11] = 0xE0;
    g_trampUsed += off + 12;
    LogUi("[ui]   trampoline built @%p (%d bytes), now patching target", tramp, off + 12);

    // ---- 改写目标函数体 ----
    // 这是整个流程最容易出事的几步：VirtualProtect 可能失败（ACE 保护/页属性异常），
    // 写指令页可能触发访问冲突。**每一步都留证据**，否则失败时外部只能看到"卡住了"。
    DWORD old = 0;
    BOOL vp = VirtualProtect(src, kLen, PAGE_EXECUTE_READWRITE, &old);
    if (!vp) {
        LogUi("[ui] FAILED: VirtualProtect(%p, %d) err=%lu -- 目标页不可写（可能被保护驱动锁住）",
              src, (int)kLen, (unsigned long)GetLastError());
        return false;
    }
    LogUi("[ui]   VirtualProtect ok (oldProtect=0x%X), writing 12-byte jump",
          (unsigned)old);

    // 先把原字节落到 trampoline 之后再改写；写之前确认目标确实可读（避免 IsBadReadPtr 误判）
    if (IsBadReadPtr(src, kLen)) {
        LogUi("[ui] FAILED: target %p became unreadable right before patch", src);
        VirtualProtect(src, kLen, old, &old);
        return false;
    }

    src[0] = 0x48; src[1] = 0xB8; *(UINT64 *)(src + 2) = (UINT64)detour;
    src[10] = 0xFF; src[11] = 0xE0;
    for (int t = 12; t < kLen; ++t) src[t] = 0x90;   // NOP 填充（已入 trampoline）

    BOOL vp2 = VirtualProtect(src, kLen, old, &old);
    LogUi("[ui]   patch written, VirtualProtect restore %s", vp2 ? "ok" : "FAILED");
    if (!vp2)
        LogUi("[ui] WARN: 页属性没恢复（可能残留 RWX），err=%lu", (unsigned long)GetLastError());
    FlushInstructionCache(GetCurrentProcess(), src, kLen);

    LogUi("[ui] detour kLen=%d insns=%d netRsp=%d tramp=%p  <-- INSTALLED",
          kLen, nInsn, netRsp, tramp);
    *outTramp = (void *)tramp;
    return true;
}

// =====================================================================
//  dummy swapchain：只为拿全局唯一的 Present 代码地址
// =====================================================================
static IDXGISwapChain *g_dummySc = nullptr;
// dummy 链的 device/context 也要留引用（原来只 AddRef 不保存 ⇒ 永远放不掉，
// 退出时 DXGI 会对着一份没人能释放的 swapchain 收尾）。
static ID3D11Device        *g_dummyDev = nullptr;
static ID3D11DeviceContext *g_dummyCtx = nullptr;

static void GrabPresentAddress()
{
    if (g_origPresent) return;   // 已装好

    // 显式加载系统 d3d11：手动映射的载荷不保证进程里已加载它；
    // 用绝对路径避免命中游戏目录（万一以后游戏自带改名版）。
    HMODULE d3d = LoadLibraryW(L"C:\\Windows\\System32\\d3d11.dll");
    if (!d3d) { LogUi("[ui] LoadLibrary System32\\d3d11.dll failed: %lu", GetLastError()); return; }

    // 用 MinGW 头里的真实签名（注意：宽度/高度/格式都藏在 DXGI_SWAP_CHAIN_DESC 里，
    // 且额外返回 device / immediate_context —— 正好省一次 GetDevice）。
    typedef HRESULT(WINAPI *CreateDevAndSCFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                               const D3D_FEATURE_LEVEL *, UINT, UINT,
                                               const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
                                               ID3D11Device **, D3D_FEATURE_LEVEL *,
                                               ID3D11DeviceContext **);
    CreateDevAndSCFn fn = (CreateDevAndSCFn)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain");
    if (!fn) { LogUi("[ui] D3D11CreateDeviceAndSwapChain not found"); return; }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = GetDesktopWindow();   // 必须给合法 hwnd；永不 Present，无副作用
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Windowed = TRUE;

    IDXGISwapChain *sc = nullptr;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *devCtx = nullptr;
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;
    // pAdapter 传 NULL：D3D11 自己按序枚举硬件设备并挑一个。
    HRESULT hr = fn(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                    nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                    &dev, &obtained, &devCtx);
    if (FAILED(hr) || !sc) {
        // 硬件设备失败（无 GPU / 受限环境）就退到 WARP 软件设备，地址拿的一样有效。
        LogUi("[ui] dummy hardware device failed hr=0x%08X, trying WARP", (unsigned)hr);
        if (sc) { sc->Release(); sc = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
        if (devCtx) { devCtx->Release(); devCtx = nullptr; }
        hr = fn(nullptr, D3D_DRIVER_TYPE_WARP, NULL, 0,
                nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                &dev, &obtained, &devCtx);
    }
    if (FAILED(hr) || !sc) {
        LogUi("[ui] dummy D3D11CreateDeviceAndSwapChain failed hr=0x%08X", (unsigned)hr);
        return;
    }

    void **vtbl = *reinterpret_cast<void ***>(sc);
    if (!vtbl || IsBadReadPtr(vtbl, sizeof(void *) * 16)) {
        LogUi("[ui] dummy vtable unreadable");
        sc->Release();
        return;
    }
    void *present = vtbl[PRESENT_VTABLE_INDEX];
    LogUi("[ui] dummy swapchain ok: vtable=%p PresentFn=%p", vtbl, present);

    // =====================================================================
    //  只改 **vtable[8] 这个槽位**，不改 Present 的函数体。
    //
    //  为什么不改函数体（这是踩过的大坑，别改回去）：
    //    dxgi!Present 的 prologue 是
    //        mov [rsp+0x08], rbx ; mov [rsp+0x10], rsi ; push rbp ; push rdi ; ...
    //    钩子末尾若写 `return g_origPresent(...)`，编译成 CALL 会把 rsp 压低 8，
    //    于是 trampoline 里克隆的 `mov [rsp+0x08], rbx` 实际写到 (R-8)+8 = R，
    //    **正好覆盖 CALL 刚压进去的返回地址**；两个 push 也整体错位 8 字节。
    //    函数最后 ret 弹出的是 rbx 值 -> 跳到随机地址 -> 卡住/闪退。
    //    实测日志正好断在 "f1: 绘制完成，调原 Present" 之后。
    //
    //  改 vtable 的好处：
    //    * g_origPresent 指向**完全没被动过**的 Present 代码，
    //      `return g_origPresent(...)` 是普通 call，栈天然正确；
    //    * dxgi 里所有 swapchain 实例共用同一张 vtable（都在 dxgi.dll 的 .rdata），
    //      改一处全局生效，游戏切主链也照样进钩子。
    // 代价：改 vtable 是可被完整性校验发现的写操作。
    //    原 esp_overlay.cpp 的注释记录过这个位置「已被 ACE 放行」，所以采用。
    // =====================================================================
    g_origPresent = (PresentFn)present;          // 原函数地址，代码未被改动
    g_patchedPresentAddr = present;              // 仅供存活校验（校验 vtable 槽而非函数体）
    g_presentVtable = vtbl;
    g_origPresentSlot = present;                 // 备份原始槽值，还原时用
    g_origResizeSlot = nullptr;

    DWORD old = 0;
    BOOL vp = VirtualProtect(&vtbl[PRESENT_VTABLE_INDEX], sizeof(void *), PAGE_READWRITE, &old);
    if (!vp) {
        LogUi("[ui] FAILED: VirtualProtect(vtable slot) err=%lu", (unsigned long)GetLastError());
    } else {
        void *prev = vtbl[PRESENT_VTABLE_INDEX];
        vtbl[PRESENT_VTABLE_INDEX] = (void *)&HookPresent;
        VirtualProtect(&vtbl[PRESENT_VTABLE_INDEX], sizeof(void *), old, &old);
        FlushInstructionCache(GetCurrentProcess(), &vtbl[PRESENT_VTABLE_INDEX], sizeof(void *));
        LogUi("[ui] Present **vtable slot** patched: %p -> %p (原函数体未改动: %p)",
              prev, (void *)&HookPresent, present);
    }

    // dummy 链本身只是探测工具，保留引用计数防止它被销毁后 vtable 悬空。
    // ★ 引用要**存下来**，卸载/退出时才能逐个 Release —— 只 AddRef 不保存等于泄漏，
    //   而 DXGI 收尾时会对着一个没人能释放的 swapchain 等，是"退出卡死"的嫌疑之一。
    g_dummySc = sc;
    g_dummyDev = dev;
    g_dummyCtx = devCtx;
    if (dev) dev->AddRef();
    if (devCtx) devCtx->AddRef();
}

// =====================================================================
//  Present 钩子
// =====================================================================
static void ReleaseBackRTV() {
    if (g_backRTV) { g_backRTV->Release(); g_backRTV = nullptr; }
}

static ID3D11RenderTargetView *EnsureBackRTV(IDXGISwapChain *sc) {
    if (g_backRTV) return g_backRTV;
    if (!g_uiDev) return nullptr;               // 还没绑定成功时不该走到这
    ID3D11Texture2D *bb = nullptr;
    HRESULT hrGet = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    if (FAILED(hrGet) || !bb) {
        LogUi("[ui] EnsureBackRTV: GetBuffer failed hr=0x%08X", (unsigned)hrGet);
        return nullptr;
    }
    HRESULT hr = g_uiDev->CreateRenderTargetView(bb, nullptr, &g_backRTV);
    bb->Release();
    if (FAILED(hr) || !g_backRTV) {
        LogUi("[ui] EnsureBackRTV: CreateRenderTargetView failed hr=0x%08X", (unsigned)hr);
        g_backRTV = nullptr;
        return nullptr;
    }
    return g_backRTV;
}

static HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain *sc, UINT bufCount,
                                                   UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    LogUi("[ui] ResizeBuffers %ux%u bufs=%u sc=%p", w, h, bufCount, (void *)sc);
    ReleaseBackRTV();
    if (g_uiReady) ImGui_ImplDX11_InvalidateDeviceObjects();
    // 卸载竞态保护（同 HookPresent 的快速路径）：g_origResize 可能已被注入器线程清掉
    return g_origResize ? g_origResize(sc, bufCount, w, h, fmt, flags) : S_OK;
}

static void CheckPresentHookAlive() {
    // 校验 **vtable[8] 槽**（不再改函数体，所以校验对象也换了）。
    // 反作弊可能把槽改回去，那样钩子就静默失效了。
    if (!g_presentVtable || IsBadReadPtr(g_presentVtable, sizeof(void *) * 16)) return;
    void *cur = g_presentVtable[PRESENT_VTABLE_INDEX];
    if (cur != (void *)&HookPresent) {
        LogUi("[ui] WARN: Present hook LOST at frame=%lu - vtable[8] 现在是 %p，期望 %p（被改回？）",
              g_frame, cur, (void *)&HookPresent);
    }
}

// ---- 界面内容：日志面板（由主文件提供取数）----
struct UiHooks {
    void (*beginPanel)(bool *open);   // 主文件填：ImGui::Begin(...)
    void (*drawPanel)();              // 主文件填：面板内控件
    void (*endPanel)();               // 主文件填：ImGui::End()
};
static UiHooks g_hooks = {};

// ---- 光标只在面板上显示 ----------------------------------------------------
//
// 需求：游戏（Unity）自己用贴图画光标，那个光标在 overlay 之下被面板盖住。
// 解法：用 imgui **内置**的软件光标（io.MouseDrawCursor），但每帧动态开关：
//       悬停面板时才打开，移出就关掉，游戏自己的贴图光标继续服务正常操作。
//
// 为什么不用自绘箭头：试过 ImGui::GetForegroundDrawList() + AddConvexPolyFilled，
//       **完全不显示** —— 因为画在 ImGui::Render() 之后，此时 draw data 已组装完毕，
//       再往 draw list 里追加的东西不会提交。正确时机是 Render() 之前。
//
// 为什么不用固定 MouseDrawCursor=true：那样光标会一直挂在游戏画面上，
//       哪怕人根本没在用面板，反而干扰正常游戏操作。

// 本工具窗口的矩形（DisplaySize 空间），由主程序每帧通过 UiSetPanelRect() 上报。
// 这样就不依赖 imgui 的内部窗口查找接口。
static float g_panelX0 = 0, g_panelY0 = 0, g_panelX1 = 0, g_panelY1 = 0;
static bool  g_panelRectValid = false;

// 主程序每帧调用：告知面板矩形（DisplaySize 空间）
void UiSetPanelRect(float x0, float y0, float x1, float y1)
{
    g_panelX0 = x0; g_panelY0 = y0; g_panelX1 = x1; g_panelY1 = y1;
    g_panelRectValid = (x1 > x0 && y1 > y0);
}

// 命中测试用**屏幕**矩形（g_panelScreenX0..X1，由本模块每帧算好）
static bool CursorOverPanel(float sx, float sy)
{
    if (!g_panelRectValid || !g_panelVisible) return false;
    return sx >= g_panelScreenX0 && sx < g_panelScreenX1 &&
           sy >= g_panelScreenY0 && sy < g_panelScreenY1;
}

// 面板是否吃掉了该点（供"拦截点击透传"用）
bool UiCursorOverPanel() { return g_cursorOverPanel; }
bool UiPointOverPanel(int screenX, int screenY)
{
    return screenX >= (int)g_panelScreenX0 && screenX < (int)g_panelScreenX1 &&
           screenY >= (int)g_panelScreenY0 && screenY < (int)g_panelScreenY1;
}

// 每帧在 ImGui::Render() **之前**调用：算出"光标是否悬停在面板上"，
// 写进 io.MouseDrawCursor（imgui 内置软件光标的开关）。
//
// 关键时机：RenderMouseCursor 在 ImGui::EndFrame() 末尾执行（imgui.cpp:5298），
// 而 EndFrame 由 Render() 触发。所以 **必须在 Render 之前**设这个标志 ——
// 早一帧晚一帧都不生效（之前自绘箭头就是画在 Render 之后，结果完全不显示）。
//
// 顺带把面板矩形算成屏幕坐标，供命中测试与"吃掉点击"用。
static void UpdateCursorVisibility()
{
    if (!g_uiReady) return;
    ImGuiIO &io = ImGui::GetIO();

    // 面板矩形由主程序（il2cpp_test.cpp）每帧用 ImGui::GetWindowPos()+GetWindowSize()
    // 主动上报 —— 不用 FindWindowByName，它在 imgui 内部命名空间里，跨文件用不稳。
    if (!g_panelRectValid)
        return;

    // 屏幕坐标 = imgui 坐标 + 客户区屏幕原点（与 PollMouseForImGui 反向）
    float sx = 0, sy = 0;
    if (g_gameHwnd && IsWindow(g_gameHwnd)) {
        RECT rc;
        if (GetClientRect(g_gameHwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
            POINT org = { rc.left, rc.top };
            ClientToScreen(g_gameHwnd, &org);
            sx = (float)org.x;
            sy = (float)org.y;
        }
    }
    // 面板的屏幕矩形（命中测试用）
    g_panelScreenX0 = g_panelX0 + sx; g_panelScreenY0 = g_panelY0 + sy;
    g_panelScreenX1 = g_panelX1 + sx; g_panelScreenY1 = g_panelY1 + sy;

    float screenX = io.MousePos.x + sx;
    float screenY = io.MousePos.y + sy;

    const bool over = CursorOverPanel(screenX, screenY);
    if (over != g_cursorOverPanel) {
        g_cursorOverPanel = over;
        if (g_frame <= 3 || g_frame % 600 == 0)
            LogUi("[ui] f%lu 光标%s面板 (screen=%.0f,%.0f rect=%.0f,%.0f-%.0f,%.0f valid=%d)",
                  g_frame, over ? "进入" : "离开",
                  screenX, screenY, g_panelScreenX0, g_panelScreenY0, g_panelScreenX1, g_panelScreenY1,
                  (int)g_panelRectValid);
    }
    // 用 imgui 内置的软件光标，但**只在悬停面板时打开**：
    //   - 游戏（Unity）自己用贴图画光标，那个光标在 overlay 之下会被面板盖住；
    //   - 悬停面板时用 imgui 的光标（画在最上层，永远可见）；
    //   - 移出面板就关掉，让游戏自己的光标回到正常状态，不干扰操作。
    io.MouseDrawCursor = over;
}

void UiSetHooks(void (*beginPanel)(bool *), void (*drawPanel)(), void (*endPanel)()) {
    g_hooks.beginPanel = beginPanel;
    g_hooks.drawPanel = drawPanel;
    g_hooks.endPanel = endPanel;
}
// 每帧钩子：面板关着也要跑（主线程落位等逻辑依赖）
static void (*g_frameHook)() = nullptr;
void UiSetFrameHook(void (*fn)()) { g_frameHook = fn; }

// ---- 全屏叠加绘制钩子 -------------------------------------------------------
// 在 ImGui::NewFrame() 之后、ImGui::Render() 之前调用 —— 唯一合法的"画东西"时机。
// 用途：准星/坐标系等调试可视化（不受面板可见性影响）。
static void (*g_overlayDrawHook)() = nullptr;
void UiSetOverlayDrawHook(void (*fn)()) { g_overlayDrawHook = fn; }

// 把 imgui 上下文指针暴露给外部模块（工具 DLL 加载后 SetCurrentContext 用）。
// 本模块零 .tls，不能把 STL 传出去，这里只回一个裸指针。
void *UiImGuiContext() { return (void *)ImGui::GetCurrentContext(); }

// 给工具 DLL 用的日志出口：把工具侧的日志也写进本模块的日志文件，
// 否则工具 DLL 崩了完全看不到它的日志（它自己那份 logger 只在内存里）。
// 这样跨 DLL 排查崩溃才有线索。
void UiLogRaw(const char *s) { LogRaw("%s", s ? s : "(null)"); }
bool UiPanelVisible() { return g_panelVisible; }
void UiTogglePanel() { g_panelVisible = !g_panelVisible; }

// =====================================================================
//  真·鼠标滚轮支持（2026-10-11）
// =====================================================================
//
// 问题：Present 不在游戏的消息循环里，我们收不到 WM_MOUSEWHEEL；
//       而 GetAsyncKeyState 也查不到滚轮（滚轮没有"按住"状态，只有一次性增量）。
//       之前只能拿 PgUp/PgDn 顶替 —— 每帧固定走一格，完全不跟手。
//
// 办法：挂一个 **WH_MOUSE_LL 低级鼠标钩子**。
//   低级钩子在**系统层面**回调，由 Windows 把消息送到装钩子的线程的消息队列，
//   跟游戏自己的消息循环、跟 Present 在不在消息循环里**毫无关系** ——
//   只要进程活着、有消息泵（游戏主线程有），就能收到 WM_MOUSEWHEEL。
//   钩子回调里把 wheel delta **累加**到一个原子变量；渲染线程每帧取走并清零，
//   转成 io.AddMouseWheelEvent()。
//
// 为什么用 LL 钩子而不是 RegisterRawInput（RawInput 其实也能收滚轮）：
//   RawInput 需要 register 到具体 hwnd 且要处理 WM_INPUT —— 又回到"要靠消息循环"。
//   LL 钩子只需一个 SetWindowsHookEx(WH_MOUSE_LL, proc, hInst, 0)，最省事。
//
// 代价与注意：
//   - 钩子回调运行在**装钩子线程**的上下文中（本进程主线程），有轻微全局开销；
//     这只在 overlay 存活期间存在，卸载时会 UnhookWindowsHookEx。
//   - 回调里**绝不能调 imgui**（不在渲染线程），只能碰原子变量。
//   - 必须 return CallNextHookEx(...) 放行，否则会吞掉游戏自己的滚轮输入（比如地图缩放）。
// =====================================================================

static HHOOK g_mouseHook = nullptr;
static volatile LONG g_wheelAccum = 0;        // 累加的 wheel delta（1 格 = WHEEL_DELTA=120）
static volatile LONG g_mouseHookThreadId = 0; // 装钩子并 pump 消息的线程 id
static volatile LONG g_mouseHookStop = 0;     // 置 1 -> pump 线程退出
static HANDLE g_mouseHookThread = nullptr;    // pump 线程句柄

// WHEEL_DELTA = 120。imgui 的 AddMouseWheelEvent 期望"格数"（1.0 = 一格），
// 所以这里除以 120 得到格数。用整数累加，避免浮点跨线程问题。
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL)
    {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;
        if (ms)
        {
            int delta = (int)(short)HIWORD(ms->mouseData);   // 有符号：正=上滚
            // 累加到全局；正负都加，渲染线程每帧取走
            InterlockedExchangeAdd(&g_wheelAccum, (LONG)delta);
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// 钩子线程：装钩子 + 跑消息循环。
//
// ★ 为什么必须单独开线程 pump 消息：
//   WH_MOUSE_LL 的回调由系统投递到 **调用 SetWindowsHookEx 的那个线程的消息队列**，
//   要求该线程持续 GetMessage/PeekMessage。而本工程的 Worker 线程是 `for(;;) Sleep(1000)`
//   —— **从不 pump 消息**，若直接在它上面装钩子，回调永远不会触发（且可能被系统摘掉）。
//   所以这里专门 CreateThread 一个只做 GetMessage 的线程来拥有钩子。
static DWORD WINAPI MouseHookThread(LPVOID)
{
    g_mouseHookThreadId = (LONG)GetCurrentThreadId();
    // ★ dwThreadId 必须传 0！低级钩子（WH_MOUSE_LL/WH_KEYBOARD_LL）是 **global-only**，
    //   传具体线程 id 会让 SetWindowsHookEx 失败并返回 1429
    //   （ERROR_GLOBAL_ONLY_HOOK，"此挂接程序只可整体设置"）——这就是最初滚轮装不上的原因。
    //   hMod 传 NULL 是允许的（LL 钩子的回调可以放在当前进程代码里，不要求是 DLL）。
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
    if (!g_mouseHook)
    {
        LogUi("[ui] 安装 WH_MOUSE_LL 失败 (GetLastError=%lu) —— 滚轮退回 PgUp/PgDn",
              GetLastError());
        g_mouseHookThreadId = 0;
        return 0;
    }
    LogUi("[ui] WH_MOUSE_LL 已安装（pump tid=%ld）—— 真鼠标滚轮可用", (long)g_mouseHookThreadId);

    // 消息泵：直到要求停止。
    MSG msg;
    while (!g_mouseHookStop)
    {
        // PeekMessage 非阻塞，避免无消息时卡在 GetMessage 里导致停不掉
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(4);   // 4ms 轮询：滚轮事件延迟可忽略，又不至于空转烧 CPU
    }

    if (g_mouseHook)
    {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
        LogUi("[ui] WH_MOUSE_LL 已卸载");
    }
    g_mouseHookThreadId = 0;
    return 0;
}

// 安装钩子（起 pump 线程）。返回 true 表示线程起来了。
static bool InstallMouseWheelHook()
{
    if (g_mouseHookThread) return true;
    g_mouseHookStop = 0;
    g_mouseHookThread = CreateThread(nullptr, 0, MouseHookThread, nullptr, 0, nullptr);
    if (!g_mouseHookThread)
    {
        LogUi("[ui] 创建鼠标钩子线程失败 (GetLastError=%lu) —— 滚轮退回 PgUp/PgDn",
              GetLastError());
        return false;
    }
    return true;
}

static void UninstallMouseWheelHook()
{
    if (!g_mouseHookThread) return;
    g_mouseHookStop = 1;
    WaitForSingleObject(g_mouseHookThread, 1000);   // 等 pump 线程自行卸钩子退出
    CloseHandle(g_mouseHookThread);
    g_mouseHookThread = nullptr;
}

static void PollMouseForImGui()
{
    // Present 不在消息循环里，imgui 收不到 WM_MOUSEMOVE，需要每帧喂真实光标位置。
    //
    // **坐标系必须换算**（这就是"鼠标指向的位置和实际点击位置有偏移"的原因）：
    //   - GetCursorPos 给的是**屏幕坐标**；
    //   - imgui 用的是 **io.DisplaySize 空间**，那个值 = swapchain 的 back buffer 尺寸；
    //   - 实测机器：屏幕 1920x1080，游戏 back buffer 1600x900。
    // 直接把屏幕坐标喂给 imgui，就会偏 (窗口屏幕原点) —— 窗口不在屏幕左上角时必偏。
    //
    // 换算两步：
    //   1) 屏幕坐标 -> 客户区坐标：减去客户区屏幕原点（ClientToScreen(GetClientRect)）。
    //   2) 客户区 -> back buffer：按比例缩放（无边框全屏时客户区==backbuffer，比例=1）。
    ImGuiIO &io = ImGui::GetIO();

    POINT pt{};
    if (!GetCursorPos(&pt))
        return;

    float cx = (float)pt.x, cy = (float)pt.y;      // 默认：当作已是 backbuffer 坐标

    if (g_gameHwnd && IsWindow(g_gameHwnd)) {
        RECT rc;
        if (GetClientRect(g_gameHwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
            POINT org = { rc.left, rc.top };
            ClientToScreen(g_gameHwnd, &org);                 // org 变成客户区屏幕原点
            cx = (float)(pt.x - org.x);
            cy = (float)(pt.y - org.y);
            // 客户区 != back buffer 时按比例缩放（窗口化 / DPI 缩放 / 有边框时）
            const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            if ((int)io.DisplaySize.x != cw && cw > 0)
                cx = cx * io.DisplaySize.x / (float)cw;
            if ((int)io.DisplaySize.y != ch && ch > 0)
                cy = cy * io.DisplaySize.y / (float)ch;
        }
    }
    // 前几帧把换算结果打出来，便于核对偏移是否已消除
    if (g_frame <= 3) {
        RECT cr = {0};
        if (g_gameHwnd && IsWindow(g_gameHwnd)) GetClientRect(g_gameHwnd, &cr);
        LogUi("[ui] f%lu 输入: screen=(%ld,%ld) -> imgui=(%.0f,%.0f)  "
              "DisplaySize=%.0fx%.0f  client=%dx%d  hwnd=%p",
              g_frame, (long)pt.x, (long)pt.y, cx, cy,
              io.DisplaySize.x, io.DisplaySize.y,
              cr.right - cr.left, cr.bottom - cr.top, g_gameHwnd);
    }
    io.AddMousePosEvent(cx, cy);
    // 键鼠按钮：按当前状态喂（边缘触发由 io 处理）
    bool down[5] = {
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0,
        (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0,
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
        (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
    };
    for (int i = 0; i < 5; ++i) io.AddMouseButtonEvent(i, down[i]);
    // ---- 鼠标滚轮 ----
    // 主路径：LL 钩子累加的 wheel delta（每格 120）。取走并清零，转成"格数"喂 imgui。
    // 备用路径：PgUp/PgDn（钩子装不上、或鼠标没滚轮时用），每帧 ±1 格。
    // 二者可叠加（不影响）。
    float wheel = 0.0f;
    LONG acc = InterlockedExchange(&g_wheelAccum, 0);
    if (acc != 0)
        wheel += (float)acc / 120.0f;          // WHEEL_DELTA=120
    if (GetAsyncKeyState(VK_PRIOR) & 0x8000) wheel += 1.0f;   // 上滚（备用）
    if (GetAsyncKeyState(VK_NEXT) & 0x8000) wheel -= 1.0f;    // 下滚（备用）
    if (wheel != 0.0f) io.AddMouseWheelEvent(0.0f, wheel);
    // INSERT 切换面板
    static bool insertDown = false;
    bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (ins && !insertDown) g_panelVisible = !g_panelVisible;
    insertDown = ins;

    // ---- 键盘轮询（15:55 面板数字输入需要）----
    //
    // 和鼠标同理：Present 不在消息循环里，WM_CHAR/WM_KEYDOWN 全收不到，
    // 不主动喂键盘事件 io.InputQueueCharacters 永远是空的，输入框打不了字。
    // 做法：GetAsyncKeyState 逐键轮询，喂 AddKeyEvent（键状态）+ 在按下沿用
    // ToUnicode 生成字符喂 AddInputCharacter（文本）。
    static bool s_prevKey[256] = {};     // 键事件去抖
    static bool s_prevChar[256] = {};    // 字符按下沿检测（与前者独立，避免互相吃掉边沿）
    static bool s_caps = false;          // CapsLock 锁定态（自己跟踪，原因见下）
    static bool s_prevCapsKey = false;   // CapsLock 键按下沿
    static bool s_inited = false;
    if (!s_inited) {
        // 首帧初始化：只记状态不喂事件，避免把"注入前按住的键"误判为按下沿
        for (int vk = 0; vk < 256; ++vk) {
            s_prevKey[vk] = s_prevChar[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }
        // CapsLock 初值只能用 GetKeyState 猜一次（它读线程输入队列，可能不准），
        // 之后靠"按下沿自己翻转"维护，不再依赖它。
        s_caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        s_prevCapsKey = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
        s_inited = true;
        return;
    }
    static const struct { int vk; ImGuiKey key; } kKeys[] = {
        { VK_BACK, ImGuiKey_Backspace }, { VK_RETURN, ImGuiKey_Enter },
        { VK_ESCAPE, ImGuiKey_Escape },  { VK_DELETE, ImGuiKey_Delete },
        { VK_LEFT, ImGuiKey_LeftArrow }, { VK_RIGHT, ImGuiKey_RightArrow },
        { VK_UP, ImGuiKey_UpArrow },     { VK_DOWN, ImGuiKey_DownArrow },
        { VK_HOME, ImGuiKey_Home },      { VK_END, ImGuiKey_End },
        { VK_TAB, ImGuiKey_Tab },
        { VK_SHIFT, ImGuiMod_Shift },    { VK_CONTROL, ImGuiMod_Ctrl },
        { '0', ImGuiKey_0 }, { '1', ImGuiKey_1 }, { '2', ImGuiKey_2 },
        { '3', ImGuiKey_3 }, { '4', ImGuiKey_4 }, { '5', ImGuiKey_5 },
        { '6', ImGuiKey_6 }, { '7', ImGuiKey_7 }, { '8', ImGuiKey_8 },
        { '9', ImGuiKey_9 },
        { VK_OEM_MINUS, ImGuiKey_Minus }, { VK_OEM_PERIOD, ImGuiKey_Period },
        { VK_OEM_COMMA, ImGuiKey_Comma },
        // ---- 字母键也要喂 KeyEvent（2026-10-11 补）----
        // 之前字母只走下面的 ToUnicode -> AddInputCharacter（文本），**从不产生 ImGuiKey_A..Z**。
        // 后果：imgui 的剪贴板快捷键是按 key binding 实现的（ImGuiKey_V + Ctrl），
        // 没有 V 的 key event -> **Ctrl+V 粘贴完全失效**，Ctrl+C/X/A 同理。
        // 补上后 Ctrl+V 生效（imgui 的 Win32 默认剪贴板实现走 CF_UNICODETEXT + UTF-8，
        // 中文能正常粘进输入框 —— 这是本注入环境下输入中文最可行的路径）。
        { 'A', ImGuiKey_A }, { 'B', ImGuiKey_B }, { 'C', ImGuiKey_C },
        { 'D', ImGuiKey_D }, { 'E', ImGuiKey_E }, { 'F', ImGuiKey_F },
        { 'G', ImGuiKey_G }, { 'H', ImGuiKey_H }, { 'I', ImGuiKey_I },
        { 'J', ImGuiKey_J }, { 'K', ImGuiKey_K }, { 'L', ImGuiKey_L },
        { 'M', ImGuiKey_M }, { 'N', ImGuiKey_N }, { 'O', ImGuiKey_O },
        { 'P', ImGuiKey_P }, { 'Q', ImGuiKey_Q }, { 'R', ImGuiKey_R },
        { 'S', ImGuiKey_S }, { 'T', ImGuiKey_T }, { 'U', ImGuiKey_U },
        { 'V', ImGuiKey_V }, { 'W', ImGuiKey_W }, { 'X', ImGuiKey_X },
        { 'Y', ImGuiKey_Y }, { 'Z', ImGuiKey_Z },
    };
    for (const auto &k : kKeys) {
        bool d = (GetAsyncKeyState(k.vk) & 0x8000) != 0;
        if (d != s_prevKey[k.vk]) {
            io.AddKeyEvent(k.key, d);
            s_prevKey[k.vk] = d;
        }
    }
    // 文本字符：**全键扫描**（0..255），在按下沿用 ToUnicode 转成字符喂给 imgui。
    //
    // 为什么不再维护"能打字的键"白名单：白名单漏一个键就打不出那个字（之前漏了
    // VK_OEM_PLUS，"+" 和 "=" 一直打不出来）。全扫一遍最省心，代价是每帧 256 次
    // GetAsyncKeyState —— 这是个很轻的调用，可以忽略。
    // 控制字符（Enter/Backspace/Tab/Esc 等，< 0x20）在这里丢掉，它们走上面的 AddKeyEvent。
    // CapsLock 锁定态：边沿翻转自己维护（GetKeyState 的低位依赖线程输入队列，不可靠）
    {
        bool cd = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
        if (cd && !s_prevCapsKey) s_caps = !s_caps;
        s_prevCapsKey = cd;
    }
    for (int vk = 0; vk < 256; ++vk) {
        bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool edge = d && !s_prevChar[vk];     // 按下沿（不做长按重复，够用）
        s_prevChar[vk] = d;
        if (!edge) continue;
        // ★★ 键状态数组必须**自己拼**，绝不能用 GetKeyboardState() ★★
        //   GetKeyboardState 返回的是**调用线程输入队列**的键状态：只有该线程自己
        //   拥有焦点、并且处理过对应的键盘消息之后它才更新。
        //   而本函数跑在 Present 钩子里 —— 那是**游戏渲染线程**，很可能既不持有窗口
        //   焦点也不处理键盘消息，拿到的是"什么都没按下"的状态 ⇒ ToUnicode 看不到
        //   Shift ⇒ **按 Shift 打出来的永远是小写**（实测：Shift+A 出 'a'，
        //   Shift+- 出 '-' 而不是 '_'）。
        //   GetAsyncKeyState 是全局的、与线程无关，逐键取高位自己拼就对了。
        BYTE ks[256] = {};
        for (int v = 0; v < 256; ++v)
            if (GetAsyncKeyState(v) & 0x8000) ks[v] = 0x80;
        if (s_caps) ks[VK_CAPITAL] |= 0x01;   // 低位 = 锁定态（开/关）
        wchar_t wcs[8] = {};
        int n = ToUnicode((UINT)vk, 0, ks, wcs, 8, 0);
        for (int c = 0; c < n; ++c) {
            if (wcs[c] >= 0x20)
                io.AddInputCharacter((unsigned int)wcs[c]);
        }
    }
}

static void BindToChain(IDXGISwapChain *sc) {
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&dev)) || !dev) return;
    dev->GetImmediateContext(&ctx);
    DXGI_SWAP_CHAIN_DESC d = {0};
    HWND hwnd = NULL;
    if (SUCCEEDED(sc->GetDesc(&d))) hwnd = d.OutputWindow;

    // 第一次进来时把拿到的所有指针**逐个校验**。之前只用 `if (ctx)` 这种真值判断，
    // 而 COM 指针可能是个不可读的小整数（真值非 NULL 但解引用就崩），
    // 典型症状是日志打出 "init ok" 然后下一帧闪退 —— 日志里已经见过 dev=0x6FF13E20。
    if (!g_uiReady) {
        LogUi("[ui] BindToChain: sc=%p dev=%p ctx=%p hwnd=%p",
              (void *)sc, (void *)dev, (void *)ctx, (void *)hwnd);
        bool devBad = IsBadReadPtr(dev, 0x100);
        bool ctxBad = ctx ? IsBadReadPtr(ctx, 0x100) : true;
        LogUi("[ui]   可读性: dev=%s ctx=%s  hwnd=%p IsWindow=%s",
              devBad ? "BAD" : "ok", ctxBad ? "BAD" : "ok",
              hwnd, (hwnd && IsWindow(hwnd)) ? "yes" : "no");
        // 设备指针不可读就意味着传进来的是**野对象**（可能是我自己的 dummy 链已经 Release 掉）。
        // 此时绝不能初始化 imgui —— 直接跳过，等下一条真正的游戏链。
        if (devBad || ctxBad || !dev || !ctx) {
            LogUi("[ui]   -> dev/ctx 不可用，本帧不初始化（等下一条有效链）");
            dev->Release();
            return;
        }
        // **HWND 有效性必须用 IsWindow() 判断，不能用 IsBadReadPtr()**。
        // IsBadReadPtr 是"这块内存能不能读"，对句柄这种不透明值毫无意义 ——
        // 日志里 hwnd=0x8D16C8 被判成 BAD 就是这个误用导致的。
        // 用 IsWindow() 还能同时覆盖"句柄已销毁"和"根本不是窗口"两种情况。
        if (hwnd && !IsWindow(hwnd)) {
            LogUi("[ui]   -> OutputWindow %p 不是有效窗口，改用 GetForegroundWindow()", hwnd);
            HWND fg = GetForegroundWindow();
            hwnd = (fg && IsWindow(fg)) ? fg : NULL;
            LogUi("[ui]   -> hwnd = %p", hwnd);
        }
        if (!hwnd) {
            // 实在拿不到窗口也不影响：坐标换算会退化成"屏幕坐标即 backbuffer 坐标"，
            // 对**无边框全屏**（窗口铺满整个 backbuffer）恰好是正确的。
            LogUi("[ui]   -> 无有效 hwnd，坐标将按全屏处理（屏幕坐标直接用）");
        }
        LogUi("[ui]   开始 ImGui::CreateContext()");
        // --- 字体：系统微软雅黑 + 常用简体字形范围 ---
        if (!ImGui::CreateContext()) {
            LogUi("[ui]   FAILED: ImGui::CreateContext() 返回 NULL");
            dev->Release();
            return;
        }
        LogUi("[ui]   CreateContext ok，开始 ImGui_ImplDX11_Init");
        if (!ImGui_ImplDX11_Init(dev, ctx)) {
            LogUi("[ui]   FAILED: ImGui_ImplDX11_Init 返回 false（设备对象创建失败）");
            ImGui::DestroyContext();
            dev->Release();
            return;
        }
        LogUi("[ui]   ImGui_ImplDX11_Init ok");
        ImGui::StyleColorsDark();

        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;                       // 不写 imgui.ini
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.LogFilename = nullptr;

        // ---- 软件鼠标光标 ----
        // 这游戏（Unity）**隐藏了系统光标、自己用贴图画指针**，那个指针画在游戏
        // 画面里 —— 必然在 overlay 之下，面板区域会把它盖住，看不到光标在哪。
        // imgui 1.91 内置软件光标：EndFrame 之后用 RenderMouseCursor 绘制
        //（imgui.cpp:5298，位于所有窗口之后），所以能盖在面板之上。
        //
        // 光标开关由每帧的 UpdateCursorVisibility() 动态控制（悬停面板才开）。
        // 这里先关掉，避免第一帧无条件显示。
        io.MouseDrawCursor = false;
        LogUi("[ui]   软件光标：仅悬停面板时显示（每帧动态开关）");

        const wchar_t *fontPaths[] = {
            L"C:\\Windows\\Fonts\\msyh.ttc",              // 微软雅黑（优先，界面最干净）
            L"C:\\Windows\\Fonts\\simhei.ttf",             // 黑体（次选）
            L"C:\\Windows\\Fonts\\simsun.ttc",             // 宋体（兜底）
        };
        ImFontConfig fc;
        fc.FontNo = 0;
        // ★glyph ranges = CJK 表意文字 + 符号区（圈号①②③、箭头、几何、勾叉、全角）
        //   根因（2026-10-14 实测）：
        //     · GetGlyphRangesChineseSimplifiedCommon 只含 **2500 个最常用字**，
        //       渲 / 帧 / 逻 / 辑 都不在里面 ⇒ 面板「渲染帧数」显示成「?染?数」。
        //       所以默认改用 GetGlyphRangesChineseFull（见 mod_config.h 的 MOD_FONT_FULL_CJK，
        //       那里写了实测的字形数/图集尺寸/构建耗时）。
        //     · 即使 ChineseFull 也不含 U+2460 圈号，所以还要自己拼符号区间。
        //   malloc 拼接；**不用函数内 static 非平凡对象**（避免 __cxa_guard/libstdc++ EH）
        struct RangesBuf
        {
            static const ImWchar *Build()
            {
                static ImWchar *cached = nullptr;          // POD 指针，静态安全
                if (cached) return cached;
#if MOD_FONT_FULL_CJK
                const ImWchar *base = ImGui::GetIO().Fonts->GetGlyphRangesChineseFull();
#else
                const ImWchar *base = ImGui::GetIO().Fonts
                                        ->GetGlyphRangesChineseSimplifiedCommon();
#endif
                size_t n = 0;
                while (base[n]) n += 2;                    // 区间对序列，末尾 0
                static const ImWchar extra[] = {
                    0x2000, 0x206F,   // 通用标点 — – …
                    0x2190, 0x21FF,   // 箭头 ← → ↑ ↓
                    0x2460, 0x24FF,   // ★圈号 ①②③④⑤⑥⑦⑧⑨
                    0x25A0, 0x25FF,   // 几何图形 ■ ▲ ▼ ●
                    0x2600, 0x26FF,   // 杂项符号 ★ ☆ ⚠
                    0x2700, 0x27BF,   // 勾叉 ✓ ✔ ✗ ✘
                    0x2B00, 0x2BFF,   // 补充箭头
                    0xFF00, 0xFFEF,   // 全角字符
                    0
                };
                size_t m = 0;
                while (extra[m]) m += 2;
                ImWchar *out = (ImWchar *)malloc((n + m + 1) * sizeof(ImWchar));
                if (!out) return base;
                memcpy(out, base, n * sizeof(ImWchar));
                memcpy(out + n, extra, (m + 1) * sizeof(ImWchar)); // 含结尾 0
                cached = out;
                return out;
            }
        };
        bool fontOk = false;
        for (size_t i = 0; i < sizeof(fontPaths) / sizeof(fontPaths[0]); ++i) {
            FILE *fp = NULL;
            if (_wfopen_s(&fp, fontPaths[i], L"rb") != 0 || !fp) continue;
            fclose(fp);
            char pathA[MAX_PATH];
            WideCharToMultiByte(CP_ACP, 0, fontPaths[i], -1, pathA, sizeof(pathA), NULL, NULL);
            if (io.Fonts->AddFontFromFileTTF(pathA, 18.0f, &fc, RangesBuf::Build())) {
                char pathU[200];
                WideCharToMultiByte(CP_UTF8, 0, fontPaths[i], -1, pathU, sizeof(pathU), NULL, NULL);
                // ★ 这行日志必须把**实际生效的字形范围模式**打出来。
                //   原来硬编码写着 "ChineseSimplifiedCommon"，结果 MOD_FONT_FULL_CJK
                //   因为漏 include 而静默失效时，日志还在说"用的是常用2500"，完全看不出来。
                LogUi("[ui] font loaded: %s (18px, 字形范围=%s)", pathU,
#if MOD_FONT_FULL_CJK
                      "完整CJK(4E00-9FFF)+符号区");
#else
                      "常用简体2500+符号区");
#endif
                fontOk = true;
                break;
            }
        }
        if (!fontOk) {
            io.Fonts->AddFontDefault();                 // 无中文字体时仍可显示 ASCII
            LogUi("[ui] WARN: no CJK font found, falling back to default (ASCII only)");
        }

        POINT pt{};
        if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt))
            io.AddMousePosEvent((float)pt.x, (float)pt.y);

        // ---- 关键：DisplaySize / DeltaTime 必须由我们自己填 ----
        // 这两件事原本是 win32 backend 的活，但我们为了避开 emutls 静态 TLS 把它砍了
        //（见 Makefile 头注释）。结果 io.DisplaySize 一直是 (0,0)、DeltaTime 一直是 0，
        // 而 ImGui::NewFrame() 里有两条 assert：
        //     IM_ASSERT((g.IO.DeltaTime > 0.0f || g.FrameCount == 0) && "Need a positive DeltaTime!");
        //     IM_ASSERT(g.IO.DisplaySize.x >= 0.0f && ...);      // (0,0) 能过这条，但会导致布局全 0
        // 编译时没加 -DNDEBUG，所以 IM_ASSERT 展开成 assert() -> abort() -> **整个游戏闪退**。
        // 这就是"卡住然后闪退"的真正原因。
        ID3D11Texture2D *bb0 = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb0)) && bb0) {
            D3D11_TEXTURE2D_DESC td;
            bb0->GetDesc(&td);
            io.DisplaySize = ImVec2((float)td.Width, (float)td.Height);
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            bb0->Release();
        } else {
            // 拿不到 back buffer 就退到窗口客户区大小，至少不是 0
            RECT rc = { 0, 0, 640, 480 };
            if (hwnd) GetClientRect(hwnd, &rc);
            io.DisplaySize = ImVec2((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
            LogUi("[ui]   GetBuffer 失败，用窗口客户区 %dx%d 作为 DisplaySize",
                  (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        }
        LogUi("[ui]   DisplaySize = %.0fx%.0f", io.DisplaySize.x, io.DisplaySize.y);

        g_uiDev = dev; g_uiCtx = ctx; g_uiSc = sc; g_gameHwnd = hwnd;
        g_lastFrameTime = GetTickCount64();
        g_uiReady = true;
        LogUi("[ui] first Present: imgui init ok (hwnd=%p sc=%p dev=%p)", hwnd, (void *)sc, (void *)dev);

        // ★ 装"游戏退出守护"（点 X / Alt+F4 / 注销 / 窗口被销毁）。
        //   放在这里是因为：本函数跑在**游戏渲染线程**上，而 Unity 的 D3D11 Present
        //   就在主线程 —— 子类化窗口过程要求与窗口同线程，此时机正好。
        //   不装它的话，游戏自己退出时我们的钩子/线程原样留着，退出序列会卡住
        //   （见 src/exit_guard.h 的完整说明）。
        ExitGuard_AttachToWindow(hwnd);
        return;
    }

    // 游戏换了主渲染链 -> 重绑后端
    if (sc != g_uiSc) {
        LogUi("[ui] swapchain changed %p -> %p, rebinding", (void *)g_uiSc, (void *)sc);
        ReleaseBackRTV();
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_Shutdown();
        if (ImGui_ImplDX11_Init(dev, ctx)) {
            if (g_uiDev) g_uiDev->Release();
            g_uiDev = dev; g_uiCtx = ctx; g_uiSc = sc; g_gameHwnd = hwnd;
            g_lastFrameTime = GetTickCount64();
        } else {
            LogUi("[ui] rebind FAILED: ImGui_ImplDX11_Init 返回 false，保持旧绑定");
            dev->Release();
            g_uiReady = false;      // 别再往一个 Init 失败的后端上画
            return;
        }
    } else if (g_uiDev != dev) {
        LogUi("[ui] device changed %p -> %p, rebinding", (void *)g_uiDev, (void *)dev);
        ReleaseBackRTV();
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_Shutdown();
        if (ImGui_ImplDX11_Init(dev, ctx)) {
            if (g_uiDev) g_uiDev->Release();
            g_uiDev = dev; g_uiCtx = ctx;
            g_lastFrameTime = GetTickCount64();
        } else {
            LogUi("[ui] rebind FAILED: ImGui_ImplDX11_Init 返回 false，保持旧绑定");
            dev->Release();
            g_uiReady = false;
            return;
        }
    }
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain *sc, UINT syncInterval, UINT flags) {
    // ---- 卸载时序驱动：必须在**游戏渲染线程**上做，且要早于一切 imgui 操作 ----
    // 注入器线程只置标志（EspUiRequestShutdown），实际清理在这里分 3 帧完成。
    PumpUnloadIfRequested();

    ++g_frame;
    if (g_frame <= 20 || g_frame % 300 == 0)
        LogUi("[ui] Present frame=%lu sc=%p ready=%d", g_frame, (void *)sc, (int)g_uiReady);
    if (g_frame % 600 == 0) CheckPresentHookAlive();

    // 只对**游戏自己的**链做绑定。
    // 必须排除 g_dummySc：它是 64x64 + GetDesktopWindow 的探测链，
    // 用它去初始化 imgui 会拿到一个假窗口（症状：日志里 hwnd 是 0xC00A0A 这种
    // 无意义小值），而且它不是游戏渲染的那个 device，画出来也不会被看到。
    // 卸载进行中：不要再碰 imgui，直接把这一帧交给原 Present。
    // （DestroyContext 之后任何 Add* 都是访问已释放内存）
    // ★ 判空保护：脱钩帧（n==2）置完成标志后，注入器线程可能立即执行
    //   ReleaseOwnedResources() 清掉 g_origPresent，而本线程还没走到这里 ——
    //   2026-10-11 闪退修正的配套保险（撞上就丢一帧，不能崩）。
    if (g_unloadRequested)
        return g_origPresent ? g_origPresent(sc, syncInterval, flags) : S_OK;

    if (sc != g_dummySc && !IsBadReadPtr(sc, sizeof(void *))) BindToChain(sc);

    if (g_uiReady && g_uiCtx) {
        // 第一次见到这条链时，给它的 vtable 打 ResizeBuffers 补丁。
        // 注意条件是 `!g_origResize`（还没记下原函数）而不是反过来 —— 移植时写反过，
        // 结果这段永远不执行，窗口缩放时 imgui 的 RTV 不会失效重建。
        if (!g_origResize && sc != g_dummySc) {
            void **vtbl = *reinterpret_cast<void ***>(sc);
            if (vtbl && !IsBadReadPtr(vtbl, sizeof(void *) * 16)) {
                g_origResize = (ResizeBuffersFn)vtbl[RESIZEBUFFERS_VTABLE_INDEX];
                g_resizeVtable = vtbl;      // 记下来，卸载时要还原
                g_origResizeSlot = vtbl[RESIZEBUFFERS_VTABLE_INDEX];   // 备份原始值
                DWORD old;
                VirtualProtect(&vtbl[RESIZEBUFFERS_VTABLE_INDEX], sizeof(void *), PAGE_READWRITE, &old);
                vtbl[RESIZEBUFFERS_VTABLE_INDEX] = (void *)&HookResizeBuffers;
                VirtualProtect(&vtbl[RESIZEBUFFERS_VTABLE_INDEX], sizeof(void *), old, &old);
                FlushInstructionCache(GetCurrentProcess(), &vtbl[RESIZEBUFFERS_VTABLE_INDEX], sizeof(void *));
                LogUi("[ui] ResizeBuffers vtable patched on game swapchain %p (orig=%p)",
                      (void *)sc, (void *)g_origResize);
            }
        }

        // ---- 以下每一步都可能崩，逐段留证据 ----
        // 每帧刷新 DisplaySize / DeltaTime：窗口大小会变，而 NewFrame 的 assert 每帧都查。
        // 这两件事原本由 win32 backend 负责，我们砍了那个 backend，必须自己补。
        {
            ImGuiIO &fio = ImGui::GetIO();
            ID3D11Texture2D *bbNow = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bbNow)) && bbNow) {
                D3D11_TEXTURE2D_DESC td;
                bbNow->GetDesc(&td);
                fio.DisplaySize = ImVec2((float)td.Width, (float)td.Height);
                fio.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
                bbNow->Release();
            }
            ULONGLONG now = GetTickCount64();
            double dt = (double)(now - g_lastFrameTime) / 1000.0;
            g_lastFrameTime = now;
            // NewFrame 里 assert(DeltaTime > 0 || FrameCount==0)，所以必须给正值并夹住上限
            fio.DeltaTime = (float)(dt > 0.0 ? (dt < 0.1 ? dt : 0.1) : 0.00001);
        }

        if (g_frame <= 3) LogUi("[ui] f%lu: poll mouse", g_frame);
        PollMouseForImGui();
        if (g_frame <= 3) LogUi("[ui] f%lu: ImGui_ImplDX11_NewFrame", g_frame);
        ImGui_ImplDX11_NewFrame();
        // 字体图集是在上面这一句里**第一次**构建并上传的（后端 NewFrame 里
        // `if (!pFontSampler) CreateDeviceObjects()`）。所以字形统计要在这里打，
        // 打早了全是 0。这行是"字形范围改动有没有真的生效"的唯一客观证据：
        //   完整 CJK ≈ 21791 字形 / 4096x4096；常用 2500 ≈ 3299 字形 / 1024x2048。
        if (g_frame <= 3) {
            ImGuiIO &fio = ImGui::GetIO();
            int totalGlyphs = 0;
            for (int fi = 0; fi < fio.Fonts->Fonts.Size; ++fi)
                totalGlyphs += fio.Fonts->Fonts[fi]->Glyphs.Size;
            LogUi("[ui] f%lu: 字体图集 %dx%d, 合计 %d 字形, 字体数 %d",
                  g_frame, fio.Fonts->TexWidth, fio.Fonts->TexHeight,
                  totalGlyphs, fio.Fonts->Fonts.Size);
        }
        if (g_frame <= 3) LogUi("[ui] f%lu: ImGui::NewFrame", g_frame);
        if (g_frameHook) g_frameHook();
        ImGui::NewFrame();

        if (g_panelVisible && g_hooks.beginPanel) {
            bool open = true;
            g_hooks.beginPanel(&open);
            if (open) {
                if (g_hooks.drawPanel) g_hooks.drawPanel();
            }
            if (g_hooks.endPanel) g_hooks.endPanel();
            if (!open) g_panelVisible = false;
        }

        // 全屏叠加（准星/坐标系等调试可视化；面板关着也画）
        if (g_overlayDrawHook) g_overlayDrawHook();

        // 光标可见性：**必须在 Render 之前**（Render 里的 EndFrame 才消费 MouseDrawCursor）
        UpdateCursorVisibility();

        ImGui::Render();
        if (g_frame <= 3) LogUi("[ui] f%lu: Render ok, 绑定 back buffer", g_frame);

        // 游戏主画面阶段可能把 RTV 换成中间渲染目标或解绑，ImGui 后端画到当前绑定目标上
        // 会导致 overlay 只在 logo 页可见。这里显式绑回 back buffer，画完恢复（含 DSV）。
        ID3D11RenderTargetView *oldRTV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {0};
        ID3D11DepthStencilView *oldDSV = nullptr;
        g_uiCtx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, &oldDSV);
        if (ID3D11RenderTargetView *bb = EnsureBackRTV(sc))
            g_uiCtx->OMSetRenderTargets(1, &bb, nullptr);
        else if (g_frame <= 3)
            LogUi("[ui] f%lu: EnsureBackRTV 返回 NULL —— 本帧不画 overlay", g_frame);
        if (g_frame <= 3) LogUi("[ui] f%lu: ImGui_ImplDX11_RenderDrawData", g_frame);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_uiCtx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRTV, oldDSV);
        for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (oldRTV[i]) oldRTV[i]->Release();
        if (oldDSV) oldDSV->Release();
        if (g_frame <= 3) LogUi("[ui] f%lu: 绘制完成，调原 Present", g_frame);
    }

    // ★ 判空同样必须：热重载/退出清理的竞态窗口里，注入器线程可能刚把
    //   g_origPresent 清掉，而本线程已经越过了上面那条快速路径。
    //   直接调空指针 = 渲染线程当场崩（不是"丢一帧"）。
    return g_origPresent ? g_origPresent(sc, syncInterval, flags) : S_OK;
}

// =====================================================================
//  对外接口
// =====================================================================
int UiOverlayInstall() {
    // dummy 链只为拿全局唯一的 Present 代码地址；detour 装好后它就没用了。
    GrabPresentAddress();
    if (!g_origPresent) {
        LogUi("[ui] install FAILED: Present detour not in place");
        return 0;
    }
    // 装鼠标滚轮钩子（失败不影响 overlay 本体，滚轮退回 PgUp/PgDn）。
    InstallMouseWheelHook();
    LogUi("[ui] install ok — overlay 会在游戏下一次 Present 时自动绑定");
    return 1;
}

// ---------------------------------------------------------------- 卸载
//
// 重要：**绝不能在注入器线程上销毁 imgui context**。
// EspUnload() 由 map_dll 远程调用、执行在注入器线程；而 imgui context 正被
// **游戏渲染线程**每帧读写。在注入器线程里 DestroyContext 是数据竞争 ——
// 实测必崩（日志停在 "begin unload" 没有 end，目标进程整个消失）。
//
// 所以拆成两段：
//   EspUiRequestShutdown()  —— 注入器线程调用，只置标志（线程安全，不碰 imgui）
//   UiShutdownOnRenderThread() —— **Present 钩子里**调用，才是真正的清理
//
// 卸载时序（在渲染线程上）：
//   帧 N   ：看到标志 -> 还原 vtable[8]（下一帧不再进钩子）
//   帧 N+2 ：确认没新帧进入 -> 销毁 context / 释放资源
//   帧 N+3 ：清理 trampoline 池（此后本镜像代码已无人执行，释放才安全）

// 注入器线程侧：仅置标志，立刻返回（不阻塞、不碰 imgui）
void EspUiRequestShutdown()
{
    InterlockedExchange(&g_unloadRequested, 1);
    LogUi("[ui] 收到卸载请求（由渲染线程执行实际清理）");
}

int EspUiShutdownPending() { return InterlockedCompareExchange(&g_unloadRequested, 0, 0) != 0; }

// 渲染线程侧第一步：还原 vtable，之后本镜像不再被调用
static void RestoreVtables()
{
    if (g_presentVtable && !IsBadReadPtr(g_presentVtable, sizeof(void *) * 16)) {
        void *cur = g_presentVtable[PRESENT_VTABLE_INDEX];
        void *orig = g_origPresentSlot ? g_origPresentSlot : (void *)g_origPresent;
        if (cur == (void *)&HookPresent) {
            DWORD old = 0;
            if (VirtualProtect(&g_presentVtable[PRESENT_VTABLE_INDEX], sizeof(void *),
                                PAGE_READWRITE, &old)) {
                g_presentVtable[PRESENT_VTABLE_INDEX] = orig;
                VirtualProtect(&g_presentVtable[PRESENT_VTABLE_INDEX], sizeof(void *), old, &old);
                FlushInstructionCache(GetCurrentProcess(),
                                       &g_presentVtable[PRESENT_VTABLE_INDEX], sizeof(void *));
                LogUi("[ui] vtable[8] restored -> %p", orig);
            }
        } else {
            LogUi("[ui] vtable[8] 已不是我们的钩子（%p），不还原", cur);
        }
        g_presentVtable = nullptr;
    }
    if (g_resizeVtable && !IsBadReadPtr(g_resizeVtable, sizeof(void *) * 16)) {
        void *cur = g_resizeVtable[RESIZEBUFFERS_VTABLE_INDEX];
        void *orig = g_origResizeSlot ? g_origResizeSlot : (void *)g_origResize;
        if (cur == (void *)&HookResizeBuffers) {
            DWORD old = 0;
            if (VirtualProtect(&g_resizeVtable[RESIZEBUFFERS_VTABLE_INDEX], sizeof(void *),
                                PAGE_READWRITE, &old)) {
                g_resizeVtable[RESIZEBUFFERS_VTABLE_INDEX] = orig;
                VirtualProtect(&g_resizeVtable[RESIZEBUFFERS_VTABLE_INDEX], sizeof(void *), old, &old);
                FlushInstructionCache(GetCurrentProcess(),
                                       &g_resizeVtable[RESIZEBUFFERS_VTABLE_INDEX], sizeof(void *));
                LogUi("[ui] vtable[13] restored");
            }
        }
        g_resizeVtable = nullptr;
    }
}

// 渲染线程侧第二步：真正销毁 imgui（此刻已无并发访问）
static void DestroyImguiOnRenderThread()
{
    if (!g_uiReady) return;
    ReleaseBackRTV();
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext(nullptr);
    g_uiReady = false;
    LogUi("[ui] imgui context destroyed (on render thread)");
}

// 渲染线程侧第三步：释放本镜像自己占的资源
// 注意：这里**不能** VirtualFree 自己的镜像（那会释放正在执行的代码页）。
// 真正的镜像释放交给工具侧的 VirtualFreeEx。
static void ReleaseOwnedResources()
{
    UninstallMouseWheelHook();          // 先卸滚轮钩子（它会有回调进本镜像，必须先断）
    ReleaseBackRTV();
    if (g_dummySc)  { g_dummySc->Release();  g_dummySc = nullptr; }
    if (g_dummyDev) { g_dummyDev->Release(); g_dummyDev = nullptr; }
    if (g_dummyCtx) { g_dummyCtx->Release(); g_dummyCtx = nullptr; }
    // 我们自己从游戏 swapchain 上取的 device/context 引用（BindToChain 里的
    // GetDevice / GetImmediateContext）也要还回去 —— 不还的话游戏自己的
    // Release 永远到不了 0，退出时图形栈收不了尾。
    if (g_uiDev) { g_uiDev->Release(); g_uiDev = nullptr; }
    if (g_uiCtx) { g_uiCtx->Release(); g_uiCtx = nullptr; }
    if (g_trampPool) {
        VirtualFree(g_trampPool, 0, MEM_RELEASE);
        g_trampPool = nullptr;
        g_trampUsed = 0;
    }
    g_uiSc = nullptr; g_gameHwnd = nullptr;
    g_origPresent = nullptr; g_origResize = nullptr;
    g_origPresentSlot = nullptr; g_origResizeSlot = nullptr;
    g_frame = 0; g_cursorOverPanel = false; g_panelRectValid = false;
}

// 在 Present 钩子开头调用：驱动整个卸载时序（全在渲染线程，安全）
//
// 渲染线程侧只有两步：
//   第 1 帧：销毁 imgui context（本帧后续走 1047 行快速路径 `return g_origPresent(...)`，
//           origPresent 还有效 → 安全）
//   第 2 帧：还原 vtable[8]（脱钩！此后游戏直接调原始 Present，不再进本钩子）
//           + 置 g_shutdownDone
//
// **释放资源（ReleaseOwnedResources）绝不能在渲染线程做** —— 它会把 g_origPresent
// 置空。而钩子活着的前提就是 origPresent 有效（1047 行快速路径靠它退场）。
// 上一版把它放第 2 帧、还原 vtable 放第 3 帧 ⇒ 第 2 帧末尾 `return g_origPresent(...)`
// = 调用空指针 → **渲染线程当场崩**（2026-10-11 13:36 实测：游戏卡死闪退，
// 主日志最后一行 "Present frame=1 ready=0" 就是它最后一口气 —— g_frame 被
// ReleaseOwnedResources 清零后 ++g_frame 又变 1）。
//
// 正确时序：
//   渲染线程: 销毁 context → 还原 vtable → 置 done
//   注入器线程(UiShutdown 等待返回后): ReleaseOwnedResources()
//     —— 此时 vtable 已还原，没有任何线程再进钩子，释放 trampoline 池/清指针都安全。
//     （UninstallMouseWheelHook 在注入器线程做还更好：不用在渲染线程上等 pump 退出卡帧）
//
// 历史教训 1（「每热重载卡一点」）：最初是「第1帧还原 vtable、第2/3帧清理」——
//   还原 vtable 后没人再进钩子，第 2/3 步永远等不到帧 → 每次泄漏一整套 GPU 资源。
// 历史教训 2（「热重载闪退」）：把释放资源插到还原 vtable 之前 → 钩子退场路径
//   调空 origPresent → 崩。两个教训合起来就是一句话：
//   **"销毁在钩子内，脱钩是钩子的最后动作，释放资源在脱钩之后由外部线程做。"**
static void PumpUnloadIfRequested()
{
    if (InterlockedCompareExchange(&g_unloadRequested, 0, 0) == 0) return;

    LONG n = InterlockedIncrement(&g_unloadFrameCount);

    if (n == 1) {
        LogUi("[ui] 卸载第 1 步：销毁 imgui context（vtable 未还原，退路 origPresent 有效）");
        DestroyImguiOnRenderThread();
        return;
    }
    if (n == 2) {
        LogUi("[ui] 卸载第 2 步：还原 vtable（脱钩，此后游戏不再进本钩子）");
        RestoreVtables();
        // vtable 已还原 ⇒ 渲染线程本帧收尾（1047 快速路径）用的是栈上的 origPresent
        // 读值，且此后无人再进本函数 → 置完成标志安全
        InterlockedExchange((volatile LONG *)&g_shutdownDone, 1);
        g_unloadDoneTick = GetTickCount64();
        LogUi("[ui] ===== 渲染线程侧卸载完成（资源释放交给注入器线程）=====");
        return;
    }
    // n >= 3：已脱钩，理论上不会再进来；万一进来（竞态窗口），直接走快速路径退场
}

// =====================================================================
//  游戏主动退出时的"抢跑清理"（见 exit_guard.h / ui_api.h 的说明）
// =====================================================================
// 为什么必须单独有这么一个函数，而不是直接调 UiShutdown：
//   UiShutdown 是**为热重载卸载设计的**，它会等渲染线程最多 3 秒。
//   而游戏退出的时间线完全由游戏掌控（点 X 之后 Unity 会立刻开始拆自己的
//   图形栈并杀线程），我们必须**立刻**把两样东西收回来：
//     1) 全局 WH_MOUSE_LL 钩子 —— 它让系统输入线程同步等我们 pump 线程的回调，
//        而 pump 线程马上就会被 ExitProcess 杀掉；
//     2) dxgi 共享 vtable[8]/[13] —— 不还原的话，退出过程中的 Present 还会进
//        我们的镜像，去碰一个正在被拆掉的 imgui/D3D 状态。
//   这两步都很快且幂等，不需要等待任何线程。
// =====================================================================
void UiExitFastPath()
{
    InterlockedExchange(&g_unloadRequested, 1);   // HookPresent 立即走快速退场路径
    UninstallMouseWheelHook();                     // 1) 先卸全局鼠标钩子（<1s）
    RestoreVtables();                              // 2) 再让 dxgi 脱钩
    LogUi("[ui] 退出抢跑清理完成：全局鼠标钩子已卸、dxgi vtable 已还原");
}

// 供 hot_reload.cpp 的 EspUnload 调用（现在它只置标志，不做实际清理）
void UiShutdown()
{
    EspUiRequestShutdown();
    // 等待渲染线程完成"销毁 context + 还原 vtable"；游戏不渲染时最多等 3 秒。
    ULONGLONG t0 = GetTickCount64();
    while (!g_shutdownDone && (GetTickCount64() - t0) < 3000) {
        Sleep(20);
    }
    if (!g_shutdownDone)
    {
        // ★ 渲染线程没接住（窗口不可见 / 已停止 Present / 游戏正在退出）。
        //   必须在**这里**补上"还原 vtable"这一步，不能直接放手去释放资源：
        //   vtable[8] 还指着 HookPresent，而下面的 ReleaseOwnedResources 会把
        //   g_origPresent 置空 ⇒ 之后游戏每一帧 Present 都退化成 "返回 S_OK 但
        //   不真的 Present" ⇒ 画面冻结、窗口无响应（表现为"卡死"）。
        //   2026-10-14 复盘：这是"点 X 退出卡死"的一个独立成因。
        LogUi("[ui] WARN: 3s 内渲染线程未完成卸载 —— 由本线程补还原 vtable");
        RestoreVtables();
        InterlockedExchange((volatile LONG *)&g_shutdownDone, 1);
        g_unloadDoneTick = GetTickCount64();
    }
    else
        LogUi("[ui] UiShutdown：渲染线程已销毁 context 并脱钩");

    // ★ 资源释放必须在这里（注入器线程）做，且必须在脱钩之后：
    //   ReleaseOwnedResources 会清 g_origPresent / 释放 trampoline 池 / 卸滚轮钩子线程
    //   —— 钩子活着时做这些 = 退场路径调空指针（上一版闪退的直接原因）。
    //   此刻 vtable 已还原（或游戏根本没在渲染），无人再进钩子，安全。
    //   副带好处：UninstallMouseWheelHook 要等 pump 线程退出（最多 1s），
    //   在注入器线程等不卡渲染帧。
    ReleaseOwnedResources();

    // 残留自检：全为 0/null 才算干净（「每热重载卡一点」的验证指纹）。
    LogUi("[ui] 残留自检 dummySc=%p trampPool=%p uiReady=%d wheelHookThread=%p",
          (void *)g_dummySc, (void *)g_trampPool, g_uiReady ? 1 : 0, (void *)g_mouseHookThread);
}

// 给工具侧查询：清理是否真的完成了（决定能否安全 VirtualFreeEx）
extern "C" int UiShutdownCompleted()
{
    return g_shutdownDone ? 1 : 0;
}

bool UiIsReady() { return g_uiReady; }
unsigned long UiFrameCount() { return g_frame; }
HWND UiGameHwnd() { return g_gameHwnd; }