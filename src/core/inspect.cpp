// inspect.cpp — 实例字段树构建
// ============================================================================
// 三条硬规则（违反任何一条都会让面板卡死或崩）：
//   1. 只在已 attach 的线程跑（Worker / 游戏线程）。渲染线程只读结果数组。
//   2. 值类型字段一律按"字段地址 + 自身宽度"读，绝不按 int32 硬读（会读到邻居字节）。
//   3. 节点数、深度、数组长度、环路 四重限制同时生效 —— 对象图是有环的。
// ============================================================================
#include "core/inspect.h"
#include "mod_config.h"
#include "core/log.h"
#include "core/reflect.h"
#include "core/vec.h"

#include "il2cpp/il2cpp_runtime.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace ir = il2cpp::runtime;

// ★ 三个容量都是"够用 + 不失控"的折中：
//   节点 6000（≈2.9MB，.data 常驻）、路径 40 层（深度上限 20 时的环路保护）、
//   展开集合 512 条。全部定长 —— 理由见下面的 realloc 警告。
#define ARK_INSP_NODE_CAP   6000
#define ARK_INSP_PATH_MAX   40     // 当前路径上最多记多少个对象（环路检测）
#define ARK_INSP_EXPAND_CAP 512    // 能记住的"已展开"节点数

// ★ 节点表是**固定容量、一次分配、永不 realloc** 的。
//   原因：Worker 重建树的时候，渲染线程可能正在遍历同一片内存。
//   如果这里用可增长容器，重建时 realloc 会释放旧块 ⇒ 渲染线程读到已释放内存 ⇒ AV。
//   固定容量下最坏情况只是"这一帧看到半新半旧的值"，下一帧就自愈 —— 不崩。
static ArkInspNode  g_nodeBuf[ARK_INSP_NODE_CAP];
static volatile LONG g_nodeCount = 0;

// 展开状态。key 用 "所属对象 + 字段 + 下标"，跨重建稳定；
// 用节点索引记是不行的 —— 重建后索引全变。
struct InspKey
{
    void *owner;
    void *field;
    int   index;
};
static InspKey       g_expandBuf[ARK_INSP_EXPAND_CAP];
static volatile LONG g_expandCount = 0;

static int   g_maxDepth = 3;
static int   g_maxItems = 16;

// 环路检测：只沿"当前这条路径"记，兄弟节点可以重复访问。
static Il2CppObject *g_path[ARK_INSP_PATH_MAX];
static int           g_pathLen = 0;

// 写入阶段用局部计数，最后一次性发布 —— 渲染线程永远看不到"写了一半的 count"。
static int g_writeCount = 0;

// ================================================================ 基础工具
static const char *SafeName(const char *s)
{
    return (s && s[0]) ? s : "?";
}

static void Utf16ToUtf8(const Il2CppChar *src, int len, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!src || len <= 0) return;

    const int need = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)src, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;

    if (need < cap)
    {
        WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)src, len, out, cap - 1, nullptr, nullptr);
        out[need] = 0;
    }
    else
    {
        // 截断时按整字符截，别在 UTF-8 中间劈开（会在面板上显示成乱码方块）
        int cut = cap - 4;
        int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)src, len, out, cut, nullptr, nullptr);
        out[n > 0 ? n : 0] = 0;
        strncat(out, "…", cap - strlen(out) - 1);
    }
}

static void StrToUtf8(Il2CppObject *strObj, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!strObj) { _snprintf_s(out, cap, _TRUNCATE, "null"); return; }
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_string_length || !api.il2cpp_string_chars) { _snprintf_s(out, cap, _TRUNCATE, "<String>"); return; }
    const int32_t len = api.il2cpp_string_length((Il2CppString *)strObj);
    Utf16ToUtf8(api.il2cpp_string_chars((Il2CppString *)strObj), len, out, cap);
}

static Il2CppClass *ClassOf(Il2CppObject *obj)
{
    if (!obj) return nullptr;
    Il2CppApi &api = ir::Api();
    return api.il2cpp_object_get_class ? api.il2cpp_object_get_class(obj) : obj->klass;
}

// 类名统一走 reflect 的那一份：泛型实例会带上实参（List`1<EnemyData>），
// 否则 UI 上全是 "List`1"，看不出装的是什么。
static void ClassFullName(Il2CppClass *k, char *out, int cap)
{
    ArkReflectClassDisplayName(out, cap, k);
}

static int ClassRank(Il2CppClass *k)
{
    Il2CppApi &api = ir::Api();
    if (api.il2cpp_class_get_rank && k)
        return (int)api.il2cpp_class_get_rank(k);
    return 0;
}

static Il2CppClass *ArrayElementClass(Il2CppClass *k)
{
    Il2CppApi &api = ir::Api();
    if (!k) return nullptr;
    if (api.il2cpp_class_get_element_class)
    {
        Il2CppClass *e = api.il2cpp_class_get_element_class(k);
        if (e) return e;
    }
    if (api.il2cpp_class_get_type && api.il2cpp_type_get_class_or_element_class)
    {
        Il2CppType *t = api.il2cpp_class_get_type(k);
        if (t) return api.il2cpp_type_get_class_or_element_class(t);
    }
    return nullptr;
}

// List<T> / Dictionary<K,V> 的识别：只看名字，够了。
// 实例化之后的泛型类名字仍是 "List`1"（名字来自泛型定义），但个别版本会带上
// 不同 arity 后缀，所以 ` 之前的部分相同就算命中。
static bool NameIs(Il2CppClass *k, const char *simple)
{
    if (!k) return false;
    Il2CppApi &api = ir::Api();
    const char *nm = api.il2cpp_class_get_name ? api.il2cpp_class_get_name(k) : nullptr;
    if (!nm) return false;
    if (strcmp(nm, simple) == 0) return true;

    const char *tick = strchr(simple, '`');
    if (!tick) return false;
    const size_t n = (size_t)(tick - simple);
    return strncmp(nm, simple, n) == 0 && (nm[n] == '`' || nm[n] == 0);
}

