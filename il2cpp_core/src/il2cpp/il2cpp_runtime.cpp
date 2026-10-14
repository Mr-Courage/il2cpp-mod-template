// il2cpp_runtime.cpp — 高层 IL2CPP 反射封装实现
#include "il2cpp_runtime.h"
#include "platform/il2cpp_module.h"

#include <cstdio>
#include <cstring>

namespace il2cpp
{
namespace runtime
{

namespace
{
Il2CppApi g_api;
bool g_ready = false;

const char *DefaultModuleName()
{
#ifdef _WIN32
    return "GameAssembly.dll";
#else
    return "libil2cpp.so";
#endif
}

const char *LastError()
{
#ifdef _WIN32
    static char buf[256];
    snprintf(buf, sizeof(buf), "(windows) module not attached");
    return buf;
#else
    return "(posix) module not attached";
#endif
}

} // namespace

Il2CppApi &Api()
{
    return g_api;
}

bool Ready()
{
    return g_ready && g_api.il2cpp_domain_get != nullptr;
}

bool Init(const char *module_name)
{
    if (g_ready)
        return true;

    const char *name = module_name ? module_name : DefaultModuleName();
    void *handle = il2cpp_module_open(name);
    if (!handle)
    {
        std::fprintf(stderr, "[il2cpp] open module '%s' failed: %s\n", name, LastError());
        return false;
    }

    il2cpp_api_reset(&g_api);
    int n = il2cpp_api_resolve(&g_api, handle);
    std::printf("[il2cpp] module '%s' resolved %d exports\n", name, n);

    g_ready = (g_api.il2cpp_domain_get != nullptr && g_api.il2cpp_domain_assembly_open != nullptr);
    if (!g_ready)
        std::fprintf(stderr, "[il2cpp] module '%s' missing critical exports\n", name);
    return g_ready;
}

bool EnsureAttached()
{
    if (!Ready())
        return false;
    if (g_api.il2cpp_thread_current && g_api.il2cpp_thread_current())
        return true;
    auto *domain = Domain();
    if (!domain)
        return false;
    auto *thread = g_api.il2cpp_thread_attach(domain);
    return thread != nullptr;
}

void Detach()
{
    if (!g_ready || !g_api.il2cpp_thread_current)
        return;
    auto *self = g_api.il2cpp_thread_current();
    if (self && g_api.il2cpp_thread_detach)
        g_api.il2cpp_thread_detach(self);
}

Il2CppDomain *Domain()
{
    return g_api.il2cpp_domain_get ? g_api.il2cpp_domain_get() : nullptr;
}

Il2CppAssembly *Assembly(const char *name)
{
    if (!g_ready || !g_api.il2cpp_domain_assembly_open)
        return nullptr;
    return g_api.il2cpp_domain_assembly_open(Domain(), name);
}

Il2CppImage *Image(Il2CppAssembly *assembly)
{
    return (assembly && g_api.il2cpp_assembly_get_image) ? g_api.il2cpp_assembly_get_image(assembly) : nullptr;
}

Il2CppImage *Image(const char *assemblyName)
{
    return Image(Assembly(assemblyName));
}

// 以下四个函数使用 std::vector / std::string。定义 IL2CPP_CORE_NO_STL 时整体编译剔除，
// 让产物不引用 libstdc++ 的 STL 目标文件——否则链接器会连带拉入 EH/emutls/pthread，
// 在 DLL 里生成静态 TLS 段（.tls + TLS 回调），使「手动映射（不模拟静态 TLS）」失败。
#ifndef IL2CPP_CORE_NO_STL
std::vector<Il2CppImage *> Images()
{
    std::vector<Il2CppImage *> out;
    if (!g_ready || !g_api.il2cpp_domain_get_assemblies)
        return out;
    size_t size = 0;
    auto *assemblies = g_api.il2cpp_domain_get_assemblies(Domain(), &size);
    if (!assemblies)
        return out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i)
    {
        auto *img = Image(assemblies[i]);
        if (img)
            out.push_back(img);
    }
    return out;
}
#endif

Il2CppClass *ClassFromNS(Il2CppImage *image, const char *ns, const char *name)
{
    if (!image || !name || !g_api.il2cpp_class_from_name)
        return nullptr;
    return g_api.il2cpp_class_from_name(image, ns ? ns : "", name);
}

#ifndef IL2CPP_CORE_NO_STL
Il2CppClass *FindClass(const char *ns, const char *name)
{
    for (auto *img : Images())
    {
        auto *klass = ClassFromNS(img, ns, name);
        if (klass)
            return klass;
    }
    return nullptr;
}

Il2CppClass *FindClass(const char *fullName)
{
    if (!fullName || !*fullName)
        return nullptr;
    std::string s = fullName;
    size_t dot = s.rfind('.');
    if (dot == std::string::npos)
        return FindClass("", fullName);
    return FindClass(s.substr(0, dot).c_str(), s.substr(dot + 1).c_str());
}

std::string ClassFullName(Il2CppClass *klass)
{
    if (!klass)
        return "";
    const char *ns = g_api.il2cpp_class_get_namespace ? g_api.il2cpp_class_get_namespace(klass) : nullptr;
    const char *nm = g_api.il2cpp_class_get_name ? g_api.il2cpp_class_get_name(klass) : nullptr;
    std::string out;
    if (ns && *ns)
    {
        out += ns;
        out += '.';
    }
    out += nm ? nm : "";
    return out;
}
#endif

FieldInfo *ClassField(Il2CppClass *klass, const char *name)
{
    if (!klass || !name || !g_api.il2cpp_class_get_field_from_name)
        return nullptr;
    return g_api.il2cpp_class_get_field_from_name(klass, name);
}

bool ReadField(Il2CppObject *object, FieldInfo *field, void *outValue)
{
    if (!object || !field || !outValue || !g_api.il2cpp_field_get_value)
        return false;
    g_api.il2cpp_field_get_value(object, field, outValue);
    return true;
}

bool ReadStaticField(FieldInfo *field, void *outValue)
{
    if (!field || !outValue || !g_api.il2cpp_field_static_get_value)
        return false;
    g_api.il2cpp_field_static_get_value(field, outValue);
    return true;
}

Il2CppObject *ReadFieldObject(Il2CppObject *object, FieldInfo *field)
{
    if (!field || !g_api.il2cpp_field_get_value_object)
        return nullptr;
    return g_api.il2cpp_field_get_value_object(field, object);
}

bool WriteField(Il2CppObject *object, FieldInfo *field, const void *value)
{
    if (!object || !field || !value || !g_api.il2cpp_field_set_value)
        return false;
    // il2cpp_field_set_value 预期非法输入会静默失败；这里仅做基础判空。
    g_api.il2cpp_field_set_value(object, field, const_cast<void *>(value));
    return true;
}

MethodInfo *ClassMethod(Il2CppClass *klass, const char *name, int argsCount)
{
    if (!klass || !name || !g_api.il2cpp_class_get_method_from_name)
        return nullptr;
    return g_api.il2cpp_class_get_method_from_name(klass, name, argsCount);
}

Il2CppObject *Invoke(MethodInfo *method, void *object, void **params)
{
    if (!method || !g_api.il2cpp_runtime_invoke)
        return nullptr;
    Il2CppException *exc = nullptr;
    auto *ret = g_api.il2cpp_runtime_invoke(method, object, params, &exc);
    if (exc)
    {
        if (g_api.il2cpp_format_exception)
        {
            char emsg[256] = {0};
            g_api.il2cpp_format_exception(exc, emsg, (int)sizeof(emsg) - 1);
            std::fprintf(stderr, "[il2cpp] runtime_invoke threw: %s\n", emsg);
        }
        return nullptr;
    }
    return ret;
}

#ifndef IL2CPP_CORE_NO_STL
std::string DotNetStringToUtf8(Il2CppObject *stringObj)
{
    if (stringObj && g_api.il2cpp_string_chars && g_api.il2cpp_string_length)
    {
        auto *cs = g_api.il2cpp_string_chars((Il2CppString *)stringObj);
        int32_t len = g_api.il2cpp_string_length((Il2CppString *)stringObj);
        if (cs && len > 0)
        {
            // 简单把 UTF-16 转 UTF-8；对 ASCII 前缀常用场景足够。
            std::string out;
            out.reserve((size_t)len);
            for (int32_t i = 0; i < len; ++i)
            {
                Il2CppChar c = cs[i];
                if (c < 0x80)
                    out.push_back((char)c);
                else if (c < 0x800)
                {
                    out.push_back((char)(0xC0 | (c >> 6)));
                    out.push_back((char)(0x80 | (c & 0x3F)));
                }
                else
                {
                    out.push_back((char)(0xE0 | (c >> 12)));
                    out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (c & 0x3F)));
                }
            }
            return out;
        }
    }
    return std::string();
}
#endif

int32_t UnboxInt(Il2CppObject *boxed)
{
    if (!boxed || !g_api.il2cpp_object_unbox)
        return 0;
    void *p = g_api.il2cpp_object_unbox(boxed);
    if (!p)
        return 0;
    int32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace runtime
} // namespace il2cpp