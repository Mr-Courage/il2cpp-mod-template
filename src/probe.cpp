// probe.cpp — 反射自测 / 类结构 dump
// ============================================================================
// 全部走 il2cpp 官方反射 API 拿偏移，**不按裸偏移读运行时结构**
// （MethodInfo / Il2CppClass 的内部布局随 Unity 版本变，硬编码必错）。
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "il2cpp/il2cpp_runtime.h"
#include "mod_config.h"
#include "probe.h"

namespace ir = il2cpp::runtime;

// 主模块提供（src/main.cpp）
extern void Log(const char *fmt, ...);

// ---------------------------------------------------------------- 小工具

// 把一个 Il2CppString* 打成可读文本（只用于日志）
static void LogIl2CppString(const char *label, void *strObj)
{
    auto &api = ir::Api();
    if (!strObj)
    {
        Log("    %-32s = (null)", label);
        return;
    }
    Il2CppString *s = (Il2CppString *)strObj;
    int32_t len = api.il2cpp_string_length ? api.il2cpp_string_length(s) : -1;
    Il2CppChar *ch = api.il2cpp_string_chars ? api.il2cpp_string_chars(s) : nullptr;
    if (len < 0 || !ch)
    {
        Log("    %-32s = %p (无法取字符串内容)", label, strObj);
        return;
    }
    // 截断，避免刷屏；非 ASCII 打成 '?'（il2cpp_string_chars 返回的是 UTF-16）
    char buf[256];
    int n = len < 200 ? len : 200;
    for (int i = 0; i < n; ++i)
        buf[i] = (char)(ch[i] < 0x80 ? ch[i] : '?');
    buf[n] = 0;
    Log("    %-32s = \"%s\" (len=%d, obj=%p)", label, buf, (int)len, strObj);
}

// 判断字段类型是不是「能直接当指针打印」的引用类型
static bool IsRefType(const char *typeName)
{
    if (!typeName) return false;
    // 值类型按值读；其余（class / System.* / List / Dictionary 等）打印对象地址
    static const char *kValuePrefix[] = {
        "int", "uint", "float", "double", "bool", "char", "short", "byte", "long",
        "System.Int", "System.UInt", "System.Boolean", "System.Single", "System.Double",
        "System.Char", "System.Byte", "System.SByte", "System.Int16", "System.Int64",
    };
    for (size_t i = 0; i < sizeof(kValuePrefix) / sizeof(kValuePrefix[0]); ++i)
        if (strncmp(typeName, kValuePrefix[i], strlen(kValuePrefix[i])) == 0)
            return false;
    return true;
}

// ---------------------------------------------------------------- 字段 dump

