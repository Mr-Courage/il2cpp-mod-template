// dumper.cpp — 运行时 dump.cs / script.json 生成（Worker 线程执行）
// ============================================================================
// 关键实现决策：
//
// 1. MethodInfo+0 = methodPointer —— 这是唯一一次读裸偏移，破例原因：
//    il2cpp 没有导出「拿方法指针」的 API，而 Il2CppMethodInfo 的第一个字段
//    是 methodPointer 在**所有已知版本**（v24/v27/v29）都不变 —— runtime_invoke
//    自己就是从这里取代码地址的，Il2CppDumper 的运行时 dump 也全靠它。
//    其余任何运行时结构照旧走官方 API（铁律 9）。
//
// 2. VA 用 PE 首选基址（0x180000000 那个），不是运行时实际加载地址 ——
//    IDA 静态分析 GameAssembly.dll 时按首选基址重定位，script.json 的
//    Address 必须落在 IDA 的地址空间里才有用。
//
// 3. 字段偏移显示 il2cpp_field_get_offset 原值 —— 与 Il2CppDumper 的
//    dump.cs 一致（引用类型字段 = 带 0x10 头的对象内偏移）。
//    静态字段不显示偏移（那个值对静态字段无意义）。
//
// 4. 遍历顺序 = 类索引顺序（ArkReflectClassAt），**只列本类声明的成员**，
//    不沿父类链 —— dump.cs 的语义就是"声明"，继承成员看父类的块。
// ============================================================================
#include "dumper.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "core/gcobj.h"    // Il2CppApi 表
#include "core/log.h"
#include "mod_config.h"      // MOD_GAME_MODULE
#include "core/reflect.h"
#include "core/taskbus.h"  // ArkTaskPost
#include "il2cpp/il2cpp_api.h"
#include "il2cpp/il2cpp_runtime.h"   // il2cpp::runtime::Api()

namespace ir = il2cpp::runtime;