// 值类型 → 字符串：优先问托管侧 ToString()。
// 为什么值得绕这一圈：游戏里的数值大量是 ObscuredFP / ObscuredInt 这类**加密定点**，
// 直接读原始字节得到的是一串看不懂的乱码，而它的 ToString() 会解出真实值。
// ★★ 但这条路是把双刃剑，闸门缺一条就是"call 到垃圾地址"级别的崩
//   （2026-11-01 实锤：KeyValuePair<string,List<EnemyLevel>> 元素 → 跳到 0x49020c0）：
static bool MethodPointerSane(const void *mp)
{
    // ★ 兜底闸门：要调的方法指针必须落在 il2cpp 模块镜像内。
    //   泛型/inflated 方法的 MethodInfo 布局跟普通方法不一样，我们按"头一个字段就是
    //   methodPointer"去读可能读到别的字段 —— 那就可能是个野指针。
    //   挡掉不认识的，比"调一下试试"便宜得多：后者直接把游戏打死。
    static uintptr_t s_lo = 0;
    static uintptr_t s_hi = 0;
    if (!s_lo)
    {
        HMODULE m = GetModuleHandleA(MOD_GAME_MODULE);
        if (!m) return true;                       // 拿不到范围就放行，别把功能整个掐死
        const unsigned char *p = (const unsigned char *)m;
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)p;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return true;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(p + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return true;
        s_lo = (uintptr_t)p;
        s_hi = (uintptr_t)p + nt->OptionalHeader.SizeOfImage;
    }
    const uintptr_t v = (uintptr_t)mp;
    return v >= s_lo && v < s_hi;
}

// 只有加密数值和枚举值得绕托管侧；其余值类型直接读字节就够了。
// 不加这个筛选，`List<int>` 会为每个元素做一次 runtime_invoke —— 又慢又险。
static bool WantsToString(Il2CppClass *k)
{
    if (!k) return false;
    Il2CppApi &api = ir::Api();
    const char *nm = api.il2cpp_class_get_name ? api.il2cpp_class_get_name(k) : nullptr;
    if (nm && strstr(nm, "Obscured")) return true;
    return api.il2cpp_class_is_enum && api.il2cpp_class_is_enum(k);
}

static bool BoxAndToString(Il2CppClass *k, void *addr, char *out, int cap)
{
    out[0] = 0;
    if (!k || !addr) return false;
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_value_box || !api.il2cpp_runtime_invoke || !api.il2cpp_class_get_method_from_name)
        return false;

    MethodInfo *ts = api.il2cpp_class_get_method_from_name(k, "ToString", 0);
    if (!ts) return false;

    // ① 泛型实例上的方法拿到的是**未膨胀**的方法，methodPointer 不是真代码。
    //    （il2cpp 里这种要 class_inflate_generic_method 之后才可调，否则就是跳野指针）
    if (ArkReflectIsGenericInstance(k))
        return false;
    // ② 泛型方法本身同理，runtime_invoke 不认泛型定义。
    if (api.il2cpp_method_is_generic && api.il2cpp_method_is_generic(ts))
        return false;
    // ③ MethodInfo 在本项目里是不透明类型，只能按已知的头部读 methodPointer。
    if (!MethodPointerSane(((MethodInfoHeader *)ts)->methodPointer))
    {
        ArkLogThrottled(0x6D707331, "[insp] 跳过不在 il2cpp 镜像内的 methodPointer（%s.ToString）",
                        api.il2cpp_class_get_name ? api.il2cpp_class_get_name(k) : "?");
        return false;
    }

    Il2CppObject *boxed = api.il2cpp_value_box(k, addr);
    if (!boxed) return false;

    Il2CppException *exc = nullptr;
    Il2CppObject *r = api.il2cpp_runtime_invoke(ts, boxed, nullptr, &exc);
    if (exc || !r) return false;

    StrToUtf8(r, out, cap);
    return out[0] != 0;
}

// ================================================================ 展开状态
static bool IsExpanded(void *owner, void *field, int index)
{
    const int n = (int)InterlockedCompareExchange(&g_expandCount, 0, 0);
    for (int i = 0; i < n; ++i)
    {
        const InspKey &k = g_expandBuf[i];
        if (k.owner == owner && k.field == field && k.index == index)
            return true;
    }
    return false;
}

static void SetExpanded(void *owner, void *field, int index, bool on)
{
    const int n = (int)InterlockedCompareExchange(&g_expandCount, 0, 0);
    for (int i = 0; i < n; ++i)
    {
        const InspKey &k = g_expandBuf[i];
        if (k.owner == owner && k.field == field && k.index == index)
        {
            if (!on)
            {
                // 用最后一项顶上：展开集合是**集合**，顺序无意义。
                g_expandBuf[i] = g_expandBuf[n - 1];
                InterlockedDecrement(&g_expandCount);
            }
            return;
        }
    }
    if (!on) return;
    if (n >= ARK_INSP_EXPAND_CAP) return;
    g_expandBuf[n].owner = owner;
    g_expandBuf[n].field = field;
    g_expandBuf[n].index = index;
    InterlockedIncrement(&g_expandCount);   // ★ 先填内容再发布计数，顺序不能反
}

// ================================================================ 节点
static ArkInspNode *PushNode(int depth)
{
    if (g_writeCount >= ARK_INSP_NODE_CAP)
        return nullptr;
    ArkInspNode *n = &g_nodeBuf[g_writeCount++];
    memset(n, 0, sizeof(*n));
    n->depth = depth;
    n->kind = IKN_FIELD;
    n->index = -1;
    return n;
}

static bool PathHas(Il2CppObject *obj)
{
    for (int i = 0; i < g_pathLen; ++i)
        if (g_path[i] == obj) return true;
    return false;
}

static void PathPush(Il2CppObject *obj)
{
    if (g_pathLen < ARK_INSP_PATH_MAX)
        g_path[g_pathLen++] = obj;
}

static void PathPop()
{
    if (g_pathLen > 0) --g_pathLen;
}

// 前向声明
static void WalkObject(Il2CppObject *obj, Il2CppClass *k, int depth, bool statics);
static void WalkArray(Il2CppObject *arr, int depth, const char *label);
static void WalkList(Il2CppObject *list, Il2CppClass *k, int depth, const char *label);

