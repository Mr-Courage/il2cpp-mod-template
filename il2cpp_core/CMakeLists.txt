cmake_minimum_required(VERSION 3.16)
project(il2cpp_core LANGUAGES C CXX)

# ---------------------------------------------------------------- core lib
add_library(il2cpp_core STATIC
    src/il2cpp/il2cpp_api.cpp
    src/il2cpp/il2cpp_runtime.cpp
    src/hook/il2cpp_hook.cpp
)
target_include_directories(il2cpp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(il2cpp_core PUBLIC cxx_std_17)

if(WIN32)
    target_sources(il2cpp_core PRIVATE src/platform/il2cpp_module_windows.c)
else()
    target_sources(il2cpp_core PRIVATE src/platform/il2cpp_module_posix.c)
    find_package(Threads REQUIRED)
    target_link_libraries(il2cpp_core PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# ---------------------------------------------------------------- selftest
option(IL2CPP_BUILD_TESTS "Build il2cpp_selftest" ON)
if(IL2CPP_BUILD_TESTS)
    add_executable(il2cpp_selftest tools/selftest.cpp)
    target_link_libraries(il2cpp_selftest PRIVATE il2cpp_core)
endif()