namespace
{

constexpr unsigned FA_STATIC   = 0x0010;
constexpr unsigned FA_LITERAL  = 0x0040;  // const
constexpr unsigned FA_INITONLY = 0x0080;  // readonly
constexpr unsigned FA_PUBLIC   = 0x0006;  // access mask 里 6 = public

constexpr unsigned MA_STATIC   = 0x0008;
constexpr unsigned MA_PUBLIC   = 0x0006;
constexpr unsigned MA_PROTECTED= 0x0004;  // 4/5 都算 family
constexpr unsigned MA_ABSTRACT = 0x0400;

struct DumperState
{
    volatile LONG       state;      // 0 idle 1 running 2 done 3 error
    const char         *phase;      // 静态字符串
    volatile LONG       permille;
    volatile LONG       classes;
    volatile LONG       methods;
    volatile LONG       fields;
    char                outDir[260];
};

DumperState g_dmp = {};             // state=0 / phase=nullptr 都算"空闲"

const char *PhaseOf(int st)
{
    switch (st)
    {
    case 1: return "正在导出";
    case 2: return "完成";
    case 3: return "失败";
    default: return "空闲";
    }
}

// ---- flags → 修饰符前缀 --------------------------------------------------
void MethodMods(char *out, int cap, uint32_t flags)
{
    out[0] = 0;
    const uint32_t acc = flags & 7u;
    if (acc == MA_PUBLIC)       strcat_s(out, cap, "public ");
    else if (acc >= MA_PROTECTED) strcat_s(out, cap, "protected ");
    else                          strcat_s(out, cap, "private ");
    if (flags & MA_STATIC)   strcat_s(out, cap, "static ");
    if (flags & MA_ABSTRACT) strcat_s(out, cap, "abstract ");
}

void FieldMods(char *out, int cap, uint32_t flags)
{
    out[0] = 0;
    const uint32_t acc = flags & 7u;
    if (acc == FA_PUBLIC)       strcat_s(out, cap, "public ");
    else if (acc >= MA_PROTECTED) strcat_s(out, cap, "protected ");
    else                          strcat_s(out, cap, "private ");
    if (flags & FA_LITERAL)  strcat_s(out, cap, "const ");
    else if (flags & FA_STATIC) strcat_s(out, cap, "static ");
    else if (flags & FA_INITONLY) strcat_s(out, cap, "readonly ");
}

// 写一个方法块（RVA 注释 + 签名）。返回写出的方法数（0 或 1）。
// base = 模块运行时基址；imageBase = PE 首选基址。
int WriteMethod(FILE *fcs, FILE *fjs, MethodInfo *m, uintptr_t base, uintptr_t imageBase)
{
    Il2CppApi &api = ir::Api();
    if (!m || !api.il2cpp_method_get_name)
        return 0;

    const char *mname = api.il2cpp_method_get_name(m);
    if (!mname)
        return 0;

    // ---- RVA / VA ----
    // ★ MethodInfo+0 = methodPointer（全版本不变量，见文件头说明）。
    uintptr_t ptr = *(uintptr_t *)m;
    uintptr_t rva = ptr ? (ptr - base) : 0;
    uintptr_t va  = ptr ? (imageBase + rva) : 0;

    if (fjs)
        fprintf(fjs, "{\"Address\":%llu,\"Name\":", (unsigned long long)va);

    if (fcs)
    {
        fprintf(fcs, "\t// RVA: 0x%llX VA: 0x%llX\n",
                (unsigned long long)rva, (unsigned long long)va);
    }

    // ---- 签名 ----
    char mods[64];
    uint32_t flags = 0;
    if (api.il2cpp_method_get_flags)
        flags = api.il2cpp_method_get_flags(m, nullptr);
    MethodMods(mods, sizeof(mods), flags);
    if (!api.il2cpp_method_is_instance(m))
    {   // 某些版本 flags 的 static 位不干净，双保险
        if (!strstr(mods, "static ")) strcat_s(mods, sizeof(mods), "static ");
    }

    char ret[256] = { 0 };

    if (fcs)
    {
        if (api.il2cpp_method_get_return_type && api.il2cpp_type_get_name)
        {
            Il2CppType *rt = api.il2cpp_method_get_return_type(m);
            if (rt) { const char *n = api.il2cpp_type_get_name(rt); if (n) { strncpy_s(ret, sizeof(ret), n, _TRUNCATE); api.il2cpp_free((void *)n); } }
        }
        if (!ret[0]) strncpy_s(ret, sizeof(ret), "void", _TRUNCATE);

        // 泛型方法标注 —— 它的 methodPointer 不是可直接调用的代码
        const bool generic = api.il2cpp_method_is_generic ? api.il2cpp_method_is_generic(m) : false;

        fprintf(fcs, "\t%s%s %s(", mods, ret, mname);

        const uint32_t pc = api.il2cpp_method_get_param_count ? api.il2cpp_method_get_param_count(m) : 0;
        for (uint32_t i = 0; i < pc; ++i)
        {
            char pt[256] = { 0 };
            if (api.il2cpp_method_get_param && api.il2cpp_type_get_name)
            {
                Il2CppType *t = api.il2cpp_method_get_param(m, i);
                if (t) { const char *n = api.il2cpp_type_get_name(t); if (n) { strncpy_s(pt, sizeof(pt), n, _TRUNCATE); api.il2cpp_free((void *)n); } }
            }
            const char *pn = api.il2cpp_method_get_param_name ? api.il2cpp_method_get_param_name(m, i) : nullptr;
            fprintf(fcs, "%s%s %s", i ? ", " : "", pt[0] ? pt : "?", pn ? pn : "arg");
        }
        fprintf(fcs, ")%s { }%s\n", generic ? " /* 泛型：指针不可直调 */" : "",
                (flags & MA_ABSTRACT) ? "" : "");
    }

    if (fjs)
    {
        // "Ns.Class.Method" —— 声明类型从 method 上拿，比遍历时传参稳
        //（嵌套/泛型实例类的 declaring 链一致）。
        const char *cls = "";
        const char *ns  = "";
        char buf[512] = { 0 };
        if (api.il2cpp_method_get_declaring_type)
        {
            Il2CppClass *dk = api.il2cpp_method_get_declaring_type(m);
            if (dk && api.il2cpp_class_get_name)
                cls = api.il2cpp_class_get_name(dk);
            if (dk && api.il2cpp_class_get_namespace)
            {
                const char *n = api.il2cpp_class_get_namespace(dk);
                if (n) ns = n;
            }
        }
        if (ns[0]) snprintf(buf, sizeof(buf), "%s.%s.%s", ns, cls, mname);
        else       snprintf(buf, sizeof(buf), "%s.%s", cls, mname);
        fprintf(fjs, "\"%s\"},\n", buf);
    }

    return 1;
}

// 写一个类的完整块。返回值无意义，统计走 g_dmp。
void WriteClass(FILE *fcs, FILE *fjs, const ArkClassEntry *e, uintptr_t base, uintptr_t imageBase)
{
    Il2CppApi &api = ir::Api();
    Il2CppClass *k = e->klass;
    if (!k)
        return;

    const char *name = api.il2cpp_class_get_name ? api.il2cpp_class_get_name(k) : "?";
    const char *ns   = api.il2cpp_class_get_namespace ? api.il2cpp_class_get_namespace(k) : "";

    if (fcs)
    {
        fprintf(fcs, "\n// Namespace: %s\n", ns[0] ? ns : "-");

        // 嵌套类提示（完整缩进结构太费 —— 一行注释足够定位）
        if (api.il2cpp_class_get_declaring_type)
        {
            Il2CppClass *outer = api.il2cpp_class_get_declaring_type(k);
            if (outer && api.il2cpp_class_get_name)
                fprintf(fcs, "// 嵌套于 %s\n", api.il2cpp_class_get_name(outer));
        }

        // 类声明 + 父类
        uint32_t kflags = api.il2cpp_class_get_flags ? api.il2cpp_class_get_flags(k) : 0;
        const bool isIface = (kflags & 0x20u) != 0;          // TypeAttributes.Interface
        const bool isAbstract = (kflags & 0x100u) != 0;      // TypeAttributes.Abstract
        fprintf(fcs, "%s%s %s", isIface ? "interface " : (isAbstract ? "abstract class " : "class "),
                "", name);

        Il2CppClass *parent = api.il2cpp_class_get_parent ? api.il2cpp_class_get_parent(k) : nullptr;
        if (parent && api.il2cpp_class_get_name)
            fprintf(fcs, " : %s", api.il2cpp_class_get_name(parent));
        fprintf(fcs, "\n{\n");
    }

    // ---- Fields ----
    if (fcs && api.il2cpp_class_get_fields && api.il2cpp_field_get_name)
    {
        bool any = false;
        void *iter = nullptr;
        FieldInfo *f = nullptr;
        while ((f = api.il2cpp_class_get_fields(k, &iter)) != nullptr)
        {
            const char *fn = api.il2cpp_field_get_name(f);
            if (!fn) continue;
            if (!any) { fprintf(fcs, "\t// Fields\n"); any = true; }

            char mods[64];
            const uint32_t fflags = api.il2cpp_field_get_flags ? api.il2cpp_field_get_flags(f) : 0;
            FieldMods(mods, sizeof(mods), fflags);

            char ft[256] = { 0 };
            if (api.il2cpp_field_get_type && api.il2cpp_type_get_name)
            {
                Il2CppType *t = api.il2cpp_field_get_type(f);
                if (t) { const char *n = api.il2cpp_type_get_name(t); if (n) { strncpy_s(ft, sizeof(ft), n, _TRUNCATE); api.il2cpp_free((void *)n); } }
            }

            const bool isStatic = (fflags & FA_STATIC) != 0;
            if (isStatic)
                fprintf(fcs, "\t%s%s %s;\n", mods, ft[0] ? ft : "?", fn);
            else
            {
                const size_t off = api.il2cpp_field_get_offset ? api.il2cpp_field_get_offset(f) : 0;
                fprintf(fcs, "\t%s%s %s; // 0x%llX\n", mods, ft[0] ? ft : "?", fn,
                        (unsigned long long)off);
            }
            InterlockedIncrement(&g_dmp.fields);
        }
    }

    // ---- Methods ----
    if (api.il2cpp_class_get_methods)
    {
        if (fcs) fprintf(fcs, "\n\t// Methods\n");
        void *iter = nullptr;
        MethodInfo *m = nullptr;
        while ((m = api.il2cpp_class_get_methods(k, &iter)) != nullptr)
        {
            if (WriteMethod(fcs, fjs, m, base, imageBase))
                InterlockedIncrement(&g_dmp.methods);
        }
    }

    if (fcs)
        fprintf(fcs, "}\n");

    InterlockedIncrement(&g_dmp.classes);
}

// ---- PE 首选基址 ----
uintptr_t PreferredImageBase(HMODULE mod)
{
    const BYTE *p = (const BYTE *)mod;
    if (!p)
        return 0;
    const IMAGE_DOS_HEADER    *dos = (const IMAGE_DOS_HEADER *)p;
    const IMAGE_NT_HEADERS    *nt  = (const IMAGE_NT_HEADERS *)(p + dos->e_lfanew);
    return (uintptr_t)nt->OptionalHeader.ImageBase;
}

// ---- 任务体（Worker 线程） ------------------------------------------------
void DumpTask(void *ud)
{
    (void)ud;
    g_dmp.phase = "定位游戏模块";
    InterlockedExchange(&g_dmp.state, 1);
    InterlockedExchange(&g_dmp.permille, 0);
    InterlockedExchange(&g_dmp.classes, 0);
    InterlockedExchange(&g_dmp.methods, 0);
    InterlockedExchange(&g_dmp.fields, 0);

    HMODULE mod = GetModuleHandleA(MOD_GAME_MODULE);
    if (!mod)
    {
        Log("[dump] ✗ 找不到 %s", MOD_GAME_MODULE);
        InterlockedExchange(&g_dmp.state, 3);
        return;
    }
    const uintptr_t base      = (uintptr_t)mod;
    const uintptr_t imageBase = PreferredImageBase(mod);
    Log("[dump] %s base=0x%llX 首选基址=0x%llX", MOD_GAME_MODULE,
        (unsigned long long)base, (unsigned long long)imageBase);

    const int total = ArkReflectClassCount();
    if (total <= 0)
    {
        Log("[dump] ✗ 类索引为空 —— 先在工具 tab 里让框架建过索引");
        InterlockedExchange(&g_dmp.state, 3);
        return;
    }

    g_dmp.phase = "打开输出文件";
    char pathCs[520]  = { 0 };
    char pathJs[520]  = { 0 };
    ArkPathA(pathCs, sizeof(pathCs), "dump.cs");
    ArkPathA(pathJs, sizeof(pathJs), "script.json");

    FILE *fcs = nullptr;
    FILE *fjs = nullptr;
    if (fopen_s(&fcs, pathCs, "wb") != 0 || !fcs)
    {
        Log("[dump] ✗ 打不开 %s", pathCs);
        InterlockedExchange(&g_dmp.state, 3);
        return;
    }
    if (fopen_s(&fjs, pathJs, "wb") != 0 || !fjs)
    {
        fclose(fcs);
        Log("[dump] ✗ 打不开 %s", pathJs);
        InterlockedExchange(&g_dmp.state, 3);
        return;
    }

    g_dmp.phase = "写出 dump.cs / script.json";

    // 文件头：与 Il2CppDumper 产物对齐的说明 + 图片信息在类块里带
    fprintf(fcs, "// Runtime dump by arkModify tools —— %d classes\n", total);
    fprintf(fcs, "// 模块 %s 运行时基址 0x%llX，PE 首选基址 0x%llX\n\n",
            MOD_GAME_MODULE, (unsigned long long)base, (unsigned long long)imageBase);
    fprintf(fjs, "{\"ScriptMethod\":[\n");

    const int kProgressStep = 64;
    int done = 0;
    for (int i = 0; i < total; ++i)
    {
        const ArkClassEntry *e = ArkReflectClassAt(i);
        if (e && e->klass)
            WriteClass(fcs, fjs, e, base, imageBase);

        if (++done % kProgressStep == 0)
            InterlockedExchange(&g_dmp.permille, (LONG)(done * 1000 / total));
    }
    InterlockedExchange(&g_dmp.permille, 1000);

    fprintf(fjs, "],\"ScriptString\":[],\"ScriptMetadata\":null,\"ScriptMetadataMethod\":[],\"Addresses\":[]}\n");

    const bool csOk = fflush(fcs) == 0 && fclose(fcs) == 0;
    const bool jsOk = fflush(fjs) == 0 && fclose(fjs) == 0;

    if (!csOk || !jsOk)
    {
        Log("[dump] ✗ 写文件失败（磁盘满/路径失效）cs=%d js=%d", (int)csOk, (int)jsOk);
        InterlockedExchange(&g_dmp.state, 3);
        return;
    }

    g_dmp.phase = "完成";
    InterlockedExchange(&g_dmp.state, 2);
    Log("[dump] ★完成：%d 类 / %d 方法 / %d 字段", (int)g_dmp.classes,
        (int)g_dmp.methods, (int)g_dmp.fields);
    Log("[dump]   dump.cs     -> %s", pathCs);
    Log("[dump]   script.json -> %s", pathJs);
}

} // namespace