// 读一个字段，填成文本 + 拿子对象指针。返回 false = 类型不支持（节点照样会建，只是值空着）。
static void ReadFieldValue(FieldInfo *f, Il2CppObject *owner, ArkInspNode *n)
{
    Il2CppApi &api = ir::Api();
    Il2CppType *t = api.il2cpp_field_get_type(f);
    n->valKind = ArkReflectKindOf(t);
    n->editable = 0;
    n->child = nullptr;

    const bool isStatic = n->isStatic != 0;

    if (n->valKind == VK_STRING || n->valKind == VK_OBJECT)
    {
        if (!api.il2cpp_field_get_value_object) return;
        Il2CppObject *v = api.il2cpp_field_get_value_object(f, isStatic ? nullptr : owner);
        n->child = v;
        if (!v) { _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "null"); return; }

        if (n->valKind == VK_STRING)
        {
            char tmp[200];
            StrToUtf8(v, tmp, sizeof(tmp));
            _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "\"%s\"", tmp);
            return;
        }

        char cls[120];
        ClassFullName(ClassOf(v), cls, sizeof(cls));
        _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%p  %s", (void *)v, cls);
        return;
    }

    void *addr = ArkReflectFieldAddr(f, owner);
    if (!addr) { _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "<?>"); return; }

    switch (n->valKind)
    {
    case VK_BOOL:
    {
        const unsigned char v = *(unsigned char *)addr;
        _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%s", v ? "true" : "false");
        n->num = v ? 1 : 0;
        n->editable = 1;
        return;
    }
    case VK_INT:
    {
        const int sz = ArkReflectSizeOf(t);
        long long v = 0;
        if (sz == 1)      v = *(signed char *)addr;
        else if (sz == 2) v = *(short *)addr;
        else if (sz == 4) v = *(int *)addr;
        else              v = *(long long *)addr;
        _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%lld", v);
        n->num = (double)v;
        n->editable = 1;
        return;
    }
    case VK_LONG:
    {
        const long long v = *(long long *)addr;
        _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%lld", v);
        // 超过 2^53 的整数 double 存不精确，这时候不给拖条，避免"改一次掉精度"
        n->num = (double)v;
        n->editable = (v > -9007199254740992LL && v < 9007199254740992LL) ? 1 : 0;
        return;
    }
    case VK_FLOAT:
    {
        const float v = *(float *)addr;
        _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%.4f", (double)v);
        n->num = (double)v;
        n->editable = 1;
        return;
    }
    case VK_DOUBLE:
    {
        const double v = *(double *)addr;
        _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%.6f", v);
        n->num = v;
        n->editable = 1;
        return;
    }
    case VK_STRUCT:
    {
        Il2CppClass *sk = api.il2cpp_class_from_type ? api.il2cpp_class_from_type(t) : nullptr;
        char cls[120];
        ClassFullName(sk, cls, sizeof(cls));

        // 加密数值（Obscured*）与枚举：问托管侧要可读形式。
        const bool obscured = strstr(cls, "Obscured") != nullptr;
        const bool isEnum = sk && api.il2cpp_class_is_enum && api.il2cpp_class_is_enum(sk);
        if ((obscured || isEnum) && BoxAndToString(sk, addr, n->value, sizeof(n->value)))
            return;

        const int sz = ArkReflectSizeOf(t);
        char hex[120] = { 0 };
        int pos = 0;
        const int show = (sz > 12) ? 12 : sz;
        for (int i = 0; i < show && pos < (int)sizeof(hex) - 4; ++i)
            pos += _snprintf_s(hex + pos, sizeof(hex) - pos, _TRUNCATE, "%02X",
                               (unsigned char)((char *)addr)[i]);
        _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "(%s %dB) %s%s",
                    cls, sz, hex, (sz > 12) ? "…" : "");
        return;
    }
    default:
        _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "<?>");
        return;
    }
}

// 引用类型的子对象要下钻，按它的真实形态选遍历器。
static void Diverge(ArkInspNode *n, int depth)
{
    Il2CppObject *c = n->child;
    if (!c) return;
    Il2CppClass *k = ClassOf(c);
    if (!k) return;

    if (ClassRank(k) > 0)
        WalkArray(c, depth, n->name);
    else if (NameIs(k, "List`1"))
        WalkList(c, k, depth, n->name);
    else if (NameIs(k, "Dictionary`2") || NameIs(k, "HashSet`1") || NameIs(k, "Queue`1") ||
             NameIs(k, "Stack`1"))
    {
        // 这些容器内部结构复杂（buckets/entries），硬啃性价比低：
        // 显示元素个数就够判断"有没有东西"，具体内容用 ToString 或调用器去看。
        Il2CppApi &api = ir::Api();
        FieldInfo *cf = ArkReflectFindField(k, "_count", nullptr);
        if (!cf) cf = ArkReflectFindField(k, "count", nullptr);
        if (cf)
        {
            int cnt = 0;
            if (api.il2cpp_field_get_value)
                api.il2cpp_field_get_value(c, cf, &cnt);
            ArkInspNode *e = PushNode(depth);
            if (e)
            {
                e->kind = IKN_ITEM;
                e->obj = c;
                _snprintf_s(e->name, sizeof(e->name), _TRUNCATE, "count");
                _snprintf_s(e->type, sizeof(e->type), _TRUNCATE, "System.Int32");
                _snprintf_s(e->value, sizeof(e->value), _TRUNCATE, "%d", cnt);
            }
        }
    }
    else
        WalkObject(c, k, depth, false);
}

