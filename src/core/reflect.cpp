// reflect.cpp — 反射便利层实现
#include "core/reflect.h"
#include "core/log.h"
#include "core/vec.h"
#include "mod_config.h"

#include "il2cpp/il2cpp_runtime.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace ir = il2cpp::runtime;

// ================================================================ Il2CppType 布局
// Il2CppType 在运行时是不透明的，但我们只需要它的 tag 位。
// 这个布局在所有 Unity IL2CPP 版本上稳定（data / attrs / type / byref / pinned）。
struct ArkTypeLayout
{
    void *data;
    unsigned int attrs    : 16;
    unsigned int type     : 8;
    unsigned int num_mods : 6;
    unsigned int byref    : 1;
    unsigned int pinned   : 1;
};

static Il2CppTypeEnum TypeTag(Il2CppType *t)
{
    if (!t) return IL2CPP_TYPE_END;
    return (Il2CppTypeEnum)((const ArkTypeLayout *)t)->type;
}

// ================================================================ 类索引
// ★ 双缓冲：重建时先写 spare，建完整体换给 live。
//   渲染线程每帧都在 ArkReflectClassAt 里拿元素指针，直接清空重建会读到半个表 / realloc 后的野指针。
//   换下来的旧块**不 free**（留给可能还在读它的那一两帧），下次重建才复用。
static Vec<ArkClassEntry> g_index;      // 对外可读的那份
static Vec<ArkClassEntry> g_indexSpare; // 重建草稿 / 旧块寄存
static volatile LONG      g_indexBuilt;
static volatile LONG      g_indexRebuilding;
static volatile LONG      g_indexGen;   // 每次换代 +1，UI 可用来判断"要不要重扫"

static int BuildInto(Vec<ArkClassEntry> &dst)
{
    dst.Clear();

    Il2CppDomain *domain = ir::Domain();
    if (!domain)
    {
        Log("[reflect] 建索引失败：没有 domain");
        return 0;
    }

    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_domain_get_assemblies || !api.il2cpp_image_get_class_count ||
        !api.il2cpp_image_get_class)
    {
        Log("[reflect] 建索引失败：缺关键导出");
        return 0;
    }

    dst.Reserve(16384);

    size_t asmCount = 0;
    Il2CppAssembly **asms = api.il2cpp_domain_get_assemblies(domain, &asmCount);
    int truncated = 0;
    int imgCount = 0;
    int errCount = 0;
    int asmWithClass = 0;

    for (size_t a = 0; a < asmCount; ++a)
    {
        Il2CppImage *img = asms[a] ? api.il2cpp_assembly_get_image(asms[a]) : nullptr;
        if (!img) continue;
        ++imgCount;

        const size_t n = api.il2cpp_image_get_class_count(img);
        if (n) ++asmWithClass;
        for (size_t i = 0; i < n; ++i)
        {
            if (dst.count >= MOD_CLASS_INDEX_MAX) { truncated = 1; break; }

            Il2CppClass *k = api.il2cpp_image_get_class(img, i);
            if (!k) continue;

            // ★ 取名字要走 API，别按裸偏移读 Il2CppClass —— 那是"必须能走官方 API 就走"的铁律。
            const char *name = api.il2cpp_class_get_name(k);
            if (!name || !name[0]) { ++errCount; continue; }
            const char *ns = api.il2cpp_class_get_namespace(k);

            ArkClassEntry *e = dst.Push();
            if (!e) break;
            e->ns = ns ? ns : "";
            e->name = name;
            e->klass = k;
            e->image = img;
        }
        if (truncated) break;
    }

    Log("[reflect] 类索引建立完成：%d 个类 / %d 个镜像（其中 %d 个有类）/ 共 %u 个程序集%s%s",
        dst.count, imgCount, asmWithClass, (unsigned)asmCount,
        truncated ? "（已达上限被截断）" : "", errCount ? "（有跳过项）" : "");
    return dst.count;
}

