// il2cpp_module_posix.c — POSIX（Android）下的 IL2CPP 模块加载（libil2cpp.so）
#include "il2cpp_module.h"
#include <dlfcn.h>

#ifndef RTLD_NOLOAD
#define RTLD_NOLOAD 0
#endif

void *il2cpp_module_open(const char *module_name)
{
    if (!module_name || !*module_name)
        return NULL;

    // 1) 优先附着到已加载的模块，避免二次加载破坏状态
    void *handle = dlopen(module_name, RTLD_NOLOAD);
    if (handle)
        return handle;

    // 2) 容错：全局句柄解析（依赖符号已被解析到的主映像）
    void *self = dlopen(NULL, RTLD_NOW | RTLD_NOLOAD);
    if (self)
        return self;

    // 3) 兜底：尝试加载（一般会因宿主环境异常失败）
    return dlopen(module_name, RTLD_NOW | RTLD_LOCAL);
}

void *il2cpp_module_sym(void *handle, const char *symbol_name)
{
    if (!handle || !symbol_name)
        return NULL;
    // RTLD_DEFAULT 允许在未显式句柄情况下仍能找到主库符号
    return dlsym(handle, symbol_name);
}

void il2cpp_module_close(void *handle)
{
    (void)handle; // 生命周期交由宿主管理
}