// ================================================================ 遍历器
static void WalkFields(Il2CppObject *obj, Il2CppClass *k, int depth)
{
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_class_get_fields) return;

    // 沿父类链上溯：il2cpp_class_get_fields 只返回**本类**字段。
    Il2CppClass *cur = k;
    int guard = 0;
    while (cur && guard++ < 16)
    {
        void *iter = nullptr;
        FieldInfo *f = nullptr;
        while ((f = api.il2cpp_class_get_fields(cur, &iter)) != nullptr)
        {
            if (g_writeCount >= ARK_INSP_NODE_CAP) return;

            ArkInspNode *n = PushNode(depth);
            if (!n) return;

            n->field = f;
            n->owner = cur;
            n->obj = obj;
            n->isStatic = 0;
            n->kind = IKN_FIELD;

            const char *fn = api.il2cpp_field_get_name ? api.il2cpp_field_get_name(f) : "?";
            _snprintf_s(n->name, sizeof(n->name), _TRUNCATE, "%s", SafeName(fn));

            Il2CppType *t = api.il2cpp_field_get_type(f);
            ArkReflectTypeName(n->type, sizeof(n->type), t);

            ReadFieldValue(f, obj, n);

            const bool canDrill = n->child && (depth + 1 < g_maxDepth) && !PathHas(n->child);
            n->expanded = canDrill && IsExpanded(obj, f, -1) ? 1 : 0;
            n->expandable = canDrill ? 1 : 0;

            if (n->expanded)
            {
                PathPush(n->child);
                Diverge(n, depth + 1);
                PathPop();
            }
        }
        cur = api.il2cpp_class_get_parent ? api.il2cpp_class_get_parent(cur) : nullptr;
    }
}

static void WalkStatics(Il2CppClass *k, int depth)
{
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_class_get_fields) return;

    void *iter = nullptr;
    FieldInfo *f = nullptr;
    while ((f = api.il2cpp_class_get_fields(k, &iter)) != nullptr)
    {
        if (!ArkReflectIsStaticField(f)) continue;
        if (g_writeCount >= ARK_INSP_NODE_CAP) return;

        ArkInspNode *n = PushNode(depth);
        if (!n) return;
        n->field = f;
        n->owner = k;
        n->obj = nullptr;
        n->isStatic = 1;
        n->kind = IKN_FIELD;
        const char *fn = api.il2cpp_field_get_name ? api.il2cpp_field_get_name(f) : "?";
        _snprintf_s(n->name, sizeof(n->name), _TRUNCATE, "%s", SafeName(fn));
        ArkReflectTypeName(n->type, sizeof(n->type), api.il2cpp_field_get_type(f));
        ReadFieldValue(f, nullptr, n);
        // 静态字段暂不支持下钻：它们的生命周期跟类走，展开意义不大，
        // 而误展开还可能撞上未初始化的静态区。
        n->expandable = 0;
        n->expanded = 0;
    }
}

static void WalkObject(Il2CppObject *obj, Il2CppClass *k, int depth, bool statics)
{
    if (!obj || !k) return;
    if (depth >= g_maxDepth) return;

    if (statics) WalkStatics(k, depth);
    WalkFields(obj, k, depth);
}

// ================================================================ 值类型元素
// 值类型（结构体）在数组里是**内联字节**，没有独立对象。想把
// KeyValuePair<K,V> / Vector2 这种看明白，只能按字段偏移逐个读。
// ★ il2cpp_field_get_offset 对值类型内部字段**含 16 字节装箱头** ⇒ 统一减 16，
//   减完必须用 valueSize 校验，越界一律不读 —— 宁可少显示一个字段，也不读到缓冲区外面。
//
// ★★ 泛型值类型（KeyValuePair<K,V>）还有第二层坑：il2cpp **不给实参化后的类膨胀字段类型**，
//    字段的 Il2CppType 仍然是 TYPE_VAR（也就是 TKey / TValue）。按它判种类只能得到
//    VK_UNKNOWN，于是一个字段都显示不出来 —— 现象就是"箭头有，点开是空的"。
//    这里改用**类名里已经展开好的实参文本**反推槽宽，字段按声明顺序对应实参顺序
//    （KeyValuePair 的 key 是 VAR0、value 是 VAR1，顺序稳定）。
#define ARK_ARG_MAX 4
#define ARK_ARG_LEN 120

static bool StrStarts(const char *s, const char *pre)
{
    if (!s || !pre) return false;
    const size_t n = strlen(pre);
    return strncmp(s, pre, n) == 0;
}

static bool StrEnds(const char *s, const char *suf)
{
    if (!s || !suf) return false;
    const size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

// 切出 "Ns.KeyValuePair<A,B>" 的顶层实参。按顶层逗号切，注意 '<' '>' 嵌套。
static int SplitGenericArgs(const char *full, char out[][ARK_ARG_LEN], int maxArgs)
{
    if (!full || !out || maxArgs <= 0) return 0;
    const char *lt = strchr(full, '<');
    if (!lt) return 0;

    int n = 0, depth = 0;
    const char *seg = lt + 1;

    for (const char *p = lt; *p; ++p)
    {
        if (*p == '<') { ++depth; continue; }

        if (*p == '>')
        {
            --depth;
            if (depth == 0)
            {
                if (n < maxArgs)
                {
                    int len = (int)(p - seg);
                    while (len > 0 && *seg == ' ') { ++seg; --len; }
                    if (len > ARK_ARG_LEN - 1) len = ARK_ARG_LEN - 1;
                    if (len > 0) { memcpy(out[n], seg, len); out[n][len] = 0; ++n; }
                }
                return n;
            }
            continue;
        }

        if (*p == ',' && depth == 1)
        {
            if (n < maxArgs)
            {
                int len = (int)(p - seg);
                while (len > 0 && *seg == ' ') { ++seg; --len; }
                if (len > ARK_ARG_LEN - 1) len = ARK_ARG_LEN - 1;
                if (len > 0) { memcpy(out[n], seg, len); out[n][len] = 0; ++n; }
            }
            seg = p + 1;
        }
    }
    return n;
}

// 削掉常见命名空间前缀。只在"名字段起点"（开头 / '<' / ',' 之后）削，
// 免得误伤恰好包含这些字样的普通名字。
static void ScrubNamespaces(const char *src, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!src) return;

    static const char *kPre[] = {
        "System.Collections.Generic.",
        "System.Collections.",
        "System.Threading.",
        "System.",
        "Torappu.",
        "UnityEngine.",
    };

    int pos = 0;
    bool atStart = true;
    const char *p = src;
    while (*p && pos < cap - 1)
    {
        if (atStart)
        {
            bool cut = false;
            for (int i = 0; i < (int)(sizeof(kPre) / sizeof(kPre[0])); ++i)
            {
                if (StrStarts(p, kPre[i])) { p += strlen(kPre[i]); cut = true; break; }
            }
            (void)cut;
            atStart = false;
            if (!*p) break;
        }
        out[pos++] = *p;
        if (*p == '<' || *p == ',') atStart = true;
        ++p;
    }
    out[pos] = 0;
}