// 启动时调一次（幂等）。
int ArkReflectBuildIndex()
{
    if (InterlockedCompareExchange(&g_indexBuilt, 1, 0) != 0)
        return g_index.count;

    const int n = BuildInto(g_index);
    if (n <= 0)
    {
        InterlockedExchange(&g_indexBuilt, 0);   // 失败允许下次重试
        return g_index.count;
    }
    InterlockedIncrement(&g_indexGen);
    return n;
}

// 重建（供「重建索引」按钮用）。⚠ 只能在没有并发读者持有元素指针的线程上调用，
// 框架里是投递到 Worker 执行。
int ArkReflectRebuildIndex()
{
    if (InterlockedCompareExchange(&g_indexRebuilding, 1, 0) != 0)
        return g_index.count;

    const int n = BuildInto(g_indexSpare);
    if (n > 0)
    {
        Vec<ArkClassEntry> old = g_index;   // 纯 POD，直接结构体交换
        g_index      = g_indexSpare;
        g_indexSpare = old;                 // 旧块留着，不 free
        InterlockedIncrement(&g_indexGen);
    }
    InterlockedExchange(&g_indexBuilt, 1);
    InterlockedExchange(&g_indexRebuilding, 0);
    return g_index.count;
}

int ArkReflectIndexGen() { return (int)g_indexGen; }

int ArkReflectClassCount() { return g_index.count; }
const ArkClassEntry *ArkReflectClassAt(int i) { return g_index.At(i); }

