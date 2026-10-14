// selftest.cpp — il2cpp_core detour 引擎离线自测（不需要游戏 / il2cpp 运行时）
//
// 编译运行：
//   cmake -B build && cmake --build build && ./build/il2cpp_selftest
// 或直接：
//   g++ -std=c++17 -O2 -I src tools/selftest.cpp src/hook/il2cpp_hook.cpp \
//       src/il2cpp/il2cpp_api.cpp src/il2cpp/il2cpp_runtime.cpp \
//       src/platform/il2cpp_module_windows.c -o selftest
//
// 覆盖点：trampoline 克隆 + 回跳必须保持被克隆指令留下的活跃寄存器值。
// 这里 TargetAdd 的 prologue 会在 RAX 中装入参数 a，随后由 src+kLen 处的
// lea (%rdx,%rax,2) 使用；若回跳用 mov rax,imm64 便会破坏该值（历史缺陷）。
#include <windows.h>
#include <cstdio>

#include "hook/il2cpp_hook.h"

static volatile int g_hits = 0;

// 期望：TargetAdd(3,4) = 3*2 + 4 + 7 = 17
__attribute__((noinline))
static int TargetAdd(int a, int b)
{
    volatile int k = a;
    int s = k * 2;
    s += b;
    volatile int z = s;
    return z + 7;
}

typedef int (*AddFn)(int, int);
static AddFn g_origAdd = nullptr;

static int DetourAdd(int a, int b)
{
    g_hits++;
    return g_origAdd(a, b);
}

int main()
{
    il2cpp::hook::SetLogger([](const char *s) { printf("[hook] %s\n", s); });

    int direct = TargetAdd(3, 4);
    printf("direct: TargetAdd(3,4) = %d\n", direct);

    void *tramp = il2cpp::hook::Detour((void *)TargetAdd, (void *)DetourAdd);
    if (!tramp)
    {
        printf("RESULT: FAIL - detour install rejected\n");
        return 2;
    }
    g_origAdd = (AddFn)tramp;

    int hooked = TargetAdd(3, 4);
    printf("hooked: TargetAdd(3,4) = %d, hits = %d\n", hooked, (int)g_hits);

    bool ok = (direct == 17) && (hooked == 17) && (g_hits == 1);
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}