// 类名的短形式：List<KeyValuePair<String,EnemyLevel>> 这种，一行放得下
static void ShortTypeName(char *out, int cap, Il2CppClass *k)
{
    if (!out || cap <= 0) return;
    char full[224];
    ArkReflectClassDisplayName(full, sizeof(full), k);
    ScrubNamespaces(full, out, cap);
}

// 宽松按名找类：实参名是 "System.Collections.Generic.List<X>"（**没有** `1 后缀），
// 而索引里存的是 name="List`1"，所以只比到反引号之前。
// 只用于"裸类名"的实参 —— 那种情况下必须问准是不是值类型，
// 猜成引用类型就等于把一段整数当指针解引用，直接 AV。
static Il2CppClass *FindClassLoose(const char *full)
{
    if (!full || !full[0]) return nullptr;

    // 缓存：展开几十上百个元素时，同一批实参类型会被问成千上万次，
    // 每次都扫 5 万个类是纯浪费。定长 POD 表，满了就不再写 —— 丢掉缓存只是慢一点。
    struct LooseHit { char key[ARK_ARG_LEN]; Il2CppClass *k; };
    static LooseHit s_hit[32];
    static int      s_used = 0;
    for (int i = 0; i < s_used; ++i)
        if (strcmp(s_hit[i].key, full) == 0) return s_hit[i].k;

    char head[ARK_ARG_LEN];
    _snprintf_s(head, sizeof(head), _TRUNCATE, "%s", full);
    char *lt = strchr(head, '<');
    if (lt) *lt = 0;

    const char *dot = strrchr(head, '.');
    const char *simple = dot ? dot + 1 : head;

    char want[96];
    _snprintf_s(want, sizeof(want), _TRUNCATE, "%s", simple);
    char *tick = strchr(want, '`');
    if (tick) *tick = 0;
    const int wantLen = (int)strlen(want);
    if (wantLen <= 0) return nullptr;

    const int nsLen = dot ? (int)(dot - head) : 0;

    const int total = ArkReflectClassCount();
    for (int i = 0; i < total; ++i)
    {
        const ArkClassEntry *e = ArkReflectClassAt(i);
        if (!e || !e->name || !e->klass) continue;

        if (nsLen)
        {
            if (!e->ns || (int)strlen(e->ns) != nsLen || strncmp(e->ns, head, nsLen) != 0) continue;
        }
        else if (e->ns && e->ns[0]) continue;

        if (strncmp(e->name, want, wantLen) != 0) continue;
        if (e->name[wantLen] != 0 && e->name[wantLen] != '`') continue;

        if (s_used < (int)(sizeof(s_hit) / sizeof(s_hit[0])))
        {
            _snprintf_s(s_hit[s_used].key, ARK_ARG_LEN, _TRUNCATE, "%s", full);
            s_hit[s_used].k = e->klass;
            ++s_used;                     // 先填内容再发布下标
        }
        return e->klass;
    }
    return nullptr;
}

// 由实参**名字文本**推槽宽与种类。
// 为什么不一律查类索引：那是 5 万次 strcmp。这里绝大多数情况靠字符串规则就够
// （KeyValuePair<String, List<X>> 两个槽都是 8 字节引用）。
static ArkValKind GuessSlot(const char *name, int *sz, Il2CppClass **cls)
{
    *sz = 0;
    if (cls) *cls = nullptr;
    if (!name || !name[0]) return VK_UNKNOWN;

    char head[ARK_ARG_LEN];
    _snprintf_s(head, sizeof(head), _TRUNCATE, "%s", name);
    char *lt = strchr(head, '<');
    if (lt) *lt = 0;

    const char *dot = strrchr(head, '.');
    const char *simple = dot ? dot + 1 : head;

    static const struct { const char *n; ArkValKind k; int sz; } kPrim[] = {
        { "Boolean", VK_BOOL, 1 },
        { "Byte", VK_INT, 1 }, { "SByte", VK_INT, 1 },
        { "Char", VK_INT, 2 },
        { "Int16", VK_INT, 2 }, { "UInt16", VK_INT, 2 },
        { "Int32", VK_INT, 4 }, { "UInt32", VK_INT, 4 }, { "Single", VK_FLOAT, 4 },
        { "Int64", VK_LONG, 8 }, { "UInt64", VK_LONG, 8 }, { "Double", VK_DOUBLE, 8 },
        { "IntPtr", VK_LONG, 8 }, { "UIntPtr", VK_LONG, 8 },
    };
    for (int i = 0; i < (int)(sizeof(kPrim) / sizeof(kPrim[0])); ++i)
    {
        if (strcmp(simple, kPrim[i].n) == 0) { *sz = kPrim[i].sz; return kPrim[i].k; }
    }

    if (lt)
    {
        // 泛型实参：容器类都是引用类型。唯一的例外是 Nullable<T>（值类型，宽度不定），
        // 认不出来就干脆不显示，绝不硬按指针解引用。
        if (strcmp(simple, "Nullable") == 0) return VK_UNKNOWN;
        *sz = 8;
        return VK_OBJECT;
    }

    if (StrEnds(simple, "]")) { *sz = 8; return VK_OBJECT; }   // 数组

    Il2CppClass *k = FindClassLoose(head);
    if (k)
    {
        Il2CppApi &api = ir::Api();
        const bool vt = api.il2cpp_class_is_valuetype && api.il2cpp_class_is_valuetype(k);
        if (!vt) { if (cls) *cls = k; *sz = 8; return VK_OBJECT; }

        if (api.il2cpp_class_value_size)
        {
            uint32_t align = 0;
            const int vs = (int)api.il2cpp_class_value_size(k, &align);
            if (vs > 0) { if (cls) *cls = k; *sz = vs; return VK_STRUCT; }
        }
    }
    return VK_UNKNOWN;
}

static bool ClassHasFields(Il2CppClass *k)
{
    Il2CppApi &api = ir::Api();
    if (!k || !api.il2cpp_class_get_fields) return false;
    void *iter = nullptr;
    return api.il2cpp_class_get_fields(k, &iter) != nullptr;
}