// 不区分大小写的子串匹配
static bool ContainsCI(const char *hay, const char *needle)
{
    if (!hay || !needle) return false;
    if (!needle[0]) return true;
    for (const char *p = hay; *p; ++p)
    {
        const char *a = p, *b = needle;
        while (*a && *b)
        {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

int ArkReflectSearch(const char *needle, int *outIdx, int maxOut)
{
    if (!outIdx || maxOut <= 0) return 0;
    int n = 0;
    for (int i = 0; i < g_index.count && n < maxOut; ++i)
    {
        const ArkClassEntry *e = &g_index.data[i];
        if (!needle || !needle[0] || ContainsCI(e->name, needle) || ContainsCI(e->ns, needle))
            outIdx[n++] = i;
    }
    return n;
}

const ArkClassEntry *ArkReflectFindClass(const char *fullOrShort)
{
    if (!fullOrShort || !fullOrShort[0]) return nullptr;
    const char *dot = strrchr(fullOrShort, '.');
    const char *shortName = dot ? dot + 1 : fullOrShort;

    for (int i = 0; i < g_index.count; ++i)
    {
        const ArkClassEntry *e = &g_index.data[i];
        if (strcmp(e->name, shortName) != 0) continue;
        if (!dot) return e;                       // 只给了短名，第一个匹配就算
        if (strcmp(e->ns, "") != 0)
        {
            // 拼全名比较（ns + "." + name）
            char full[512];
            _snprintf_s(full, sizeof(full), _TRUNCATE, "%s.%s", e->ns, e->name);
            if (strcmp(full, fullOrShort) == 0) return e;
        }
    }
    return nullptr;
}

// ================================================================ 字段
int ArkReflectEnumFields(Il2CppClass *klass, ArkFieldVisitor cb, void *ud)
{
    if (!klass || !cb) return 0;
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_class_get_fields || !api.il2cpp_class_get_parent) return 0;

    int total = 0;
    int depth = 0;
    // ★ 父类链可能很深（Assembly-CSharp 里 5~8 层很常见），加个上限防环形继承。
    while (klass && depth < 16)
    {
        void *iter = nullptr;
        FieldInfo *f = nullptr;
        while ((f = api.il2cpp_class_get_fields(klass, &iter)) != nullptr)
        {
            const char *n = api.il2cpp_field_get_name ? api.il2cpp_field_get_name(f) : nullptr;
            if (!n || !n[0]) continue;
            cb(f, klass, depth, ud);
            ++total;
        }
        klass = api.il2cpp_class_get_parent(klass);
        ++depth;
    }
    return total;
}

FieldInfo *ArkReflectFindField(Il2CppClass *klass, const char *name, Il2CppClass **ownerOut)
{
    if (!klass || !name) return nullptr;
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_class_get_fields || !api.il2cpp_class_get_parent ||
        !api.il2cpp_field_get_name)
        return nullptr;

    int depth = 0;
    while (klass && depth < 16)
    {
        void *iter = nullptr;
        FieldInfo *f = nullptr;
        while ((f = api.il2cpp_class_get_fields(klass, &iter)) != nullptr)
        {
            const char *n = api.il2cpp_field_get_name(f);
            // ★ 必须 strcmp 精确匹配：子串匹配会让 "_attack" 命中 "_attackTrigger"。
            if (n && strcmp(n, name) == 0)
            {
                if (ownerOut) *ownerOut = klass;
                return f;
            }
        }
        klass = api.il2cpp_class_get_parent(klass);
        ++depth;
    }
    return nullptr;
}

bool ArkReflectIsStaticField(FieldInfo *f)
{
    if (!f) return false;
    Il2CppApi &api = ir::Api();
    // 静态字段的 offset 最高位置 1（0x80000000）。
    if (!api.il2cpp_field_get_offset) return false;
    return (api.il2cpp_field_get_offset(f) & 0x80000000u) != 0;
}

// ================================================================ 类型名
static void Append(char *out, int cap, int &pos, const char *s)
{
    if (!s) return;
    while (*s && pos < cap - 1) out[pos++] = *s++;
    out[pos] = 0;
}

// 泛型实例的可读名字：System.Collections.Generic.List`1<Torappu.EnemyData>
// ★ 为什么要缓存：`il2cpp_type_get_name` 返回的缓冲区**所有权在各版本上不一致**
//   （有的是运行时自己的缓存，有的是一次性分配）⇒ 既不敢释放，也不该每次重建都再要一份。
//   这里拷进自己的定长池（POD，零初始化即合法），按类指针去重，一辈子只问一次。
//   不用堆分配：手动映射载荷里少一处分配就少一处泄漏面。
#define ARK_GENERIC_NAME_CACHE 160
#define ARK_GENERIC_NAME_LEN   192
static const char *GenericInstanceName(Il2CppType *t, Il2CppClass *k)
{
    if (!k) return nullptr;

    struct Entry { Il2CppClass *k; char s[ARK_GENERIC_NAME_LEN]; };
    static Entry cache[ARK_GENERIC_NAME_CACHE];
    static int   used = 0;

    for (int i = 0; i < used; ++i)
        if (cache[i].k == k && cache[i].s[0]) return cache[i].s;

    if (used >= ARK_GENERIC_NAME_CACHE) return nullptr;   // 满了就不再问，退回类名

    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_type_get_name) return nullptr;
    const char *nm = api.il2cpp_type_get_name(t);
    if (!nm || !nm[0]) return nullptr;

    Entry &e = cache[used];
    e.k = k;
    _snprintf_s(e.s, sizeof(e.s), _TRUNCATE, "%s", nm);
    ++used;                                               // 先填内容再发布下标
    return e.s;
}

// 类/对象给人看的名字：泛型实例带实参，其余退回 "命名空间.类名"。
void ArkReflectClassDisplayName(char *out, int cap, Il2CppClass *k)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    Il2CppApi &api = ir::Api();

    if (k && api.il2cpp_class_get_type)
    {
        Il2CppType *kt = api.il2cpp_class_get_type(k);
        if (kt && TypeTag(kt) == IL2CPP_TYPE_GENERICINST)
        {
            const char *pretty = GenericInstanceName(kt, k);
            if (pretty && pretty[0])
            {
                _snprintf_s(out, cap, _TRUNCATE, "%s", pretty);
                return;
            }
        }
    }

    if (!k) { _snprintf_s(out, cap, _TRUNCATE, "(null)"); return; }
    const char *ns = api.il2cpp_class_get_namespace ? api.il2cpp_class_get_namespace(k) : "";
    const char *nm = api.il2cpp_class_get_name ? api.il2cpp_class_get_name(k) : "?";
    if (ns && ns[0]) _snprintf_s(out, cap, _TRUNCATE, "%s.%s", ns, nm);
    else             _snprintf_s(out, cap, _TRUNCATE, "%s", nm);
}

