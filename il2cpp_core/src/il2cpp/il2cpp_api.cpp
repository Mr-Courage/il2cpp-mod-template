// il2cpp_api.cpp — 导出表解析实现
#include "il2cpp_api.h"
#include "platform/il2cpp_module.h"
#include <string.h>

void il2cpp_api_reset(Il2CppApi *api)
{
    if (!api)
        return;
    memset(api, 0, sizeof(*api));
}

int il2cpp_api_resolve(Il2CppApi *api, void *module_handle)
{
    if (!api || !module_handle)
        return 0;

    int ok = 0;

#define DO_API(r, n, p)                                                                                                \
    {                                                                                                                  \
        api->n = (r(*) p) il2cpp_module_sym(module_handle, #n);                                                       \
        if (api->n)                                                                                                    \
            ++ok;                                                                                                      \
        else                                                                                                           \
            api->n = NULL;                                                                                             \
    }
#define DO_API_NO_RETURN(r, n, p) DO_API(r, n, p)
#include "il2cpp/il2cpp_api_functions.h"
#undef DO_API
#undef DO_API_NO_RETURN

    return ok;
}