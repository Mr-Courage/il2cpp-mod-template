// il2cpp_module_windows.c — Windows 下的 IL2CPP 模块加载（GameAssembly.dll）
#include "il2cpp_module.h"
#include <windows.h>
#include <string.h>

void *il2cpp_module_open(const char *module_name)
{
    if (!module_name || !*module_name)
        return NULL;

    // 1) 优先附着到游戏进程内已加载的模块，避免二次加载破坏状态
    HMODULE attached = GetModuleHandleA(module_name);
    if (attached)
        return (void *)attached;

    // 2) 进程外/独立运行兜底：尝试加载（一般会因宿主缺失失败）
    HMODULE loaded = LoadLibraryA(module_name);
    return (void *)loaded;
}

void *il2cpp_module_sym(void *handle, const char *symbol_name)
{
    if (!handle || !symbol_name)
        return NULL;
    return (void *)GetProcAddress((HMODULE)handle, symbol_name);
}

void il2cpp_module_close(void *handle)
{
    // 附着句柄不应 FreeLibrary；自行加载的句柄可安全释放。
    // 为简单与安全起见，此库不负责卸载，交由宿主管理生命周期。
    (void)handle;
}