bool ArkReflectIsGenericInstance(Il2CppClass *k)
{
    Il2CppApi &api = ir::Api();
    if (!k || !api.il2cpp_class_get_type) return false;
    Il2CppType *t = api.il2cpp_class_get_type(k);
    return t && TypeTag(t) == IL2CPP_TYPE_GENERICINST;
}

void ArkReflectTypeName(char *out, int cap, Il2CppType *t)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    int pos = 0;
    if (!t) { Append(out, cap, pos, "?"); return; }

    Il2CppApi &api = ir::Api();
    const Il2CppTypeEnum tag = TypeTag(t);

    switch (tag)
    {
    case IL2CPP_TYPE_VOID:    Append(out, cap, pos, "void"); return;
    case IL2CPP_TYPE_BOOLEAN: Append(out, cap, pos, "bool"); return;
    case IL2CPP_TYPE_CHAR:    Append(out, cap, pos, "char"); return;
    case IL2CPP_TYPE_I1:      Append(out, cap, pos, "sbyte"); return;
    case IL2CPP_TYPE_U1:      Append(out, cap, pos, "byte"); return;
    case IL2CPP_TYPE_I2:      Append(out, cap, pos, "short"); return;
    case IL2CPP_TYPE_U2:      Append(out, cap, pos, "ushort"); return;
    case IL2CPP_TYPE_I4:      Append(out, cap, pos, "int"); return;
    case IL2CPP_TYPE_U4:      Append(out, cap, pos, "uint"); return;
    case IL2CPP_TYPE_I8:      Append(out, cap, pos, "long"); return;
    case IL2CPP_TYPE_U8:      Append(out, cap, pos, "ulong"); return;
    case IL2CPP_TYPE_R4:      Append(out, cap, pos, "float"); return;
    case IL2CPP_TYPE_R8:      Append(out, cap, pos, "double"); return;
    case IL2CPP_TYPE_STRING:  Append(out, cap, pos, "string"); return;
    case IL2CPP_TYPE_OBJECT:  Append(out, cap, pos, "object"); return;
    case IL2CPP_TYPE_I:       Append(out, cap, pos, "IntPtr"); return;
    case IL2CPP_TYPE_U:       Append(out, cap, pos, "UIntPtr"); return;
    default: break;
    }

    // 泛型实例：光有类名只是 "List`1"，不带实参等于没说清是什么容器。
    if (tag == IL2CPP_TYPE_GENERICINST)
    {
        Il2CppClass *gk = api.il2cpp_class_from_type ? api.il2cpp_class_from_type(t) : nullptr;
        const char *pretty = GenericInstanceName(t, gk);
        if (pretty && pretty[0])
        {
            Append(out, cap, pos, pretty);
            if (((const ArkTypeLayout *)t)->byref) Append(out, cap, pos, "&");
            return;
        }
    }

    // 复合类型：取它的 class 再拼名字
    Il2CppClass *k = api.il2cpp_class_from_type ? api.il2cpp_class_from_type(t) : nullptr;
    if (!k && api.il2cpp_class_from_il2cpp_type)
        k = api.il2cpp_class_from_il2cpp_type(t);

    if (k)
    {
        const char *ns = api.il2cpp_class_get_namespace ? api.il2cpp_class_get_namespace(k) : "";
        const char *nm = api.il2cpp_class_get_name ? api.il2cpp_class_get_name(k) : "?";
        if (ns && ns[0]) { Append(out, cap, pos, ns); Append(out, cap, pos, "."); }
        Append(out, cap, pos, nm);
    }
    else
    {
        char tmp[32];
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "<type:%d>", (int)tag);
        Append(out, cap, pos, tmp);
    }

    if (tag == IL2CPP_TYPE_SZARRAY) Append(out, cap, pos, "[]");
    if (((const ArkTypeLayout *)t)->byref) Append(out, cap, pos, "&");
}

