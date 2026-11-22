# Makefile — il2cpp_mod.dll（手动映射注入 + ImGui overlay 模板）
#
# 组件：
#   il2cpp_core/       跨平台 IL2CPP 反射（内置，不用再去别处取）
#   src/main.cpp       引导 / 日志 / 配置 / detour 探针 / 面板骨架
#   src/probe.cpp      反射自测 + 类结构 dump
#   src/user_hooks.cpp 你的业务代码（六个扩展点）
#   src/ui/imgui_overlay.cpp  D3D11 Present vtable hook + imgui DX11 后端 + 中文字体
#   src/ui/log_panel.cpp     日志面板
#   src/log_buffer.cpp       无锁日志环形缓冲
#   src/hot_reload.cpp       单实例握手 + 卸载例程
#   third_party/imgui        ImGui 1.91.0（MIT，已本地打补丁：D3DCompile 改动态获取）
#
# 约束（手动映射场景的硬约束，改动前必读）：
#   - 不含静态 TLS：不用 std::thread / thread_local，避免 libstdc++ 的
#     __emutls_* 与 emutls 段 —— 手动映射未模拟静态 TLS，出现就会被判定为
#     "有 TLS 但没处理"。imgui 自身不用 STL，天然满足。
#   - 线程一律 Win32 CreateThread；计数用 volatile LONG + InterlockedIncrement。
#   - 全静态链接：不导入 libwinpthread-1.dll / libstdc++-6.dll / libgcc_s_*.dll。
#     导入表最终只应剩 KERNEL32/msvcrt/SHELL32/USER32 等系统 DLL。
#   - 图形侧只链 d3d11/dxgi/gdi32/user32。**不链 d3dcompiler**：imgui_impl_dx11.cpp
#     已本地打补丁，把 D3DCompile 改成运行时 LoadLibraryA("d3dcompiler_47.dll")
#     +GetProcAddress。否则导入表会出现 D3DCOMPILER_47.dll，映射器要为它远程
#     LoadLibraryW，那条路径在部分目标进程里不可靠（实测返回 NULL / err=87，
#     注入直接失败）。
#
# 依赖：mingw-w64（默认 D:/mingw64）。
#   有 make 就：  make            /   make clean
#   没有 make 就：powershell -File build.ps1   （推荐，它还做了 ABI 陈旧检查）
#
# 校验产物是否合规（可选，release 前跑一遍）：
#   objdump -h il2cpp_mod.dll | grep -c '\.tls'    # 必须是 0
#   nm build/*.o | grep -c emutls                  # 必须是 0（开启 -s 后查 .o）
#   objdump -p il2cpp_mod.dll | grep -i "DLL Name" # 只应有系统 DLL

MINGW ?= D:/mingw64
CXX   := $(MINGW)/bin/x86_64-w64-mingw32-g++.exe
CC    := $(MINGW)/bin/x86_64-w64-mingw32-gcc.exe

IL2CPP_ROOT := il2cpp_core
IMGUI       := third_party/imgui/imgui-1.91.0
INC         := -I$(IL2CPP_ROOT)/src -I$(IMGUI) -Isrc

# IL2CPP_CORE_NO_STL：剔除 il2cpp_runtime 里基于 std::string/vector 的函数，
# 避免连带拉入 libstdc++ EH/emutls/pthread。
# 不加 -fdata-sections：MinGW PE/COFF 下它会把全零静态缓冲放到 .data$*（占磁盘字节）
# 而不是 .bss，会让 DLL 凭空多出 ~4.5 MB 的零字节。
CXXFLAGS := -O2 -std=c++17 -Wall -Wno-unused-function -fno-exceptions -fno-rtti \
            -ffunction-sections \
            -DIL2CPP_CORE_NO_STL -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE
CFLAGS   := -O2 -std=c11 -Wall -ffunction-sections \
            -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601

OD := build

# ---- il2cpp_core（跨平台反射库）----
IL2CPP_CPP := $(IL2CPP_ROOT)/src/il2cpp/il2cpp_api.cpp \
              $(IL2CPP_ROOT)/src/il2cpp/il2cpp_runtime.cpp \
              $(IL2CPP_ROOT)/src/hook/il2cpp_hook.cpp
IL2CPP_C   := $(IL2CPP_ROOT)/src/platform/il2cpp_module_windows.c

# ---- 本项目 ----
OWN_CPP := src/main.cpp src/probe.cpp src/user_hooks.cpp src/exit_guard.cpp \
           src/ui/imgui_overlay.cpp src/ui/log_panel.cpp \
           src/log_buffer.cpp src/hot_reload.cpp

