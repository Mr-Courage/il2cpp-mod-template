// il2cpp_runtime.h — 高层 IL2CPP 反射封装（平台无关）
// 用法：
//   1) il2cpp::runtime::Init()                 // 默认模块名：Windows=GameAssembly.dll, Android=libil2cpp.so
//   2) il2cpp::runtime::EnsureAttached()       // 在工作线程中使用前必须挂到 IL2CPP 域
//   3) 用 FindClass / ClassField / ClassMethod / RuntimeInvoke 做反射
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "il2cpp/il2cpp_types.h"
#include "il2cpp/il2cpp_api.h"

namespace il2cpp
{
namespace runtime
{

// 初始化：打开 IL2CPP 模块并解析全部导出。module_name 为空时使用平台默认值。
// 返回是否拿到了关键导出（il2cpp_domain_get）。可重复调用（幂等）。
bool Init(const char *module_name = nullptr);

// 是否已完成初始化且拿到这张导出表。
bool Ready();

// 对当前线程挂接 IL2CPP 域；返回是否已挂接成功。
bool EnsureAttached();

// 从当前线程解除挂接。
void Detach();

// 直接访问导出表（低层能力，谨慎使用）。
Il2CppApi &Api();

// ---- 域 / 程序集 / 镜像 ----
Il2CppDomain *Domain();
Il2CppAssembly *Assembly(const char *name);
Il2CppImage *Image(Il2CppAssembly *assembly);
Il2CppImage *Image(const char *assemblyName);
std::vector<Il2CppImage *> Images();

// ---- 类 ----
Il2CppClass *ClassFromNS(Il2CppImage *image, const char *ns, const char *name);
// 支持 "Torappu.Battle.BattleController" 或拆分传入(ns,name)。
Il2CppClass *FindClass(const char *ns, const char *name);
Il2CppClass *FindClass(const char *fullName);
std::string ClassFullName(Il2CppClass *klass);

// ---- 字段 ----
FieldInfo *ClassField(Il2CppClass *klass, const char *name);
bool ReadField(Il2CppObject *object, FieldInfo *field, void *outValue);
bool ReadStaticField(FieldInfo *field, void *outValue);
Il2CppObject *ReadFieldObject(Il2CppObject *object, FieldInfo *field);
bool WriteField(Il2CppObject *object, FieldInfo *field, const void *value);

// ---- 方法 ----
MethodInfo *ClassMethod(Il2CppClass *klass, const char *name, int argsCount = -1);
Il2CppObject *Invoke(MethodInfo *method, void *object, void **params);

// ---- 值工具 ----
std::string DotNetStringToUtf8(Il2CppObject *stringObj); // 对 Il2CppString* 调用
int32_t UnboxInt(Il2CppObject *boxed);

} // namespace runtime
} // namespace il2cpp