ArkValKind ArkReflectKindOf(Il2CppType *t)
{
    switch (TypeTag(t))
    {
    case IL2CPP_TYPE_BOOLEAN: return VK_BOOL;
    case IL2CPP_TYPE_CHAR:
    case IL2CPP_TYPE_I1: case IL2CPP_TYPE_U1:
    case IL2CPP_TYPE_I2: case IL2CPP_TYPE_U2:
    case IL2CPP_TYPE_I4: case IL2CPP_TYPE_U4: return VK_INT;
    case IL2CPP_TYPE_I8: case IL2CPP_TYPE_U8: return VK_LONG;
    case IL2CPP_TYPE_R4: return VK_FLOAT;
    case IL2CPP_TYPE_R8: return VK_DOUBLE;
    case IL2CPP_TYPE_STRING: return VK_STRING;
    case IL2CPP_TYPE_CLASS: case IL2CPP_TYPE_OBJECT:
    case IL2CPP_TYPE_SZARRAY: case IL2CPP_TYPE_ARRAY: return VK_OBJECT;
    case IL2CPP_TYPE_VALUETYPE: case IL2CPP_TYPE_ENUM: return VK_STRUCT;
    // ★ 泛型实例（List<T> / Dictionary<K,V> / Nullable<T>…）是独立的一个类型标签，
    //   不在这张 switch 里就等于**什么都没判出来**：字段既读不出值（打 <?>），
    //   也拿不到 child ⇒ 面板上没有展开箭头，整个容器在树里是死的。
    //   实参分不清值类型还是引用类型，所以打回 class 上问一句。
    case IL2CPP_TYPE_GENERICINST:
    {
        Il2CppApi &api = ir::Api();
        Il2CppClass *k = api.il2cpp_class_from_type ? api.il2cpp_class_from_type(t) : nullptr;
        if (k && api.il2cpp_class_is_valuetype && api.il2cpp_class_is_valuetype(k))
            return VK_STRUCT;
        return VK_OBJECT;   // 拿不到 class 时按引用类型兜底：最坏是多显示一个指针，不会崩
    }
    default: return VK_UNKNOWN;
    }
}

int ArkReflectSizeOf(Il2CppType *t)
{
    Il2CppApi &api = ir::Api();
    switch (TypeTag(t))
    {
    case IL2CPP_TYPE_BOOLEAN: case IL2CPP_TYPE_I1: case IL2CPP_TYPE_U1: return 1;
    case IL2CPP_TYPE_CHAR: case IL2CPP_TYPE_I2: case IL2CPP_TYPE_U2: return 2;
    case IL2CPP_TYPE_I4: case IL2CPP_TYPE_U4: case IL2CPP_TYPE_R4: return 4;
    case IL2CPP_TYPE_I8: case IL2CPP_TYPE_U8: case IL2CPP_TYPE_R8:
    case IL2CPP_TYPE_STRING: case IL2CPP_TYPE_CLASS: case IL2CPP_TYPE_OBJECT:
    case IL2CPP_TYPE_SZARRAY: case IL2CPP_TYPE_ARRAY:
    case IL2CPP_TYPE_I: case IL2CPP_TYPE_U: return 8;
    default: break;
    }

    Il2CppClass *k = api.il2cpp_class_from_type ? api.il2cpp_class_from_type(t) : nullptr;
    if (!k) return 0;
    if (TypeTag(t) == IL2CPP_TYPE_CLASS) return 8;

    if (api.il2cpp_class_value_size)
    {
        uint32_t align = 0;
        int32_t sz = api.il2cpp_class_value_size(k, &align);
        if (sz > 0) return sz;
    }
    return 0;
}

