// probe.h — 反射自测 / 类结构 dump（验证反射链路用，与具体游戏无关）
// ============================================================================
// 引导完成后框架会自动跑 ReflectionSelfTest()：枚举全部程序集、打印镜像名与
// 类总数 —— 只要它打得出来，就证明「IL2CPP 导出解析 → 域挂接 → 反射」整条链路是通的。
//
// ReflectDumpClass() 是给你交互式排查用的：把 mod.ini 里的 dump_class 填上
// 「命名空间.类名」，下次注入就会把该类的继承链 + 全部字段（类型名/偏移/当前值）
// 打进日志。这是「拿不到 dump.cs 时」定位字段偏移的正路。
// ============================================================================
#pragma once

#include "il2cpp/il2cpp_types.h"

// 沿父类链找名为 fieldName 的静态字段；返回 FieldInfo*，并输出找到它的那个类。
// 泛型单例（SingletonMonoBehaviour<T>.s_instance）就靠这个拿到实例。
FieldInfo *FindStaticFieldInParents(Il2CppClass *klass, const char *fieldName,
                                    Il2CppClass **ownerOut);

// 枚举程序集/镜像并打印，验证反射链路。返回镜像总数（-1 = 反射不可用）。
int ReflectionSelfTest();

// dump 一个类的结构。fullName 支持 "Ns.Name" 或 "Name"（后者按空命名空间找）。
// maxFields：最多打印多少个字段（0 = 不限）。instance 非空时一并打印字段当前值。
void ReflectDumpClass(const char *fullName, void *instance, int maxFields);
