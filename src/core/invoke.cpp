// invoke.cpp — 方法调用器实现
// ============================================================================
#include "core/invoke.h"
#include "core/log.h"
#include "core/reflect.h"
#include "core/vec.h"

#include "il2cpp/il2cpp_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ir = il2cpp::runtime;

// 固定容量（理由同 inspect.cpp）：Worker 枚举、渲染线程遍历，同一片内存。
// 绝不 realloc，宁可截断。
#define ARK_METHOD_CAP 1200
static ArkMethodEntry  g_methodBuf[ARK_METHOD_CAP];
static volatile LONG   g_methodCount = 0;

// MethodInfo 在 types.h 里是完整定义（只有前两个字段），methodPointer 就在这里。
static void *MethodPtr(MethodInfo *m)
{
    return m ? ((MethodInfoHeader *)m)->methodPointer : nullptr;
}

static void TypeLabel(Il2CppType *t, char *out, int cap)
{
    ArkReflectTypeName(out, cap, t);
}

// ================================================================ 参数签名
int ArkInvokePrepare(MethodInfo *m, ArkParamSlot *out, int cap)
{
    if (!m || !out || cap <= 0) return 0;
    Il2CppApi &api = ir::Api();

    memset(out, 0, sizeof(ArkParamSlot) * (size_t)cap);

    const uint32_t pc = api.il2cpp_method_get_param_count ? api.il2cpp_method_get_param_count(m) : 0;
    const int n = (pc < (uint32_t)cap) ? (int)pc : cap;

    for (int i = 0; i < n; ++i)
    {
        ArkParamSlot &s = out[i];

        Il2CppType *t = api.il2cpp_method_get_param ? api.il2cpp_method_get_param(m, (uint32_t)i) : nullptr;
        if (!t)
        {
            s.unsupported = 1;
            _snprintf_s(s.name, sizeof(s.name), _TRUNCATE, "arg%d", i);
            _snprintf_s(s.typeName, sizeof(s.typeName), _TRUNCATE, "?");
            continue;
        }

        const char *pn = api.il2cpp_method_get_param_name ? api.il2cpp_method_get_param_name(m, (uint32_t)i) : nullptr;
        _snprintf_s(s.name, sizeof(s.name), _TRUNCATE, "%s", (pn && pn[0]) ? pn : "");
        if (!s.name[0])
            _snprintf_s(s.name, sizeof(s.name), _TRUNCATE, "arg%d", i);

        TypeLabel(t, s.typeName, sizeof(s.typeName));

        s.kind = (int)ArkReflectKindOf(t);
        s.klass = api.il2cpp_class_from_type ? api.il2cpp_class_from_type(t) : nullptr;

        if (s.klass && api.il2cpp_class_is_enum && api.il2cpp_class_is_enum(s.klass))
            s.isEnum = 1;
        if (s.kind == VK_STRING)
            s.isString = 1;

        if (s.kind == VK_STRUCT && !s.isEnum)
        {
            // 自建结构体参数（Vector2/自定义 struct）：没法从字符串构造出来。
            // 与其猜一个布局去填，不如明确标出来 —— 猜错的后果是往游戏里写垃圾。
            s.unsupported = 1;
        }
    }

    return n;
}

// ================================================================ 装箱
// 按参数类型把用户输入的文本装箱成 Il2CppObject*。失败返回 nullptr。
static Il2CppObject *BoxValue(ArkParamSlot &s)
{
    Il2CppApi &api = ir::Api();
    if (!s.kind) return nullptr;

    if (s.isString)
        return api.il2cpp_string_new ? (Il2CppObject *)api.il2cpp_string_new(s.text) : nullptr;

    if (s.kind == VK_OBJECT)
        return s.obj;

    if (!s.klass || !api.il2cpp_value_box) return nullptr;

    if (s.isEnum)
    {
        // 枚举允许填名字（"Stun"）或数字。名字要按静态字面量查，查不到再按数字。
        const char *txt = s.text;
        if (txt[0] && (txt[0] < '0' || txt[0] > '9') && txt[0] != '-')
        {
            FieldInfo *lf = api.il2cpp_class_get_field_from_name
                                ? api.il2cpp_class_get_field_from_name(s.klass, txt)
                                : nullptr;
            if (lf && api.il2cpp_field_static_get_value)
            {
                int32_t v = 0;
                api.il2cpp_field_static_get_value(lf, &v);
                return api.il2cpp_value_box(s.klass, &v);
            }
        }
        int32_t v = (int32_t)strtol(txt, nullptr, 0);
        return api.il2cpp_value_box(s.klass, &v);
    }

    switch (s.kind)
    {
    case VK_BOOL:
    {
        // 用 int32 承载：C# 的 bool 在装箱后是 1 字节，但 il2cpp 的装箱按类大小拷贝，
        // 传一个 4 字节整数进去只会读低字节，稳。
        int32_t v = (strcmp(s.text, "true") == 0 || strcmp(s.text, "1") == 0) ? 1 : 0;
        return api.il2cpp_value_box(s.klass, &v);
    }
    case VK_INT:
    {
        // value_box 按**目标类的大小**拷字节，所以统一用 int32 承载，
        // 1/2 字节的类型只会拷到低位，不会写坏内存。
        int32_t v = (int32_t)strtoll(s.text, nullptr, 0);
        return api.il2cpp_value_box(s.klass, &v);
    }
    case VK_LONG:
    {
        long long v = strtoll(s.text, nullptr, 0);
        return api.il2cpp_value_box(s.klass, &v);
    }
    case VK_FLOAT:
    {
        float v = (float)atof(s.text);
        return api.il2cpp_value_box(s.klass, &v);
    }
    case VK_DOUBLE:
    {
        double v = atof(s.text);
        return api.il2cpp_value_box(s.klass, &v);
    }
    default:
        return nullptr;
    }
}