// ================================================================ 值读写
// 取字段的原始存储地址。
// 实例字段：instance + offset（offset 不含 16 字节对象头，il2cpp 已扣好）。
// 静态字段：class_get_static_field_data + (offset & 0x7FFFFFFF)。
void *ArkReflectFieldAddr(FieldInfo *f, Il2CppObject *instance)
{
    if (!f) return nullptr;
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_field_get_offset) return nullptr;

    const size_t raw = api.il2cpp_field_get_offset(f);
    if (raw & 0x80000000u)
    {
        if (!instance) return nullptr;
        Il2CppClass *k = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(instance) : nullptr;
        if (!k || !api.il2cpp_class_get_static_field_data) return nullptr;
        void *base = api.il2cpp_class_get_static_field_data(k);
        return base ? (char *)base + (raw & 0x7FFFFFFFu) : nullptr;
    }
    if (!instance) return nullptr;
    return (char *)instance + raw;
}

// 把 Il2CppString 转成 UTF-8 写进 out。
static bool StringToUtf8(Il2CppObject *s, char *out, int cap)
{
    if (!out || cap <= 0) return false;
    out[0] = 0;
    if (!s) { strcpy_s(out, cap, "(null)"); return true; }

    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_string_length || !api.il2cpp_string_chars) return false;

    Il2CppString *str = (Il2CppString *)s;
    const int32_t len = api.il2cpp_string_length(str);
    if (len <= 0) { out[0] = 0; return true; }
    Il2CppChar *chars = api.il2cpp_string_chars(str);
    if (!chars) return false;

    // 只显示前 200 字符，够看内容了，也不至于把 UI 撑爆
    const int32_t show = len > 200 ? 200 : len;
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)chars, show, out, cap - 1, NULL, NULL);
    if (n < 0) n = 0;
    out[n] = 0;
    return true;
}

int ArkReflectReadFieldText(FieldInfo *f, Il2CppObject *instance, char *out, int cap)
{
    if (!f || !out || cap <= 0) return -1;
    out[0] = 0;

    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_field_get_type) return -1;

    Il2CppType *t = api.il2cpp_field_get_type(f);
    const int kind = ArkReflectKindOf(t);
    const bool isStatic = ArkReflectIsStaticField(f);

    // 引用类型走 get_value_object，省得自己处理 GC 句柄
    if (kind == VK_STRING || kind == VK_OBJECT)
    {
        if (!api.il2cpp_field_get_value_object) return -3;
        Il2CppObject *obj = api.il2cpp_field_get_value_object(f, isStatic ? nullptr : instance);
        if (kind == VK_STRING)
        {
            StringToUtf8(obj, out, cap);
        }
        else
        {
            _snprintf_s(out, cap, _TRUNCATE, "%p", (void *)obj);
        }
        return 0;
    }

    void *addr = ArkReflectFieldAddr(f, instance);
    if (!addr) return -3;

    switch (kind)
    {
    case VK_BOOL:
    {
        const unsigned char v = *(unsigned char *)addr;
        _snprintf_s(out, cap, _TRUNCATE, "%s", v ? "true" : "false");
        return 0;
    }
    case VK_INT:
    {
        // 按字段自身宽度取，别一律按 int32 读 —— 1 字节字段后面会读到邻居的字节。
        const int sz = ArkReflectSizeOf(t);
        long long v = 0;
        if (sz == 1) v = *(signed char *)addr;
        else if (sz == 2) v = *(short *)addr;
        else if (sz == 4) v = *(int *)addr;
        else v = *(long long *)addr;
        _snprintf_s(out, cap, _TRUNCATE, "%lld", v);
        return 0;
    }
    case VK_LONG:
    {
        _snprintf_s(out, cap, _TRUNCATE, "%lld", *(long long *)addr);
        return 0;
    }
    case VK_FLOAT:
    {
        _snprintf_s(out, cap, _TRUNCATE, "%.4f", (double)*(float *)addr);
        return 0;
    }
    case VK_DOUBLE:
    {
        _snprintf_s(out, cap, _TRUNCATE, "%.6f", *(double *)addr);
        return 0;
    }
    case VK_STRUCT:
    {
        // 值类型（含 enum、以及游戏自己的 ObscuredFP 这种加密定点）：
        // 不知道内部语义，就把原始字节打出来。至少能看出"它在变"。
        const int sz = ArkReflectSizeOf(t);
        char hex[160] = { 0 };
        int pos = 0;
        const int show = (sz > 16) ? 16 : sz;
        for (int i = 0; i < show && pos < (int)sizeof(hex) - 4; ++i)
            pos += _snprintf_s(hex + pos, sizeof(hex) - pos, _TRUNCATE, "%02X",
                               (unsigned char)((char *)addr)[i]);
        _snprintf_s(out, cap, _TRUNCATE, "(%d B) %s%s", sz, hex, (sz > 16) ? "…" : "");
        return 0;
    }
    default:
        return -2;
    }
}

