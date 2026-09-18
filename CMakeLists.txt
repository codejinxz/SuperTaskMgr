cmake_minimum_required(VERSION 3.20)
project(SuperTaskMgr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "MSVC toolset required (VS2022 Build Tools).")
endif()
# Static CRT for a portable single-exe deployment (arch section 1).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
# /W4 everywhere; third_party isolated at /W0 below. /WX is deliberately not set during
# development; the phase-4 gate enforces zero warnings via build log inspection.
add_compile_options(/W4 /permissive- /utf-8 /MP)

# ---------------- stm_core: contracts + platform-neutral utilities ----------------
add_library(stm_core STATIC
    src/core/Log.cpp
    src/core/Str.cpp
    src/core/Err.cpp
    src/core/Cfg.cpp
    src/core/Jobs.cpp
    src/core/Notifications.cpp
    src/core/FsUtil.cpp
    src/core/Privilege.cpp
    src/core/ProtectedList.cpp)
target_include_directories(stm_core PUBLIC src)
target_link_libraries(stm_core PUBLIC shell32 ole32 advapi32)

# ---------------- stm_collect: snapshot collection thread ----------------
file(GLOB STM_COLLECT_SRC CONFIGURE_DEPENDS src/collect/*.cpp)
add_library(stm_collect STATIC ${STM_COLLECT_SRC})
target_link_libraries(stm_collect PUBLIC stm_core pdh iphlpapi dxgi)

# ---------------- stm_ops: destructive ops, elevation, signature ----------------
file(GLOB STM_OPS_SRC CONFIGURE_DEPENDS src/ops/*.cpp)
add_library(stm_ops STATIC ${STM_OPS_SRC})
target_link_libraries(stm_ops PUBLIC stm_core wintrust taskschd wevtapi)

# ---------------- vendored third_party (W0 isolation) ----------------
file(GLOB IMGUI_SRC third_party/imgui/*.cpp)
list(FILTER IMGUI_SRC EXCLUDE REGEX "imgui_demo")
add_library(stm_imgui STATIC
    ${IMGUI_SRC}
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(stm_imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_compile_options(stm_imgui PRIVATE /W0)
target_compile_definitions(stm_imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)

add_library(stm_implot STATIC
    third_party/implot/implot.cpp
    third_party/implot/implot_items.cpp)
target_include_directories(stm_implot PUBLIC third_party/implot)
target_link_libraries(stm_implot PUBLIC stm_imgui)
target_compile_options(stm_implot PRIVATE /W0)

# ---------------- app ----------------
file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS src/app/*.cpp)
add_executable(SuperTaskMgr WIN32 ${APP_SRC} src/app/app.manifest)
target_include_directories(SuperTaskMgr PRIVATE third_party/stb)
target_link_libraries(SuperTaskMgr PRIVATE stm_collect stm_ops stm_imgui stm_implot
    d3d11 dxgi d3dcompiler dwmapi comdlg32)

# ---------------- selftest ----------------
file(GLOB SELFTEST_SRC CONFIGURE_DEPENDS src/selftest/*.cpp)
add_executable(stm_selftest ${SELFTEST_SRC})
target_link_libraries(stm_selftest PRIVATE stm_core stm_collect stm_ops pdh iphlpapi)
