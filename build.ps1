# build.ps1 — 一条命令构建 il2cpp_mod.dll（推荐入口：它还做了 ABI 陈旧检查）
#
#   .\build.ps1            增量构建（对象已存在且依赖没变则跳过）
#   .\build.ps1 -Clean     先清 build/ 再全量构建
#
# 依赖：mingw-w64。默认 D:/mingw64，可用环境变量 MINGW 覆盖。
param([switch]$Clean)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$MINGW  = if ($env:MINGW) { $env:MINGW } else { "D:/mingw64" }
$CXX    = "$MINGW/bin/x86_64-w64-mingw32-g++.exe"
$CC     = "$MINGW/bin/x86_64-w64-mingw32-gcc.exe"
$IL2CPP = "il2cpp_core"
$IMGUI  = "third_party/imgui/imgui-1.91.0"
$OD     = "build"
$TARGET = "il2cpp_mod.dll"

if (-not (Test-Path $CXX)) { Write-Host "[!] 找不到 g++: $CXX" -ForegroundColor Red; exit 1 }
if (-not (Test-Path "$IMGUI/imgui.cpp")) { Write-Host "[!] 找不到 imgui: $IMGUI" -ForegroundColor Red; exit 1 }

if ($Clean) {
    if (Test-Path $OD) { Remove-Item -Recurse -Force $OD }
    if (Test-Path $TARGET) { Remove-Item -Force $TARGET }
}
if (-not (Test-Path $OD)) { New-Item -ItemType Directory -Path $OD | Out-Null }

# IL2CPP_CORE_NO_STL / -fno-exceptions / -static 都是硬约束，见 Makefile 头注释
$CXXFLAGS = @(
    "-O2","-std=c++17","-Wall","-Wno-unused-function",
    "-fno-exceptions","-fno-rtti","-ffunction-sections","-fdata-sections",
    "-DIL2CPP_CORE_NO_STL","-DWIN32_LEAN_AND_MEAN","-D_WIN32_WINNT=0x0601",
    "-DUNICODE","-D_UNICODE"
)
$CFLAGS   = @("-O2","-std=c11","-Wall","-ffunction-sections","-fdata-sections",
               "-DWIN32_LEAN_AND_MEAN","-D_WIN32_WINNT=0x0601")
$INC      = @("-I$IL2CPP/src", "-I$IMGUI", "-Isrc")
# 不链 d3dcompiler / dwmapi：
#   imgui_impl_dx11.cpp 已本地打补丁 —— D3DCompile 改运行时 LoadLibraryA+GetProcAddress，
#   所以导入表不会出现 D3DCOMPILER_47.dll（否则映射器要远程 LoadLibraryW 它，
#   那条路径在部分目标进程里不可靠，实测返回 NULL/err=87 直接让注入失败）。
#   dwmapi 只被 win32 backend 用，而我们不编那个。
$LIBS     = @("-ld3d11","-ldxgi","-lgdi32","-luser32")

# 每个条目: 对象名 = @{ 源文件, 编译器, 额外 flags }
$units = [ordered]@{
    "il2cpp_api"            = @{ src = "$IL2CPP/src/il2cpp/il2cpp_api.cpp";           cc = $CXX; fl = $CXXFLAGS }
    "il2cpp_runtime"        = @{ src = "$IL2CPP/src/il2cpp/il2cpp_runtime.cpp";       cc = $CXX; fl = $CXXFLAGS }
    "il2cpp_hook"           = @{ src = "$IL2CPP/src/hook/il2cpp_hook.cpp";            cc = $CXX; fl = $CXXFLAGS }
    "il2cpp_module_windows" = @{ src = "$IL2CPP/src/platform/il2cpp_module_windows.c"; cc = $CC;  fl = $CFLAGS  }
    "main"                  = @{ src = "src/main.cpp";                                cc = $CXX; fl = $CXXFLAGS }
    "probe"                 = @{ src = "src/probe.cpp";                               cc = $CXX; fl = $CXXFLAGS }
    "user_hooks"            = @{ src = "src/user_hooks.cpp";                          cc = $CXX; fl = $CXXFLAGS }
    "exit_guard"            = @{ src = "src/exit_guard.cpp";                          cc = $CXX; fl = $CXXFLAGS }
    "imgui_overlay"         = @{ src = "src/ui/imgui_overlay.cpp";                    cc = $CXX; fl = $CXXFLAGS }
    "log_panel"             = @{ src = "src/ui/log_panel.cpp";                        cc = $CXX; fl = $CXXFLAGS }
    "log_buffer"            = @{ src = "src/log_buffer.cpp";                          cc = $CXX; fl = $CXXFLAGS }
    "hot_reload"            = @{ src = "src/hot_reload.cpp";                          cc = $CXX; fl = $CXXFLAGS }
    "imgui"                 = @{ src = "$IMGUI/imgui.cpp";                            cc = $CXX; fl = $CXXFLAGS }
    "imgui_draw"            = @{ src = "$IMGUI/imgui_draw.cpp";                       cc = $CXX; fl = $CXXFLAGS }
    "imgui_widgets"         = @{ src = "$IMGUI/imgui_widgets.cpp";                    cc = $CXX; fl = $CXXFLAGS }
    "imgui_tables"          = @{ src = "$IMGUI/imgui_tables.cpp";                     cc = $CXX; fl = $CXXFLAGS }
    "imgui_impl_dx11"       = @{ src = "$IMGUI/backends/imgui_impl_dx11.cpp";         cc = $CXX; fl = $CXXFLAGS }
    # 注意：故意**不编** imgui_impl_win32 —— 它用函数内 static（__cxa_guard_*），
    # 会把 libstdc++ EH 全链拖进来 -> eh_globals.o -> __emutls_v.* 静态 TLS。
    # 手动映射未模拟静态 TLS，那样必崩。输入由 imgui_overlay.cpp 自己轮询喂。
}