// base = 内联结构体的起始地址（也是展开状态的 key：每个元素地址不同，天然分开）
// nest = 结构体嵌套层数上限（Vector3 套 Vector3 这种，防递归）
static void WalkStructFieldsAt(void *base, Il2CppClass *k, int depth, int valueSize, int nest)
{
    Il2CppApi &api = ir::Api();
    if (!base || !k || valueSize <= 0 || nest <= 0) return;
    if (!api.il2cpp_class_get_fields || !api.il2cpp_field_get_offset) return;

    // 泛型值类型的实参表（KeyValuePair<K,V> 的 K / V）
    char args[ARK_ARG_MAX][ARK_ARG_LEN];
    int  argc = 0;
    if (ArkReflectIsGenericInstance(k))
    {
        char dn[224];
        ArkReflectClassDisplayName(dn, sizeof(dn), k);
        argc = SplitGenericArgs(dn, args, ARK_ARG_MAX);
    }

    int argCursor = 0;   // 实参游标：只有 TYPE_VAR 字段才吃一个
    int seen = 0;
    int emitted = 0;
    int skipped = 0;

    Il2CppClass *cur = k;
    int guard = 0;
    while (cur && guard++ < 8)
    {
        void *iter = nullptr;
        FieldInfo *f = nullptr;
        while ((f = api.il2cpp_class_get_fields(cur, &iter)) != nullptr)
        {
            if (g_writeCount >= ARK_INSP_NODE_CAP) return;
            if (ArkReflectIsStaticField(f)) continue;
            ++seen;

            Il2CppType *t = api.il2cpp_field_get_type(f);
            const size_t raw = api.il2cpp_field_get_offset(f);
            if (raw & 0x80000000u) continue;

            const int off = (raw >= 16) ? (int)raw - 16 : (int)raw;

            ArkValKind kind = ArkReflectKindOf(t);
            int fsz = (kind == VK_OBJECT || kind == VK_STRING) ? 8 : ArkReflectSizeOf(t);
            Il2CppClass *fcls = (t && api.il2cpp_class_from_type) ? api.il2cpp_class_from_type(t) : nullptr;

            char tname[160] = { 0 };
            ArkReflectTypeName(tname, sizeof(tname), t);

            if (kind == VK_UNKNOWN)
            {
                // TYPE_VAR / TYPE_MVAR：泛型形参，真身在实参表里。
                if (argCursor >= argc) { ++skipped; continue; }
                const char *an = args[argCursor++];
                kind = GuessSlot(an, &fsz, &fcls);
                _snprintf_s(tname, sizeof(tname), _TRUNCATE, "%s", an);
                if (kind == VK_UNKNOWN || fsz <= 0) { ++skipped; continue; }
            }

            if (off < 0 || off + fsz > valueSize) { ++skipped; continue; }

            void *addr = (char *)base + off;

            ArkInspNode *n = PushNode(depth);
            if (!n) return;
            n->kind = IKN_FIELD;
            n->field = f;
            n->owner = cur;
            n->obj = (Il2CppObject *)base;   // 仅当展开 key 用（内联结构体没有可写回的宿主对象）
            n->isStatic = 0;
            n->valKind = (int)kind;

            const char *fn = api.il2cpp_field_get_name ? api.il2cpp_field_get_name(f) : "?";
            _snprintf_s(n->name, sizeof(n->name), _TRUNCATE, "%s", SafeName(fn));
            _snprintf_s(n->type, sizeof(n->type), _TRUNCATE, "%s", tname);
            ++emitted;

            if (kind == VK_STRING || kind == VK_OBJECT)
            {
                Il2CppObject *v = *(Il2CppObject **)addr;
                n->child = v;
                if (!v)
                {
                    _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "null");
                }
                else if (kind == VK_STRING)
                {
                    char tmp[200];
                    StrToUtf8(v, tmp, sizeof(tmp));
                    _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "\"%s\"", tmp);
                }
                else
                {
                    char cls[120];
                    ClassFullName(ClassOf(v), cls, sizeof(cls));
                    _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%p  %s", (void *)v, cls);
                }

                const bool canDrill = n->child && (depth + 1 <= g_maxDepth) && !PathHas(n->child);
                n->expandable = canDrill ? 1 : 0;
                n->expanded   = canDrill && IsExpanded(base, f, -1) ? 1 : 0;
                if (n->expanded)
                {
                    PathPush(n->child);
                    Diverge(n, depth + 1);
                    PathPop();
                }
                continue;
            }

            if (kind == VK_STRUCT)
            {
                const bool drillable = fcls && nest > 1 && ClassHasFields(fcls) && (depth + 1 <= g_maxDepth);

                if (!drillable && fcls && WantsToString(fcls) &&
                    BoxAndToString(fcls, addr, n->value, sizeof(n->value)))
                {
                    // 枚举 / 加密数值：托管侧已经解出可读形式
                }
                else
                {
                    char cls[120];
                    ClassFullName(fcls, cls, sizeof(cls));
                    _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "(%s %dB)", cls, fsz);
                }

                n->expandable = drillable ? 1 : 0;
                n->expanded   = drillable && IsExpanded(base, f, -1) ? 1 : 0;
                if (n->expanded)
                    WalkStructFieldsAt(addr, fcls, depth + 1, fsz, nest - 1);
                continue;
            }

            // 标量：直接从内联地址上按自身宽度取
            switch (kind)
            {
            case VK_BOOL:
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%s",
                            *(unsigned char *)addr ? "true" : "false");
                break;
            case VK_INT:
            {
                long long v = 0;
                if (fsz == 1)      v = *(signed char *)addr;
                else if (fsz == 2) v = *(short *)addr;
                else if (fsz == 4) v = *(int *)addr;
                else               v = *(long long *)addr;
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%lld", v);
                break;
            }
            case VK_LONG:
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%lld", *(long long *)addr);
                break;
            case VK_FLOAT:
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%.4f", (double)*(float *)addr);
                break;
            case VK_DOUBLE:
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%.6f", *(double *)addr);
                break;
            default:
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "<?>");
                break;
            }
        }
        cur = api.il2cpp_class_get_parent ? api.il2cpp_class_get_parent(cur) : nullptr;
    }

    // ★ 一个字段都没展示出来时必须自述，否则面板上就是一个"点了没反应"的箭头。
    if (emitted == 0)
    {
        char dn[160];
        ShortTypeName(dn, sizeof(dn), k);
        ArkInspNode *n = PushNode(depth);
        if (n)
        {
            n->kind = IKN_ITEM;
            n->obj = (Il2CppObject *)base;
            _snprintf_s(n->name, sizeof(n->name), _TRUNCATE, "(无法展开)");
            _snprintf_s(n->type, sizeof(n->type), _TRUNCATE, "-");
            _snprintf_s(n->value, sizeof(n->value), _TRUNCATE,
                        "字段 %d / 实参 %d / 跳过 %d（%dB）", seen, argc, skipped, valueSize);
        }
        ArkLogThrottled((unsigned)((uintptr_t)k * 2654435761u),
                        "[insp] 结构体 %s 展不开：字段 %d 个，实参 %d 个，跳过 %d 个，尺寸 %d",
                        dn, seen, argc, skipped, valueSize);
    }
}

