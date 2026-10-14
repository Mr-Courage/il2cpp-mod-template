// il2cpp_api.h — IL2CPP 运行时导出函数表与解析器
// Il2CppApi 结构体即完整的一张导出表（成员名与 il2cpp_* 同名）。
// 平台差异只在解析层：对外只需拿到一个已加载的模块句柄即可填充。
#pragma once

#include "il2cpp/il2cpp_types.h"

#define DO_API(r, n, p) r (*n) p;
#define DO_API_NO_RETURN(r, n, p) DO_API(r, n, p)

struct Il2CppApi
{
#include "il2cpp/il2cpp_api_functions.h"
};

// ============================================================================
//  **ABI 哨兵**：往上面的 DO_API 列表增删成员后，必须让**所有**引用Il2CppApi
//  的 .o 全部重编，否则会静默崩溃。
//
//  原因（2026-10-10 实际踩过）：il2cpp_api_functions.h 是被 #include 进结构体的，
//  每个 TU 各自展开一份。若只重编了 il2cpp_api.o（写入者）而没重编
//  il2cpp_runtime.o（读者），两者的字段偏移就不一致 ——
//  `g_api.il2cpp_domain_get` 在旧对象里按旧偏移读，拿到的是**另一个函数的指针**，
//  一调就崩，而且崩在现场之外，极难定位。
//
//  做法：这里记录成员总数；il2cpp_api.cpp（唯一填充 g_api 的 TU）在编译期
//  用另一个常量交叉验证。改列表而漏编时，两处数字不一致 -> 编译直接失败。
// ============================================================================
#define IL2CPP_API_MEMBER_COUNT_2026_10_10  83   // <-- 增删 DO_API 后同步改这里
// 2026-10-12: +1 = il2cpp_class_get_methods（枚举类方法，用于挑出非泛型重载）
// sizeof(Il2CppApi) = 成员数 * sizeof(函数指针)，x64 上函数指针恒为 8 字节。
// 放在结构体定义**之后**，任何 TU 只要编进来就会带上这个常量。
// 真正的防线在 build.ps1：它比对各 .o 的时间戳与本头文件的时间戳 ——
// 头更新而某个 .o 更旧 = 漏编，直接报错。详见 build.ps1 的CheckApiAbi。
static_assert(sizeof(Il2CppApi) == IL2CPP_API_MEMBER_COUNT_2026_10_10 * sizeof(void (*)(void)),
              "Il2CppApi 成员数与哨兵不一致：改了 il2cpp_api_functions.h 的 DO_API 列表"
              "就必须同步改 IL2CPP_API_MEMBER_COUNT_2026_10_10，并**全量重编**所有引用者"
              "（漏编会导致字段偏移错位、调用野函数崩溃）");

#undef DO_API
#undef DO_API_NO_RETURN

// 从已打开的模块句柄解析全部导出；逐个可空（版本差异），返回成功解析的数量。
int il2cpp_api_resolve(Il2CppApi *api, void *module_handle);
// 全部置空。
void il2cpp_api_reset(Il2CppApi *api);