# ---- 工具层（类浏览器 / 对象 / 调用 / 字段监视 / 热键 / Dumper）----
# 与具体游戏无关的一整套分析工具。
# 对象名带 core_ / panel_ 前缀：src/core/invoke.cpp 与 src/panels/invoke.cpp
# 都叫 invoke.cpp，用 $(notdir) 会撞名（后编的覆盖前一个）。
CORE_CPP  := $(wildcard src/core/*.cpp)
PANEL_CPP := $(wildcard src/panels/*.cpp)
CORE_HDRS := $(wildcard src/core/*.h) $(wildcard src/panels/*.h) src/mod_config.h

IL2CPP_OBJ := $(patsubst %.cpp,$(OD)/%.o,$(notdir $(IL2CPP_CPP))) \
              $(patsubst %.c,$(OD)/%.o,$(notdir $(IL2CPP_C)))
OWN_OBJ    := $(patsubst %.cpp,$(OD)/%.o,$(notdir $(OWN_CPP)))
CORE_OBJ   := $(patsubst src/core/%.cpp,$(OD)/core_%.o,$(CORE_CPP))
PANEL_OBJ  := $(patsubst src/panels/%.cpp,$(OD)/panel_%.o,$(PANEL_CPP))

# ImGui 核心 + **仅 DX11 后端**。
# 不打 Win32 后端：它用了函数内 static（__cxa_guard_acquire/release），会把 libstdc++
# 的 EH 全链拖进来 -> eh_globals.o -> __emutls_v.* 静态 TLS。手动映射未模拟静态 TLS，
# 那样必崩。我们自己用 GetAsyncKeyState/GetCursorPos 轮询喂输入，不需要 win32 backend。
IMGUI_OBJ := $(OD)/imgui.o $(OD)/imgui_draw.o $(OD)/imgui_widgets.o $(OD)/imgui_tables.o \
             $(OD)/imgui_impl_dx11.o

LIBS := -ld3d11 -ldxgi -lgdi32 -luser32

LDFLAGS_STATIC := -static -s -Wl,--gc-sections -Wl,--exclude-all-symbols -Wl,--subsystem,windows

# mod_template.def：精确导出 Mod*/Esp*（热重载接口）与 DllMainCRTStartup。
# 必须有它 —— --exclude-all-symbols 会把 extern "C" 的可见性也一并去掉，
# 注入器的 --replace 就找不到 ModUnload 了。
EXPORT_DEF := mod_template.def

TARGET := il2cpp_mod.dll

all: $(TARGET)

$(OD):
	@if not exist $(OD) mkdir $(OD)

# ---- il2cpp_core objects ----
$(OD)/il2cpp_api.o: $(IL2CPP_ROOT)/src/il2cpp/il2cpp_api.cpp | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/il2cpp_runtime.o: $(IL2CPP_ROOT)/src/il2cpp/il2cpp_runtime.cpp | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/il2cpp_hook.o: $(IL2CPP_ROOT)/src/hook/il2cpp_hook.cpp | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/il2cpp_module_windows.o: $(IL2CPP_ROOT)/src/platform/il2cpp_module_windows.c | $(OD)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

# ---- 本项目（用 vpath 让 $(notdir) 出来的规则能找到带目录的源文件）----
vpath %.cpp src src/ui
vpath %.c   src

$(OD)/main.o: src/main.cpp src/mod_config.h src/log_buffer.h src/probe.h \
              src/ui/log_panel.h src/ui/ui_api.h | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/probe.o: src/probe.cpp src/probe.h src/mod_config.h | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/user_hooks.o: src/user_hooks.cpp src/mod_config.h src/probe.h | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/exit_guard.o: src/exit_guard.cpp src/exit_guard.h | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/imgui_overlay.o: src/ui/imgui_overlay.cpp src/ui/ui_api.h src/exit_guard.h | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/log_panel.o: src/ui/log_panel.cpp src/ui/log_panel.h src/log_buffer.h \
                   src/mod_config.h | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/log_buffer.o: src/log_buffer.cpp src/log_buffer.h | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/hot_reload.o: src/hot_reload.cpp src/mod_config.h src/ui/ui_api.h src/exit_guard.h | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

# ---- imgui ----
$(OD)/imgui.o: $(IMGUI)/imgui.cpp | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/imgui_draw.o: $(IMGUI)/imgui_draw.cpp | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/imgui_widgets.o: $(IMGUI)/imgui_widgets.cpp | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/imgui_tables.o: $(IMGUI)/imgui_tables.cpp | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/imgui_impl_dx11.o: $(IMGUI)/backends/imgui_impl_dx11.cpp | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

# ---- 工具层 ----
# 依赖挂「全部工具层头文件」：这一层互相引用密，逐个列头文件一定会漏
#（漏了就是"改了没生效"的幽灵 bug）。
$(OD)/core_%.o: src/core/%.cpp $(CORE_HDRS) | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
$(OD)/panel_%.o: src/panels/%.cpp $(CORE_HDRS) | $(OD)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@
# 注意：故意**没有** imgui_impl_win32 规则 —— 见文件头的说明（会引入 emutls 静态 TLS）。

$(TARGET): $(OWN_OBJ) $(CORE_OBJ) $(PANEL_OBJ) $(IL2CPP_OBJ) $(IMGUI_OBJ) $(EXPORT_DEF) | $(OD)
	$(CXX) -shared -o $@ $(OWN_OBJ) $(CORE_OBJ) $(PANEL_OBJ) $(IL2CPP_OBJ) $(IMGUI_OBJ) $(LDFLAGS_STATIC) $(EXPORT_DEF) $(LIBS)

clean:
	@if exist $(OD) rmdir /s /q $(OD)
	@if exist $(TARGET) del /q $(TARGET)

.PHONY: all clean