// ================================================================ 调用
static void FormatResult(Il2CppObject *r, MethodInfo *m, char *out, int cap)
{
    Il2CppApi &api = ir::Api();
    if (!r) { _snprintf_s(out, cap, _TRUNCATE, "void / null"); return; }

    Il2CppClass *rk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(r) : r->klass;
    const char *nm = (rk && api.il2cpp_class_get_name) ? api.il2cpp_class_get_name(rk) : "";

    if (nm && strcmp(nm, "String") == 0 && api.il2cpp_string_chars && api.il2cpp_string_length)
    {
        const int32_t len = api.il2cpp_string_length((Il2CppString *)r);
        const Il2CppChar *ch = api.il2cpp_string_chars((Il2CppString *)r);
        char tmp[300] = { 0 };
        if (ch && len > 0)
        {
            const int need = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)ch, len, nullptr, 0, nullptr, nullptr);
            if (need > 0)
            {
                const int cut = (need < (int)sizeof(tmp)) ? need : (int)sizeof(tmp) - 4;
                WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)ch, len, tmp, cut, nullptr, nullptr);
            }
        }
        _snprintf_s(out, cap, _TRUNCATE, "\"%s\"", tmp);
        return;
    }

    // 其余统一问 ToString()。值类型要先 unbox，引用类型直接把对象当 this。
    if (api.il2cpp_class_get_method_from_name && api.il2cpp_runtime_invoke)
    {
        MethodInfo *ts = api.il2cpp_class_get_method_from_name(rk, "ToString", 0);
        if (ts && MethodPtr(ts))
        {
            const bool isValue = api.il2cpp_class_is_valuetype && api.il2cpp_class_is_valuetype(rk);
            Il2CppException *e2 = nullptr;
            Il2CppObject *s = nullptr;
            if (isValue && api.il2cpp_object_unbox)
                s = api.il2cpp_runtime_invoke(ts, api.il2cpp_object_unbox(r), nullptr, &e2);
            else
                s = api.il2cpp_runtime_invoke(ts, r, nullptr, &e2);
            if (!e2 && s && api.il2cpp_string_chars)
            {
                const int32_t len = api.il2cpp_string_length((Il2CppString *)s);
                char tmp[300] = { 0 };
                const Il2CppChar *ch = api.il2cpp_string_chars((Il2CppString *)s);
                if (ch && len > 0)
                {
                    const int need = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)ch, len, nullptr, 0, nullptr, nullptr);
                    if (need > 0)
                    {
                        const int cut = (need < (int)sizeof(tmp)) ? need : (int)sizeof(tmp) - 4;
                        WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)ch, len, tmp, cut, nullptr, nullptr);
                    }
                }
                _snprintf_s(out, cap, _TRUNCATE, "%s  (%p)", tmp, (void *)r);
                return;
            }
        }
    }

    _snprintf_s(out, cap, _TRUNCATE, "%p  %s", (void *)r, nm);
}