$objs = @()
$failed = $false

# ---------------------------------------------------------------------
# Il2CppApi ABI 检查
#
# 背景：il2cpp_api.h 用 #include 把 il2cpp_api_functions.h 的 DO_API 列表
# 直接展开进 struct Il2CppApi。**每个 TU 各自展开一份**。
# 所以往列表里增删一个成员，所有引用 Il2CppApi 的 .o 的字段偏移都会变。
#
# 踩过的坑：只重编了写入者 il2cpp_api.o，读者 il2cpp_runtime.o 还是旧的 ->
# `g_api.il2cpp_domain_get` 按旧偏移读，拿到另一个函数的指针 -> 一调就崩，
# 而且崩点离改动处很远，极难定位（当时表现是"注入后闪退"，日志停在
# `overlay installed` 之后、domain attached 之前，看起来完全无关）。
#
# 对策：这些头一旦变动，**所有** .o 都必须重编。这里做时间戳检查来拦。
# ---------------------------------------------------------------------
$apiHeaders = @(
    "$IL2CPP/src/il2cpp/il2cpp_api_functions.h",
    "$IL2CPP/src/il2cpp/il2cpp_api.h"
) | Where-Object { Test-Path $_ }

if ($apiHeaders.Count -gt 0) {
    $newestHdr = ($apiHeaders | ForEach-Object { (Get-Item $_).LastWriteTime } | Sort-Object -Descending)[0]
    $stale = @()
    foreach ($name in $units.Keys) {
        $obj = "$OD/$name.o"
        if (-not (Test-Path $obj)) { continue }   # 不存在的本来就待编
        if ((Get-Item $obj).LastWriteTime -lt $newestHdr) { $stale += $name }
    }
    if ($stale.Count -gt 0) {
        Write-Host "  ABI   检测到 Il2CppApi 头变动 -> 强制重编 $($stale.Count) 个单元: $($stale -join ', ')" -ForegroundColor Yellow
        foreach ($n in $stale) {
            $o = "$OD/$n.o"
            if (Test-Path $o) { Remove-Item $o -Force }
        }
    }
}

foreach ($name in $units.Keys) {
    $u = $units[$name]
    $obj = "$OD/$name.o"
    $objs += $obj
    # 增量：源文件**或任何头文件**比对象新才重编。
    # 只看 .cpp 会漏：改 il2cpp_api.h 这类共享头时，所有包含它的 TU 都得重编，
    # 但它们的 .cpp 没变 -> 增量判断会跳过 -> 结构体布局不一致 -> 运行时野指针。
    $newestDep = (Get-Item $u.src).LastWriteTime
    $hdrDir = Split-Path -Parent $u.src
    $headers = @()
    # src 里可能不带目录部分，Split-Path 会返回空串 -> Test-Path 报参数错。
    if ($hdrDir -and (Test-Path $hdrDir)) {
        $headers += Get-ChildItem -Path $hdrDir -Filter *.h -File -ErrorAction SilentlyContinue
    }
    if (Test-Path "$IL2CPP/src") {
        $headers += Get-ChildItem -Path "$IL2CPP/src" -Filter *.h -File -Recurse -ErrorAction SilentlyContinue
    }
    foreach ($h in $headers) {
        if ($h.LastWriteTime -gt $newestDep) { $newestDep = $h.LastWriteTime }
    }
    if ((Test-Path $obj) -and ((Get-Item $obj).LastWriteTime -ge $newestDep)) {
        Write-Host "  skip  $name.o" -ForegroundColor DarkGray
        continue
    }
    Write-Host "  CC    $name" -ForegroundColor Cyan
    # 2>&1 |Out-String：mingw 把 warning 也写 stderr，直接重定向会被
    # $ErrorActionPreference="Stop" 当成 NativeCommandError 抛出来，
    # 编译明明成功却被判失败。先 Out-String 归一成普通字符串再打印。
    & $u.cc @($u.fl) @INC -c $u.src -o $obj 2>&1 | Out-String | Write-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "[x] 编译失败: $name" -ForegroundColor Red; $failed = $true; break }
}
if ($failed) { exit 1 }

Write-Host "  LINK  $TARGET" -ForegroundColor Magenta
& $CXX -shared -o $TARGET @objs -static '-Wl,--gc-sections' '-Wl,--exclude-all-symbols' '-Wl,--subsystem,windows' mod_template.def @LIBS
if ($LASTEXITCODE -ne 0) { Write-Host "[x] 链接失败" -ForegroundColor Red; exit 1 }

$sz = (Get-Item $TARGET).Length
Write-Host ""
Write-Host "[OK] $TARGET  ($([math]::Round($sz/1KB,1)) KB)" -ForegroundColor Green
Write-Host "     注入: map_dll_x64.exe --dll <此DLL> --pid <PID> --exec dllmain --apc"
Write-Host "     面板: 游戏内按 INSERT 开关；日志写在 DLL 同目录 / %TEMP%"
