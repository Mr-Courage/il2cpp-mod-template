// il2cpp_hook.cpp — IL2CPP 方法挂钩实现（x64 函数体 detour）
#include "hook/il2cpp_hook.h"
#include "il2cpp/il2cpp_runtime.h"

#include <windows.h>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace il2cpp
{
namespace hook
{

namespace
{

LogFn g_log = nullptr;

void HLog(const char *fmt, ...)
{
    if (!g_log)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(buf);
}

// ---- 最小 x86-64 指令长度解析（只覆盖函数头常见 prologue 形态）----
int ModrmLen(const BYTE *p, int i)
{
    BYTE m = p[i];
    int mod = m >> 6, rm = m & 7, end = i + 1;
    if (mod != 3 && rm == 4)
    {
        // 有 SIB 字节
        end++;
        // mod=0 且 SIB.base=101 -> [disp32] 绝对寻址（**不是** RIP 相对，克隆时无需重定位）
        BYTE sib = p[i + 1];
        if (mod == 0 && (sib & 7) == 5)
            end += 4;
    }
    // ★ 注意：这里绝不能 return —— SIB 之外还要再算 mod 的位移字节
    //   （mod=01 disp8 / mod=02 disp32）。2026-10-12 踩过：早退导致
    //   `mov [rsp+8],rbx`（48 89 5C 24 08）被算成 3 字节而非 5 字节。
    if (mod == 1)
        end += 1;
    else if (mod == 2)
        end += 4;
    else if (mod == 0 && rm == 5)
        end += 4; // RIP-rel disp32
    return end;
}

// 返回指令总长度；无法解码返回 -1。rel=true 表示该指令是相对位移跳转/调用。
int InsnLen(const BYTE *c, int i, bool *rel)
{
    const int LIM = 15;
    int j = i;
    *rel = false;
    while (j < i + LIM)
    {
        BYTE pr = c[j];
        if (pr == 0xF0 || pr == 0xF2 || pr == 0xF3 || pr == 0x2E || pr == 0x36 || pr == 0x3E || pr == 0x26 ||
            pr == 0x64 || pr == 0x65 || pr == 0x66 || pr == 0x67)
        {
            j++;
            continue;
        }
        break;
    }
    bool rexW = false, has66 = false;
    if (j < i + LIM && c[j] >= 0x40 && c[j] <= 0x4F)
    {
        if (c[j] & 8)
            rexW = true;
        j++;
    }
    for (int k = i; k < j; ++k)
        if (c[k] == 0x66)
            has66 = true;
    if (j >= i + LIM)
        return -1;
    int o = j;
    BYTE b = c[o];
    // 相对跳转：须在操作码起点判定
    if (b == 0xE8 || b == 0xE9)
    {
        *rel = true;
        return o + 5 - i;
    }
    if (b == 0xEB)
    {
        *rel = true;
        return o + 2 - i;
    }
    if (b >= 0x70 && b <= 0x7F)
    {
        *rel = true;
        return o + 2 - i;
    }
    if (b == 0x0F)
    {
        if (c[o + 1] >= 0x80 && c[o + 1] <= 0x8F)
        {
            *rel = true;
            return o + 6 - i;
        }
        return -1;
    }
    switch (b)
    {
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
    if (b >= 0x50 && b <= 0x5F)
        return o + 1 - i; // push/pop reg
    if (b >= 0x91 && b <= 0x96)
        return o + 1 - i; // xchg reg,rax
    if (b >= 0xB8 && b <= 0xBF)
        return o + 1 + (rexW ? 8 : (has66 ? 2 : 4)) - i;
    switch (b)
    {
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E:
    case 0x8F: case 0x63: case 0xFF: case 0x01: case 0x03: case 0x29: case 0x2B:
        return ModrmLen(c, o + 1) - i;
    default: break;
    }
    return -1;
}

// 指令分类：返回 rsp 净变化（push=-8 / pop=+8 / sub rsp,imm=+imm / add rsp,imm=-imm / 其他=0）；
// 不可钩（ret / call-indirect）返回 INT_MIN 哨兵。
int ClassifyInsn(const BYTE *c, int i, int *lenOut)
{
    int k = i;
    for (;;)
    {
        BYTE p = c[k];
        if (p == 0xF0 || p == 0xF2 || p == 0xF3 || p == 0x2E || p == 0x36 || p == 0x3E || p == 0x26 ||
            p == 0x64 || p == 0x65 || p == 0x66 || p == 0x67)
        {
            k++;
            continue;
        }
        break;
    }
    if (c[k] >= 0x40 && c[k] <= 0x4F)
        k++; // REX
    BYTE op = c[k];
    if (op >= 0x50 && op <= 0x57) { *lenOut = k + 1 - i; return -8; }   // push
    if (op >= 0x58 && op <= 0x5F) { *lenOut = k + 1 - i; return 8; }    // pop
    if (op == 0xE8) { *lenOut = k + 5 - i; return 0; }                  // call rel32（克隆时重定位）
    if (op == 0xC3 || op == 0xC2 || op == 0xCA || op == 0xCB) return INT_MIN;
    if (op == 0xFF && (((c[k + 1] >> 3) & 7) == 2)) return INT_MIN;     // call-indirect
    if (op == 0x83 || op == 0x81)                                       // add/sub rsp,imm
    {
        BYTE m = c[k + 1];
        int reg = (m >> 3) & 7;
        if (reg == 0 || reg == 5)
        {
            int mod = m >> 6, rm = m & 7;
            bool isRsp = false;
            if (rm == 4)
            {
                if (mod == 3)
                    isRsp = true;                                    // mod=3,rm=4 -> rsp 寄存器
                else
                {
                    BYTE sib = c[k + 2];
                    isRsp = ((sib & 7) == 4);                        // SIB base=rsp
                }
            }
            if (isRsp)
            {
                bool is32 = (op == 0x81);
                *lenOut = k + 1 + (is32 ? 5 : 2) - i;
                int imm = is32 ? *(int *)(c + k + 2) : (signed char)c[k + 2];
                return (reg == 5) ? -imm : imm;                      // sub rsp -> rsp 减少
            }
        }
    }
    *lenOut = 0;                                                        // 其他无 rsp 副作用
    return 0;
}

// 返回指令内 RIP 相对 disp32 相对 i 的偏移；无则 -1。
// ★ 必须与 InsnLen 支持的 ModRM 指令集合保持一致 —— 凡是 InsnLen 能算出长度的
//   ModRM 指令（即能被克隆的），这里都要能识别出它的 RIP 相对操作数。
//   否则克隆进 trampoline 后，[rip+disp] 会算到错误地址 → AV。
//   （2026-10-12 实测：BattleFactory::CreateEnemy 的 prologue 里
//    `cmp byte ptr [rip+0x83A875C],0` 被原样克隆 → 崩在 tramp+0xA。）
int RipRelDispOff(const BYTE *c, int i)
{
    int j = i;
    for (;;)
    {
        BYTE p = c[j];
        if (p == 0xF0 || p == 0xF2 || p == 0xF3 || p == 0x2E || p == 0x36 || p == 0x3E || p == 0x26 ||
            p == 0x64 || p == 0x65 || p == 0x66 || p == 0x67)
        {
            j++;
            continue;
        }
        break;
    }
    if (c[j] >= 0x40 && c[j] <= 0x4F)
        j++; // REX
    int o = j;
    BYTE b = c[o];
    bool modrmInsn = false;
    switch (b)
    {
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x01: case 0x03: case 0x29: case 0x2B: case 0x63: case 0xFF:
    case 0x69: case 0x6B:
    case 0x80: case 0x81: case 0x82: case 0x83: case 0xC0: case 0xC1: case 0xC6: case 0xC7:
        modrmInsn = true;
        break;
    default:
        break;
    }
    if (!modrmInsn)
        return -1;
    BYTE m = c[o + 1];
    if ((m >> 6) == 0 && (m & 7) == 5)   // mod=00, r/m=101 -> RIP+disp32（无 SIB）
        return o + 2 - i;
    return -1;
}

// 申请一块距 target 在 ±2GB 内的可执行内存 —— 这样克隆指令里的 RIP 相对 disp32
// 可以直接用 int32 加法平移（远了就表达不了）。
BYTE *AllocNear(void *target, size_t size)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity ? (uintptr_t)si.dwAllocationGranularity : 0x10000;
    uintptr_t t = (uintptr_t)target;
    const uintptr_t RANGE = 0x70000000ULL;      // 1.75GB，留足余量
    uintptr_t lo = (t > RANGE) ? (t - RANGE) : gran;
    uintptr_t hi = t + RANGE;

    uintptr_t addr = (lo + gran - 1) & ~(gran - 1);
    int guard = 0;
    while (addr < hi && guard++ < 100000)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != sizeof(mbi))
            break;
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        uintptr_t end = base + mbi.RegionSize;
        if (end <= addr)                    // 防呆：RegionSize 异常
            break;
        if (mbi.State == MEM_FREE)
        {
            uintptr_t cand = (addr + gran - 1) & ~(gran - 1);
            if (cand >= lo && cand + size <= end)
            {
                void *p = VirtualAlloc((LPVOID)cand, size, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
                if (p)
                    return (BYTE *)p;
            }
        }
        addr = end;
    }
    return nullptr;
}

// =============================================================================
//  原始字节保存表（支撑热重载的函数体还原）
//
//  为什么必须：函数体 detour 把 12+字节跳板写进**目标模块的代码页**
//  （GameAssembly 里）。热重载时我们只释放自己的镜像，
//  那些写进 GameAssembly 的跳板**没人还原** —— 后果是：
//    1) 旧跳板还指向已释放的旧镜像 -> 旧钩子仍在生效（计数还在涨）
//    2) 新一次 InstallDetour 把**已被改写的字节**当正常指令解析
//       -> insns 从 8 变成 2，装出残缺 hook，而且**返回 true 看起来成功**
//  （2026-10-11 实测：连续三次热重载后 class_from_name 命中数 1569 -> 777 -> 0）
//
//  所以：InstallDetour 时把被覆盖的原始字节存下来，RemoveDetour 原样写回。
//  一个 target 只允许有一个记录（重复 Install 前必须先 Remove）。
// =============================================================================
namespace {

struct SavedPatch
{
    void *target;        // 被改写的函数地址
    int   len;                // 覆盖了多少字节
    void *detour;             // 装上去的钩子（用来核对，防误还原）
    void *tramp;              // 克隆出来的跳板（VirtualAlloc 的），撤钩时要一起释放
    BYTE  orig[32];           // 原始字节（kLen <= 32，InstallDetour 已保证）
    bool  used;
};
SavedPatch g_saved[16] = {};
CRITICAL_SECTION g_savedLock;
bool g_savedLockReady = false;

CRITICAL_SECTION *SavedLock()
{
    if (!g_savedLockReady)
    {
        InitializeCriticalSection(&g_savedLock);
        g_savedLockReady = true;
    }
    return &g_savedLock;
}

SavedPatch *FindSaved(void *target, void *detour)
{
    for (int i = 0; i < 16; ++i)
        if (g_saved[i].used && g_saved[i].target == target &&
            (detour == nullptr || g_saved[i].detour == detour))
            return &g_saved[i];
    return nullptr;
}

} // namespace

bool InstallDetour(void *target, void *detour, void **outTramp)
{
    BYTE *src = (BYTE *)target;
    int i = 0, netRsp = 0, nInsn = 0;
    while (i < 16)
    {
        bool rel = false;
        int L = InsnLen(src, i, &rel);
        if (L < 0)
        {
            HLog("undecodable insn @%d; abort detour", i);
            return false;
        }
        if (rel && !(L == 5 && (src[i] == 0xE8 || src[i] == 0xE9)))
        {
            HLog("rel insn @%d without reloc support; abort detour", i);
            return false;
        }
        int L2 = 0;
        int delta = ClassifyInsn(src, i, &L2);
        if (delta == INT_MIN)
        {
            HLog("ret/call-indirect @%d; abort detour", i);
            return false;
        }
        if (L2)
            L = L2;
        netRsp += delta;
        i += L;
        nInsn++;
        if (i >= 12)
            break; // 至少覆盖 12 字节跳板
    }
    if (i < 12)
    {
        HLog("prologue too short (%d bytes); abort detour", i);
        return false;
    }
    if (i > 32)
    {
        HLog("prologue too long (%d bytes); abort detour", i);
        return false;
    }
    const int kLen = i;

    // trampoline 预算放宽：最坏情况（前 32 字节全是 call rel32）会膨胀到约 200 字节。
    // ★ 优先申请「距 target ±2GB 内」的内存：克隆指令若含 RIP 相对操作数，disp32 才能用
    //   int32 平移修好。拿不到近地址时退化为普通 VirtualAlloc —— 此时若 prologue 真含
    //   RIP 相对指令，下面会因 disp 溢出而**放弃装钩**（宁可失败也不能装出会崩的钩子）。
    BYTE *tramp = AllocNear(target, 512);
    bool trampNear = (tramp != nullptr);
    if (!tramp)
        tramp = (BYTE *)VirtualAlloc(NULL, 512, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
    {
        HLog("VirtualAlloc for trampoline failed");
        return false;
    }

    // 逐条克隆。trampoline 内的跳转一律用「jmp/push qword ptr [rip+disp]」+ 内联绝对地址，
    // 不占用任何通用寄存器：被克隆的 prologue 指令可能在 RAX/R11 等寄存器里留下活跃值，
    // 若用 mov reg,imm64 回跳会把它覆盖掉，导致原函数从 src+kLen 继续执行时读到错误数据。
    int off = 0, p = 0;
    while (p < kLen)
    {
        bool rel = false;
        int L = InsnLen(src, p, &rel);
        int L2 = 0;
        ClassifyInsn(src, p, &L2);
        if (L2)
            L = L2;
        if (rel && L == 5 && src[p] == 0xE8)
        {
            int64_t tgt = (int64_t)(src + p + 5) + (int32_t)(*(int32_t *)(src + p + 1));
            // push qword [rip+10] ; jmp qword [rip+12] ; 4B pad ; 伪返回地址 ; 目标地址
            tramp[off] = 0xFF; tramp[off + 1] = 0x35; *(int32_t *)(tramp + off + 2) = 10;
            tramp[off + 6] = 0xFF; tramp[off + 7] = 0x25; *(int32_t *)(tramp + off + 8) = 12;
            for (int t = 12; t < 16; ++t)
                tramp[off + t] = 0x90;
            // 伪返回地址取 src+kLen（落在原模块图像内），避免 ret 落到堆区 trampoline
            // 触发 dxgi/反作弊的返回地址校验。
            *(UINT64 *)(tramp + off + 16) = (UINT64)(src + kLen);
            *(UINT64 *)(tramp + off + 24) = (UINT64)tgt;
            off += 32;
        }
        else if (rel && L == 5 && src[p] == 0xE9)
        {
            int64_t tgt = (int64_t)(src + p + 5) + (int32_t)(*(int32_t *)(src + p + 1));
            tramp[off] = 0xFF; tramp[off + 1] = 0x25; *(int32_t *)(tramp + off + 2) = 0;
            *(UINT64 *)(tramp + off + 6) = (UINT64)tgt;
            off += 14;
        }
        else
        {
            memcpy(tramp + off, src + p, L);
            // ★ RIP 相对内存操作数必须重定位：克隆到别处后 [rip+disp] 会指向错误地址。
            //   保持「有效地址不变」：new = old + (src+p) - (tramp+off)
            int d = RipRelDispOff(src, p);
            if (d >= 0 && d + 4 <= L)
            {
                int64_t oldDisp = (int32_t)(*(int32_t *)(src + p + d));
                int64_t delta = (int64_t)(src + p) - (int64_t)(tramp + off);
                int64_t newDisp = oldDisp + delta;
                if (newDisp < INT_MIN || newDisp > INT_MAX)
                {
                    HLog("rip-rel disp overflow (near=%d) at +%d; abort detour", (int)trampNear, p);
                    VirtualFree(tramp, 0, MEM_RELEASE);
                    return false;
                }
                *(int32_t *)(tramp + off + d) = (int32_t)newDisp;
                HLog("rip-rel relocated +%d: disp %lld -> %lld", p, (long long)oldDisp,
                     (long long)newDisp);
            }
            off += L;
        }
        p += L;
    }
    // 克隆段执行完，jmp qword ptr [rip+0] + 绝对地址 回跳原函数体 src+kLen
    tramp[off] = 0xFF; tramp[off + 1] = 0x25; *(int32_t *)(tramp + off + 2) = 0;
    *(UINT64 *)(tramp + off + 6) = (UINT64)(src + kLen);
    off += 14;

    // 安装跳板
    DWORD old = 0;
    // ---- 先存下原始字节（供RemoveDetour 还原）----
    {
        CRITICAL_SECTION *lk = SavedLock();
        EnterCriticalSection(lk);
        SavedPatch *sp = FindSaved(target, nullptr);
        if (!sp)
        {
            for (int i = 0; i < 16; ++i)
                if (!g_saved[i].used) { sp = &g_saved[i]; break; }
        }
        if (!sp)
        {
            // 表满：宁可不装，也不要留下无法还原的补丁
            LeaveCriticalSection(lk);
            HLog("saved-patch table full; abort detour (target=%p)", target);
            VirtualFree(tramp, 0, MEM_RELEASE);   // 没装上，跳板也别留着
            return false;
        }
        sp->target = target;
        sp->len    = kLen;
        sp->detour = detour;
        sp->tramp  = tramp;
        memcpy(sp->orig, src, kLen);
        sp->used   = true;
        LeaveCriticalSection(lk);
    }

    VirtualProtect(src, kLen, PAGE_EXECUTE_READWRITE, &old);
    src[0] = 0x48; src[1] = 0xB8; *(UINT64 *)(src + 2) = (UINT64)detour;   // mov rax, detour
    src[10] = 0xFF; src[11] = 0xE0;                                        // jmp rax
    for (int t = 12; t < kLen; ++t)
        src[t] = 0x90;                                                     // NOP 填充
    VirtualProtect(src, kLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), src, kLen);

    HLog("detour installed: target=%p kLen=%d insns=%d netRsp=%d tramp=%p", target, kLen, nInsn, netRsp,
         (void *)tramp);
    *outTramp = (void *)tramp;
    return true;
}

} // namespace

// 按 "命名空间.类名" 解析类。刻意只用定长 C 缓冲 + il2cpp 导出表，不碰 std::string/std::vector：
// 一旦依赖 libstdc++ 的 STL，链接器会连带拉入 EH/emutls 目标文件，而 libgcc 的 emutls 又引用
// pthread，最终给 DLL 造出静态 TLS 段（.tls + TLS 回调）——本模块的目标注入方式是「手动映射」，
// 不模拟静态 TLS，因此必须保持产物零 .tls。
static Il2CppClass *FindClassC(const char *fullName)
{
    char ns[256] = {0};
    char nm[256] = {0};
    const char *dot = strrchr(fullName, '.');
    if (dot)
    {
        size_t n = (size_t)(dot - fullName);
        if (n >= sizeof(ns))
            n = sizeof(ns) - 1;
        memcpy(ns, fullName, n);
        ns[n] = 0;
        strncpy(nm, dot + 1, sizeof(nm) - 1);
    }
    else
    {
        strncpy(nm, fullName, sizeof(nm) - 1);
    }

    Il2CppApi &api = runtime::Api();
    if (!api.il2cpp_domain_get_assemblies || !api.il2cpp_class_from_name)
        return nullptr;
    size_t count = 0;
    Il2CppAssembly **assemblies = api.il2cpp_domain_get_assemblies(runtime::Domain(), &count);
    if (!assemblies)
        return nullptr;
    for (size_t i = 0; i < count; ++i)
    {
        Il2CppImage *img = api.il2cpp_assembly_get_image ? api.il2cpp_assembly_get_image(assemblies[i]) : nullptr;
        if (!img)
            continue;
        Il2CppClass *k = api.il2cpp_class_from_name(img, ns, nm);
        if (k)
            return k;
    }
    return nullptr;
}

void SetLogger(LogFn fn)
{
    g_log = fn;
}

// 还原某个函数体的原始字节（撤掉 detour）。成功返回 true。
// 由热重载的 EspUnload 调用 —— 必须在镜像被释放之前！
bool RemoveDetour(void *target)
{
    CRITICAL_SECTION *lk = SavedLock();
    EnterCriticalSection(lk);
    SavedPatch *sp = FindSaved(target, nullptr);
    if (!sp)
    {
        LeaveCriticalSection(lk);
        return false;
    }
    int kLen = sp->len;
    BYTE orig[32];
    memcpy(orig, sp->orig, kLen);
    void *tramp = sp->tramp;
    sp->used = false;                       // 先释放记录，避免还原失败时卡在表里
    LeaveCriticalSection(lk);

    DWORD old = 0;
    if (!VirtualProtect(target, kLen, PAGE_EXECUTE_READWRITE, &old))
    {
        HLog("RemoveDetour: VirtualProtect failed (target=%p err=%lu)", target, GetLastError());
        return false;
    }
    memcpy(target, orig, kLen);
    VirtualProtect(target, kLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, kLen);
    // 跳板是独立 VirtualAlloc 的，不在镜像里 —— 不释放就是每次热重载漏 512 字节。
    // 必须在字节还原之后：还原前别的线程还可能正在跳板里执行。
    if (tramp) VirtualFree(tramp, 0, MEM_RELEASE);
    HLog("RemoveDetour: restored %d bytes at %p (tramp %p freed)", kLen, target, tramp);
    return true;
}

int RemoveAllDetours()
{
    int n = 0;
    for (;;)
    {
        void *t = nullptr;
        CRITICAL_SECTION *lk = SavedLock();
        EnterCriticalSection(lk);
        for (int i = 0; i < 16; ++i)
            if (g_saved[i].used) { t = g_saved[i].target; break; }
        LeaveCriticalSection(lk);
        if (!t) break;
        if (!RemoveDetour(t)) break;
        ++n;
    }
    return n;
}

void *Detour(void *target, void *detour)
{
    if (!target || !detour)
        return nullptr;
    void *tramp = nullptr;
    if (!InstallDetour(target, detour, &tramp))
        return nullptr;
    return tramp;
}

void *ResolveMethod(const char *classFullName, const char *methodName, int argsCount)
{
    if (!classFullName || !methodName)
        return nullptr;
    // 空串也早退：否则会拿空/乱码类名去 il2cpp_class_from_name 反复 probe 刷屏。
    if (!classFullName[0] || !methodName[0])
    {
        HLog("ResolveMethod: empty class/method name, skip");
        return nullptr;
    }
    if (!runtime::Ready())
    {
        HLog("il2cpp not ready; cannot resolve %s::%s", classFullName, methodName);
        return nullptr;
    }

    Il2CppClass *klass = FindClassC(classFullName);
    if (!klass)
    {
        HLog("class not found: %s", classFullName);
        return nullptr;
    }

    MethodInfo *mi = runtime::ClassMethod(klass, methodName, argsCount);
    if (!mi)
    {
        HLog("method not found: %s::%s(%d)", classFullName, methodName, argsCount);
        return nullptr;
    }

    void *ptr = ((MethodInfoHeader *)mi)->methodPointer;
    if (!ptr)
    {
        HLog("method has null code pointer (abstract/generic/hotfix?): %s::%s", classFullName, methodName);
        return nullptr;
    }
    HLog("resolved %s::%s(%d) -> %p", classFullName, methodName, argsCount, ptr);
    return ptr;
}

void *HookMethod(const char *classFullName, const char *methodName, int argsCount, void *detour)
{
    void *target = ResolveMethod(classFullName, methodName, argsCount);
    if (!target)
        return nullptr;
    void *tramp = Detour(target, detour);
    if (tramp)
        HLog("hooked %s::%s(%d) target=%p tramp=%p", classFullName, methodName, argsCount, target, tramp);
    else
        HLog("detour install FAILED: %s::%s target=%p", classFullName, methodName, target);
    return tramp;
}

} // namespace hook
} // namespace il2cpp