bool ArkInvokeCall(MethodInfo *m, Il2CppObject *thisObj, ArkParamSlot *slots, int n,
                   char *out, int outCap)
{
    if (!out || outCap <= 0) return false;
    out[0] = 0;

    Il2CppApi &api = ir::Api();
    if (!m || !api.il2cpp_runtime_invoke_convert_args)
    {
        _snprintf_s(out, outCap, _TRUNCATE, "失败：runtime_invoke_convert_args 不可用");
        return false;
    }

    if (!MethodPtr(m))
    {
        _snprintf_s(out, outCap, _TRUNCATE,
                    "失败：这是泛型定义（methodPointer 为空），没有方法体可调");
        return false;
    }

    const bool isStatic = api.il2cpp_method_is_instance ? !api.il2cpp_method_is_instance(m) : false;
    if (!isStatic && !thisObj)
    {
        _snprintf_s(out, outCap, _TRUNCATE, "失败：实例方法需要先选一个 this 对象");
        return false;
    }

    Il2CppObject *args[ARK_INVOKE_MAX_PARAMS] = { nullptr };
    for (int i = 0; i < n && i < ARK_INVOKE_MAX_PARAMS; ++i)
    {
        if (slots[i].useSel)
        {
            _snprintf_s(out, outCap, _TRUNCATE,
                        "失败：参数 %s 标了「用当前选中对象」但调用方没有解析它", slots[i].name);
            return false;
        }

        if (slots[i].unsupported)
        {
            _snprintf_s(out, outCap, _TRUNCATE, "失败：参数 %s 的类型（%s）暂不支持构造",
                        slots[i].name, slots[i].typeName);
            return false;
        }
        // ★ 引用类型允许显式传 null。
        //   游戏里很多方法末位是 `Action cb` / `SomeClass opt`，默认值就是 null，
        //   而 BoxValue 对 VK_OBJECT 只会原样返回 slots[i].obj —— 也就是空。
        //   以前这里一律判失败 ⇒ `FinishGame(WIN, false, null)` 这类调用根本发不出去。
        const bool nullRef = (slots[i].kind == VK_OBJECT && !slots[i].obj &&
                              (slots[i].text[0] == 0 || strcmp(slots[i].text, "null") == 0));

        args[i] = BoxValue(slots[i]);
        if (!args[i] && !nullRef)
        {
            _snprintf_s(out, outCap, _TRUNCATE, "失败：参数 %s 无法装箱（检查输入格式）", slots[i].name);
            return false;
        }
    }

    Il2CppException *exc = nullptr;
    Il2CppObject *r = api.il2cpp_runtime_invoke_convert_args(m, isStatic ? nullptr : thisObj, args, n, &exc);

    if (exc)
    {
        char msg[512] = { 0 };
        if (api.il2cpp_format_exception)
            api.il2cpp_format_exception(exc, msg, sizeof(msg) - 1);
        _snprintf_s(out, outCap, _TRUNCATE, "托管异常：%s", msg[0] ? msg : "(无消息)");
        Log("[invoke] 方法抛异常：%s", msg[0] ? msg : "(无消息)");
        return false;
    }

    FormatResult(r, m, out, outCap);
    return true;
}

// ================================================================ 方法列表
int ArkInvokeListMethods(Il2CppClass *k, int includeParents, int maxOut)
{
    InterlockedExchange(&g_methodCount, 0);
    if (!k || !maxOut) return 0;
    if (maxOut > ARK_METHOD_CAP) maxOut = ARK_METHOD_CAP;
    int written = 0;

    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_class_get_methods) return 0;

    Il2CppClass *cur = k;
    int guard = 0;
    while (cur && guard++ < 16)
    {
        void *iter = nullptr;
        MethodInfo *m = nullptr;
        while ((m = api.il2cpp_class_get_methods(cur, &iter)) != nullptr)
        {
            if (written >= maxOut) { InterlockedExchange(&g_methodCount, written); return written; }

            void *ptr = MethodPtr(m);
            const bool isGen = (ptr == nullptr) ||
                               (api.il2cpp_method_is_generic && api.il2cpp_method_is_generic(m));

            ArkMethodEntry *e = &g_methodBuf[written++];
            memset(e, 0, sizeof(*e));
            e->method = m;
            e->isGenericDef = isGen ? 1 : 0;
            e->isStatic = (api.il2cpp_method_is_instance && !api.il2cpp_method_is_instance(m)) ? 1 : 0;
            e->paramCount = (int)(api.il2cpp_method_get_param_count ? api.il2cpp_method_get_param_count(m) : 0);

            // 标签："返回类型 名字(参数类型…)"。返回类型要单独取，别和参数混。
            char ret[64] = { 0 };
            if (api.il2cpp_method_get_return_type)
                ArkReflectTypeName(ret, sizeof(ret), api.il2cpp_method_get_return_type(m));

            const char *nm = api.il2cpp_method_get_name ? api.il2cpp_method_get_name(m) : "?";

            char par[160] = { 0 };
            int pos = 0;
            const int pc = e->paramCount;
            for (int i = 0; i < pc && pos < (int)sizeof(par) - 40; ++i)
            {
                Il2CppType *pt = api.il2cpp_method_get_param ? api.il2cpp_method_get_param(m, (uint32_t)i) : nullptr;
                char tn[48] = { 0 };
                ArkReflectTypeName(tn, sizeof(tn), pt);
                pos += _snprintf_s(par + pos, sizeof(par) - pos, _TRUNCATE, "%s%s", i ? ", " : "", tn);
            }

            _snprintf_s(e->label, sizeof(e->label), _TRUNCATE, "%s%s %s(%s)%s",
                        e->isStatic ? "[static] " : "",
                        ret[0] ? ret : "void", nm, par,
                        isGen ? "   (泛型定义/无方法体)" : "");
        }

        if (!includeParents) break;
        cur = api.il2cpp_class_get_parent ? api.il2cpp_class_get_parent(cur) : nullptr;
    }

    InterlockedExchange(&g_methodCount, written);
    return written;
}

int ArkInvokeMethodCount()
{
    return (int)InterlockedCompareExchange(&g_methodCount, 0, 0);
}

ArkMethodEntry *ArkInvokeMethodAt(int i)
{
    if (i < 0 || i >= ARK_METHOD_CAP) return nullptr;
    return &g_methodBuf[i];
}