static void DumpFieldValue(Il2CppClass *owner, FieldInfo *f, Il2CppObject *inst,
                           const char *note)
{
    auto &api = ir::Api();
    const char *fname = api.il2cpp_field_get_name ? api.il2cpp_field_get_name(f) : "?";
    Il2CppType *ft = api.il2cpp_field_get_type ? api.il2cpp_field_get_type(f) : nullptr;
    const char *ftName = (ft && api.il2cpp_type_get_name) ? api.il2cpp_type_get_name(ft) : nullptr;
    size_t off = api.il2cpp_field_get_offset ? api.il2cpp_field_get_offset(f) : 0;

    // 静态字段用偏移最高位标记
    bool isStatic = (off & (size_t)0x80000000) != 0;
    size_t realOff = off & ~(size_t)0x80000000;

    if (isStatic)
    {
        uint64_t buf = 0;
        if (api.il2cpp_field_static_get_value)
            api.il2cpp_field_static_get_value(f, &buf);

        if (ftName && (strcmp(ftName, "int") == 0 || strcmp(ftName, "System.Int32") == 0))
            Log("  static %-32s (off=0x%02X) int   = %d", fname, (unsigned)realOff, (int)(int32_t)(uint32_t)buf);
        else if (ftName && (strcmp(ftName, "float") == 0 || strcmp(ftName, "System.Single") == 0))
        {
            float v; memcpy(&v, &buf, 4);
            Log("  static %-32s (off=0x%02X) float = %f", fname, (unsigned)realOff, v);
        }
        else if (ftName && (strcmp(ftName, "bool") == 0 || strcmp(ftName, "System.Boolean") == 0))
            Log("  static %-32s (off=0x%02X) bool  = %d", fname, (unsigned)realOff, buf != 0);
        else
            Log("  static %-32s (off=0x%02X) %-28s = %p   %s", fname, (unsigned)realOff,
                ftName ? ftName : "?", (void *)buf, note ? note : "");
        return;
    }

    if (!inst)
    {
        Log("  inst   %-32s (off=0x%02X) %-28s  [无实例]", fname, (unsigned)realOff,
            ftName ? ftName : "?");
        return;
    }

    // 偏移合理性检查：拿到的偏移必须有意义，否则按裸偏移读会读到别的字段
    if (realOff == 0 || realOff > 0x2000)
    {
        Log("  inst   %-32s (off=0x%02X) %-28s  [偏移异常，跳过取值]", fname,
            (unsigned)realOff, ftName ? ftName : "?");
        return;
    }

    if (ftName && (strcmp(ftName, "int") == 0 || strcmp(ftName, "System.Int32") == 0))
    {
        int v = 0;
        if (api.il2cpp_field_get_value) api.il2cpp_field_get_value(inst, f, &v);
        Log("  inst   %-32s (off=0x%02X) int   = %d   %s", fname, (unsigned)realOff, v,
            note ? note : "");
    }
    else if (ftName && (strcmp(ftName, "bool") == 0 || strcmp(ftName, "System.Boolean") == 0))
    {
        bool v = false;
        if (api.il2cpp_field_get_value) api.il2cpp_field_get_value(inst, f, &v);
        Log("  inst   %-32s (off=0x%02X) bool  = %d   %s", fname, (unsigned)realOff, v ? 1 : 0,
            note ? note : "");
    }
    else if (ftName && (strcmp(ftName, "float") == 0 || strcmp(ftName, "System.Single") == 0))
    {
        float v = 0;
        if (api.il2cpp_field_get_value) api.il2cpp_field_get_value(inst, f, &v);
        Log("  inst   %-32s (off=0x%02X) float = %f   %s", fname, (unsigned)realOff, v,
            note ? note : "");
    }
    else if (ftName && strcmp(ftName, "System.String") == 0)
    {
        void *sv = 0;
        if (api.il2cpp_field_get_value) api.il2cpp_field_get_value(inst, f, &sv);
        LogIl2CppString(fname, sv);
    }
    else if (IsRefType(ftName))
    {
        // 引用类型只打印对象地址 —— 不去调它的 ToString（可能有副作用）
        void *ov = 0;
        if (api.il2cpp_field_get_value) api.il2cpp_field_get_value(inst, f, &ov);
        Log("  inst   %-32s (off=0x%02X) %-28s = %p   %s", fname, (unsigned)realOff,
            ftName ? ftName : "?", ov, note ? note : "");
    }
    else
    {
        Log("  inst   %-32s (off=0x%02X) %-28s   %s", fname, (unsigned)realOff,
            ftName ? ftName : "?", note ? note : "");
    }
}

// ---------------------------------------------------------------- 对外接口

FieldInfo *FindStaticFieldInParents(Il2CppClass *klass, const char *fieldName,
                                    Il2CppClass **ownerOut)
{
    auto &api = ir::Api();
    if (!api.il2cpp_class_get_field_from_name) return nullptr;
    for (Il2CppClass *c = klass; c;
         c = api.il2cpp_class_get_parent ? api.il2cpp_class_get_parent(c) : nullptr)
    {
        FieldInfo *f = api.il2cpp_class_get_field_from_name(c, fieldName);
        if (f)
        {
            if (ownerOut) *ownerOut = c;
            return f;
        }
    }
    return nullptr;
}

int ReflectionSelfTest()
{
    auto &api = ir::Api();

    if (!api.il2cpp_domain_get_assemblies)
    {
        Log("[reflect] 缺少 il2cpp_domain_get_assemblies 导出，跳过自测");
        return -1;
    }

    // ★ 这里刻意不用 ir::Images()：它返回 std::vector，会把 libstdc++ 的
    //   EH / emutls / pthread 连带拖进来，破坏「零静态 TLS」这个硬约束。
    size_t asmCount = 0;
    Il2CppAssembly **asms = api.il2cpp_domain_get_assemblies(ir::Domain(), &asmCount);
    if (!asms)
    {
        Log("[reflect] domain_get_assemblies 返回空");
        return -1;
    }

    long classes = 0;
    for (size_t i = 0; i < asmCount; ++i)
    {
        Il2CppImage *img =
            api.il2cpp_assembly_get_image ? api.il2cpp_assembly_get_image(asms[i]) : nullptr;
        if (!img) continue;
        if (api.il2cpp_image_get_class_count)
            classes += (long)api.il2cpp_image_get_class_count(img);
        if (i < (size_t)MOD_SELFTEST_IMAGES)
        {
            const char *nm = api.il2cpp_image_get_name ? api.il2cpp_image_get_name(img) : "?";
            Log("  image[%d] = %s", (int)i, nm ? nm : "?");
        }
    }
    Log("[reflect] 自测 OK: assemblies=%d classes=%ld", (int)asmCount, classes);
    return (int)asmCount;
}