// ============================================================================
// 对外接口（UI 线程随便调，只读 volatile）
// ============================================================================
bool ArkDumperStart()
{
    if (InterlockedCompareExchange(&g_dmp.state, 0, 0) == 1)
        return false;                        // 正在跑
    if (ArkReflectClassCount() <= 0)
        return false;                        // 没索引

    if (!ArkTaskPost(DumpTask, nullptr, "dump.run"))
        return false;

    InterlockedExchange(&g_dmp.state, 1);
    return true;
}

int         ArkDumperState()    { return InterlockedCompareExchange(&g_dmp.state, 0, 0); }
const char *ArkDumperPhase()    { return g_dmp.phase ? g_dmp.phase : PhaseOf(InterlockedCompareExchange(&g_dmp.state, 0, 0)); }
int         ArkDumperPermille() { return InterlockedCompareExchange(&g_dmp.permille, 0, 0); }
int         ArkDumperClasses()  { return InterlockedCompareExchange(&g_dmp.classes, 0, 0); }
int         ArkDumperMethods()  { return InterlockedCompareExchange(&g_dmp.methods, 0, 0); }
int         ArkDumperFields()   { return InterlockedCompareExchange(&g_dmp.fields, 0, 0); }

const char *ArkDumperOutDir()   { return ArkModuleDirA(); }