bool ArkReflectReadFieldNum(FieldInfo *f, Il2CppObject *instance, double *out)
{
    if (!f || !out) return false;
    char buf[64];
    if (ArkReflectReadFieldText(f, instance, buf, sizeof(buf)) != 0) return false;

    Il2CppApi &api = ir::Api();
    Il2CppType *t = api.il2cpp_field_get_type ? api.il2cpp_field_get_type(f) : nullptr;
    const ArkValKind k = ArkReflectKindOf(t);

    if (k == VK_BOOL) { *out = (buf[0] == 't') ? 1.0 : 0.0; return true; }
    *out = atof(buf);
    return true;
}

bool ArkReflectWriteFieldNum(FieldInfo *f, Il2CppObject *instance, double v)
{
    if (!f) return false;
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_field_get_type) return false;

    Il2CppType *t = api.il2cpp_field_get_type(f);
    const int kind = ArkReflectKindOf(t);

    void *addr = ArkReflectFieldAddr(f, instance);
    if (!addr) return false;

    // ★ 只按字段自身宽度写。按 int32 写一个 1 字节字段会把后面 3 个字节踩坏
    //   （这在 IL2CPP 里不会报错，只会让某个不相干的字段变成垃圾）。
    switch (kind)
    {
    case VK_BOOL:
        *(unsigned char *)addr = (unsigned char)(v != 0.0);
        return true;
    case VK_INT:
    {
        const int sz = ArkReflectSizeOf(t);
        if (sz == 1) *(signed char *)addr = (signed char)(long long)v;
        else if (sz == 2) *(short *)addr = (short)(long long)v;
        else if (sz == 4) *(int *)addr = (int)(long long)v;
        else *(long long *)addr = (long long)v;
        return true;
    }
    case VK_LONG:
        *(long long *)addr = (long long)v;
        return true;
    case VK_FLOAT:
        *(float *)addr = (float)v;
        return true;
    case VK_DOUBLE:
        *(double *)addr = v;
        return true;
    default:
        // 值类型/引用类型不在这里直接写 —— 需要知道内部布局，走专门模块。
        return false;
    }
}

bool ArkReflectWriteFieldText(FieldInfo *f, Il2CppObject *instance, const char *text)
{
    if (!f || !text) return false;
    Il2CppApi &api = ir::Api();
    Il2CppType *t = api.il2cpp_field_get_type ? api.il2cpp_field_get_type(f) : nullptr;

    if (ArkReflectKindOf(t) == VK_BOOL)
    {
        const bool v = (strcmp(text, "1") == 0 || strcmp(text, "true") == 0 ||
                        strcmp(text, "on") == 0 || strcmp(text, "是") == 0);
        return ArkReflectWriteFieldNum(f, instance, v ? 1.0 : 0.0);
    }
    return ArkReflectWriteFieldNum(f, instance, atof(text));
}