void ReflectDumpClass(const char *fullName, void *instance, int maxFields)
{
    auto &api = ir::Api();
    if (!fullName || !fullName[0]) return;
    if (!api.il2cpp_class_from_name)
    {
        Log("[reflect] 缺少 il2cpp_class_from_name，无法 dump");
        return;
    }

    // 支持 "Ns.Name" 与 "Name" 两种写法：按最后一个 '.' 切分
    char ns[256] = { 0 };
    char name[256] = { 0 };
    const char *dot = strrchr(fullName, '.');
    if (dot)
    {
        size_t nsLen = (size_t)(dot - fullName);
        if (nsLen >= sizeof(ns)) nsLen = sizeof(ns) - 1;
        memcpy(ns, fullName, nsLen);
        strncpy(name, dot + 1, sizeof(name) - 1);
    }
    else
    {
        strncpy(name, fullName, sizeof(name) - 1);
    }

    // 遍历所有镜像找这个类 —— 不同游戏主程序集名不同，写死 Assembly-CSharp 会漏
    Il2CppClass *klass = nullptr;
    size_t asmCount = 0;
    Il2CppAssembly **asms = api.il2cpp_domain_get_assemblies
                                ? api.il2cpp_domain_get_assemblies(ir::Domain(), &asmCount)
                                : nullptr;
    for (size_t i = 0; asms && i < asmCount && !klass; ++i)
    {
        Il2CppImage *img =
            api.il2cpp_assembly_get_image ? api.il2cpp_assembly_get_image(asms[i]) : nullptr;
        if (!img) continue;
        klass = api.il2cpp_class_from_name(img, ns, name);
        if (!klass && ns[0])
            klass = api.il2cpp_class_from_name(img, "", name);   // 退回空命名空间
    }

    if (!klass)
    {
        Log("[reflect] dump 失败：找不到类 \"%s\"（试过 ns=\"%s\" 与空命名空间，扫了 %d 个镜像）",
            fullName, ns, (int)asmCount);
        return;
    }

    const char *gotNs = api.il2cpp_class_get_namespace ? api.il2cpp_class_get_namespace(klass) : nullptr;
    Log("[reflect] === dump %s.%s ===", gotNs ? gotNs : "", name);
    Log("[reflect] instance size = %d 字节, 实例 = %p",
        api.il2cpp_class_instance_size ? (int)api.il2cpp_class_instance_size(klass) : -1, instance);

    // ---- 继承链 ----
    Log("[reflect] --- 继承链 ---");
    for (Il2CppClass *c = klass; c;
         c = api.il2cpp_class_get_parent ? api.il2cpp_class_get_parent(c) : nullptr)
        Log("[reflect]   -> %s (size=%d)",
            api.il2cpp_class_get_name ? api.il2cpp_class_get_name(c) : "?",
            api.il2cpp_class_instance_size ? (int)api.il2cpp_class_instance_size(c) : -1);

    // ---- 字段（遍历，不按白名单 —— 这样才有通用价值）----
    Log("[reflect] --- 字段 ---");
    if (!api.il2cpp_class_get_fields)
    {
        Log("[reflect] 缺少 il2cpp_class_get_fields，跳过");
        return;
    }

    int n = 0;
    void *iter = nullptr;
    for (FieldInfo *f = api.il2cpp_class_get_fields(klass, &iter); f;
         f = api.il2cpp_class_get_fields(klass, &iter))
    {
        const char *fname = api.il2cpp_field_get_name ? api.il2cpp_field_get_name(f) : "?";

        // ★ 编译器生成的字段（<>c__ / <Hotfix> 等）跳过 —— 它们不是真正的游戏字段
        if (fname && fname[0] == '_' && fname[1] == '_') continue;

        if (maxFields > 0 && n >= maxFields)
        {
            Log("[reflect]   ... （已达 maxFields=%d，停止）", maxFields);
            break;
        }
        DumpFieldValue(klass, f, (Il2CppObject *)instance, nullptr);
        ++n;
    }
    Log("[reflect] === end dump (%d 个字段) ===", n);
}