// 只画元素，不画 header。
// ★ List 的 header 由 WalkList 自己出：多一层 "items" 包装会白吃掉一级深度预算，
//   而模板里 depth 默认 3、上限 8，两层包装正好把"元素 → 结构体"卡在边界上。
static void WalkArrayItems(Il2CppObject *arr, int elemDepth, int limit)
{
    if (!arr) return;
    Il2CppApi &api = ir::Api();
    _Il2CppArray *a = (_Il2CppArray *)arr;

    Il2CppClass *ak = ClassOf(arr);
    Il2CppClass *ek = ArrayElementClass(ak);
    int elemSize = (api.il2cpp_array_element_size && ak) ? api.il2cpp_array_element_size(ak) : 8;
    if (elemSize <= 0) elemSize = 8;

    int len = (int)a->max_length;
    if (len < 0) len = 0;
    // ★ List 的 _items 是**扩容后**的数组，尾部是已删除元素的残留 ⇒ 必须按 _size 截。
    if (limit >= 0 && limit < len) len = limit;
    const int show = (len < g_maxItems) ? len : g_maxItems;

    char kindName[120];
    ShortTypeName(kindName, sizeof(kindName), ek);

    const bool isValue = ek && api.il2cpp_class_is_valuetype && api.il2cpp_class_is_valuetype(ek);
    const char *base = (const char *)arr + 0x20;

    for (int i = 0; i < show; ++i)
    {
        if (g_writeCount >= ARK_INSP_NODE_CAP) return;
        ArkInspNode *n = PushNode(elemDepth);
        if (!n) return;
        n->kind = IKN_ELEM;
        n->obj = arr;
        n->index = i;
        _snprintf_s(n->name, sizeof(n->name), _TRUNCATE, "[%d]", i);
        _snprintf_s(n->type, sizeof(n->type), _TRUNCATE, "%s", kindName);

        void *slot = (void *)(base + (size_t)i * (size_t)elemSize);

        if (!isValue && elemSize == 8)
        {
            Il2CppObject *v = *(Il2CppObject **)slot;
            n->child = v;
            if (!v)
            {
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "null");
            }
            else
            {
                char cls[120];
                ClassFullName(ClassOf(v), cls, sizeof(cls));
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%p  %s", (void *)v, cls);
            }
            const bool canDrill = n->child && (elemDepth + 1 <= g_maxDepth) && !PathHas(n->child);
            n->expandable = canDrill ? 1 : 0;
            n->expanded   = canDrill && IsExpanded(arr, nullptr, i) ? 1 : 0;
            if (n->expanded)
            {
                PathPush(n->child);
                Diverge(n, elemDepth + 1);
                PathPop();
            }
        }
        else
        {
            char tmp[160];
            // ★ 只有加密数值 / 枚举才值得绕托管侧；别的值类型直读字节。
            if (WantsToString(ek) && BoxAndToString(ek, slot, tmp, sizeof(tmp)))
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%s", tmp);
            else
            {
                char hex[80] = { 0 };
                int pos = 0;
                const int showBytes = (elemSize > 8) ? 8 : elemSize;
                for (int b = 0; b < showBytes && pos < (int)sizeof(hex) - 4; ++b)
                    pos += _snprintf_s(hex + pos, sizeof(hex) - pos, _TRUNCATE, "%02X",
                                       (unsigned char)((char *)slot)[b]);
                _snprintf_s(n->value, sizeof(n->value), _TRUNCATE, "%s%s", hex, (elemSize > 8) ? "…" : "");
            }

            // 值类型元素也能展开：结构体的字段就内联在这段字节里。
            // 不加这条，List<KeyValuePair<..>> 只能看到一串十六进制，等于没解析。
            const bool canDrill = (ek != nullptr) && (elemDepth + 1 <= g_maxDepth);
            n->expandable = canDrill ? 1 : 0;
            n->expanded   = canDrill && IsExpanded(arr, nullptr, i) ? 1 : 0;
            if (n->expanded)
                WalkStructFieldsAt(slot, ek, elemDepth + 1, elemSize, 4);
        }
    }
}

static void WalkArray(Il2CppObject *arr, int depth, const char *label)
{
    if (!arr) return;
    Il2CppApi &api = ir::Api();

    // ★ 数组长度读 max_length@0x18、数据在 +0x20（不是 il2cpp_array_length，
    //   那个在某些版本上会走完整性检查，在枚举出来的"半死"对象上可能炸）。
    const int len = (int)((_Il2CppArray *)arr)->max_length;
    Il2CppClass *ek = ArrayElementClass(ClassOf(arr));

    char kindName[120];
    ShortTypeName(kindName, sizeof(kindName), ek);

    ArkInspNode *hdr = PushNode(depth);
    if (hdr)
    {
        hdr->kind = IKN_ITEM;
        hdr->obj = arr;
        _snprintf_s(hdr->name, sizeof(hdr->name), _TRUNCATE, "%s", label ? label : "array");
        _snprintf_s(hdr->type, sizeof(hdr->type), _TRUNCATE, "%s[]", kindName);
        _snprintf_s(hdr->value, sizeof(hdr->value), _TRUNCATE, "[%d]", len);
    }

    WalkArrayItems(arr, depth + 1, len);
    (void)api;
}

