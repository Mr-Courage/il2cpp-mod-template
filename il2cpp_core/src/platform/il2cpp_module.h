// il2cpp_module.h — 平台无关的 IL2CPP 运行库加载器接口
// Windows: GetModuleHandle/GetProcAddress（GameAssembly.dll）
// Android : dlopen/dlsym（libil2cpp.so）
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 返回已加载模块句柄（优先 GetModuleHandle 附着到游戏进程内已加载的实例）
void *il2cpp_module_open(const char *module_name);
// 从模块句柄解析导出符号；失败返回 nullptr
void *il2cpp_module_sym(void *handle, const char *symbol_name);
// 若句柄是自行加载（非附着）则卸载；附着句柄调用是无害的空操作
void il2cpp_module_close(void *handle);

#ifdef __cplusplus
}
#endif