static void WalkList(Il2CppObject *list, Il2CppClass *k, int depth, const char *label)
{
    Il2CppApi &api = ir::Api();

    FieldInfo *itemsF = ArkReflectFindField(k, "_items", nullptr);
    FieldInfo *sizeF  = ArkReflectFindField(k, "_size", nullptr);

    int size = 0;
    if (sizeF && api.il2cpp_field_get_value)
        api.il2cpp_field_get_value(list, sizeF, &size);

    Il2CppObject *arr = nullptr;
    if (itemsF && api.il2cpp_field_get_value_object)
        arr = api.il2cpp_field_get_value_object(itemsF, list);

    int cap = arr ? (int)((_Il2CppArray *)arr)->max_length : 0;
    if (size < 0) size = 0;
    if (cap < 0) cap = 0;
    if (size > cap) size = cap;

    char typeName[160];
    ShortTypeName(typeName, sizeof(typeName), k);

    ArkInspNode *hdr = PushNode(depth);
    if (hdr)
    {
        hdr->kind = IKN_ITEM;
        hdr->obj = list;
        _snprintf_s(hdr->name, sizeof(hdr->name), _TRUNCATE, "%s", label ? label : "list");
        _snprintf_s(hdr->type, sizeof(hdr->type), _TRUNCATE, "%s", typeName);
        // 容量直接写在 header 上，不再单独占一行 —— 深度预算比屏幕空间更值钱。
        _snprintf_s(hdr->value, sizeof(hdr->value), _TRUNCATE, "%d 项 / 容量 %d", size, cap);
    }

    // 字段定位失败要说出来。不然面板上只剩一个 "0 项"，看不出是空列表还是根本没读到。
    if (!itemsF)
    {
        ArkInspNode *e = PushNode(depth + 1);
        if (e)
        {
            e->kind = IKN_ITEM;
            e->obj = list;
            _snprintf_s(e->name, sizeof(e->name), _TRUNCATE, "(找不到 _items 字段)");
            _snprintf_s(e->type, sizeof(e->type), _TRUNCATE, "-");
            _snprintf_s(e->value, sizeof(e->value), _TRUNCATE, "泛型实例的字段表没暴露");
        }
        return;
    }

    if (!arr) return;

    // 元素直接挂在 header 下一层；上限按 _size 而不是数组容量。
    WalkArrayItems(arr, depth + 1, size);
}

// ================================================================ 对外
void ArkInspReset()
{
    InterlockedExchange(&g_nodeCount, 0);
    InterlockedExchange(&g_expandCount, 0);
    g_writeCount = 0;
    g_pathLen = 0;
}

int ArkInspBuild(Il2CppObject *root, Il2CppClass *rootClass, int maxDepth, int maxItems)
{
    g_writeCount = 0;
    InterlockedExchange(&g_nodeCount, 0);
    g_pathLen = 0;

    // 深度上限 20：字段(0)→List 头(1)→元素(2)→结构体字段(3)→结构体里的容器(4)…
    // 一层容器吃两层，8 层只够看进一层嵌套。20 层能一路下到纯数值。
    g_maxDepth = (maxDepth < 1) ? 1 : (maxDepth > ARK_INSP_DEPTH_MAX ? ARK_INSP_DEPTH_MAX : maxDepth);
    g_maxItems = (maxItems < 1) ? 1 : (maxItems > 512 ? 512 : maxItems);

    if (!root)
        return 0;

    Il2CppClass *k = rootClass ? rootClass : ClassOf(root);
    if (!k) return 0;

    PathPush(root);

    if (ClassRank(k) > 0)
        WalkArray(root, 0, "array");
    else if (NameIs(k, "List`1"))
        WalkList(root, k, 0, "list");
    else
        WalkObject(root, k, 0, true);

    PathPop();

    // ★ 撞了节点上限就明说。不然面板一路滚到底，看着像"数据就这么多"，
    //   实际是被截断了 —— 这种"静默少给你一半"的体验比报错还糟。
    if (g_writeCount >= ARK_INSP_NODE_CAP)
    {
        ArkInspNode *n = &g_nodeBuf[ARK_INSP_NODE_CAP - 1];
        const int shown = ARK_INSP_NODE_CAP - 1;
        const int depthWas = g_maxDepth;
        memset(n, 0, sizeof(*n));
        n->kind = IKN_ITEM;
        n->index = -1;
        n->depth = 0;
        _snprintf_s(n->name, sizeof(n->name), _TRUNCATE, "(节点已达上限)");
        _snprintf_s(n->type, sizeof(n->type), _TRUNCATE, "-#%d", shown);
        _snprintf_s(n->value, sizeof(n->value), _TRUNCATE,
                    "只画了前 %d 个节点（深度 %d）。收起一些分支，或把深度/条目调小",
                    shown, depthWas);
        g_writeCount = ARK_INSP_NODE_CAP;
    }

    // ★ 最后一步才发布节点数：渲染线程看到的新数量一定是"数据已写好"之后的。
    InterlockedExchange(&g_nodeCount, g_writeCount);
    return g_writeCount;
}

int ArkInspCount()
{
    return (int)InterlockedCompareExchange(&g_nodeCount, 0, 0);
}

ArkInspNode *ArkInspAt(int i)
{
    if (i < 0 || i >= ARK_INSP_NODE_CAP) return nullptr;
    return &g_nodeBuf[i];
}

void ArkInspToggle(const ArkInspNode *n)
{
    if (!n || !n->expandable) return;
    SetExpanded((void *)n->obj, (void *)n->field, n->index, n->expanded ? false : true);
}

bool ArkInspWriteNum(const ArkInspNode *n, double v)
{
    if (!n || !n->field || !n->editable) return false;
    return ArkReflectWriteFieldNum(n->field, n->isStatic ? nullptr : n->obj, v);
}

void ArkInspectSummary(Il2CppObject *obj, char *out, int cap)
{
    if (!out || cap <= 0) return;
    if (!obj) { _snprintf_s(out, cap, _TRUNCATE, "null"); return; }
    char cls[120];
    ClassFullName(ClassOf(obj), cls, sizeof(cls));
    _snprintf_s(out, cap, _TRUNCATE, "%p  %s", (void *)obj, cls);
}
