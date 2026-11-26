# psprecomp — a static-recompilation toolkit for PlayStation Portable games.
#
# This file builds two things:
#   1. the `psprecomp` runtime static library — what recompiled game code links
#      against (CPU state, memory map, semantic helpers)
#   2. `allegrexrecomp` — the tool that turns a PSP module into C
#
# A game repo consumes this repo as a git submodule and does:
#     add_subdirectory(psprecomp)
#     add_subdirectory(games/<game>)
# and links its host against the `psprecomp` target.
#
# The core has no external dependencies on purpose. SDL2 and friends only ever
# appear behind an opt-in option, so a fresh clone builds with nothing installed.

cmake_minimum_required(VERSION 3.16)
project(psprecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
    # /MP: the generated corpus is hundreds of translation units, and building
    # them one at a time is the slowest part of the loop. No /bigobj -- the
    # emitter splits its output, so no single unit is large enough to need it.
    add_compile_options(/MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# --- the runtime library ------------------------------------------------------

add_library(psprecomp STATIC
    src/cpu.c
    src/mem.c
    src/dispatch.c
    src/ctors.c
    src/render.c
    src/audio_host.c
    src/render_gl.c
    src/vfpu.c
    src/traps.c
    src/window.c
    src/mods.c
    src/font.c
    src/movie.c
    src/envcache.c
    src/prof.c
    src/module.c
    src/hle/hle.c
    src/hle/sysmem.c
    src/hle/threadman.c
    src/hle/power.c
    src/hle/intrman.c
    src/hle/display.c
    src/hle/ge.c
    src/hle/sascore.c
    src/hle/iofilemgr.c
    src/hle/misc.c
    src/hle/savedata.c
    src/hle/atrac.c
)

target_include_directories(psprecomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Runtime VFPU helpers use libm on Unix; propagate it to static-library users.
if(UNIX)
    target_link_libraries(psprecomp PUBLIC m)
endif()

# Real scheduling is experimental; the existing inline model stays the default.
option(PSPRECOMP_REAL_THREADS "Enable experimental cooperative host-thread scheduling" OFF)
if(DEFINED ENV{PSPRECOMP_FNAN_CHECK})
    target_compile_definitions(psprecomp PUBLIC PSPRECOMP_FNAN_CHECK)
endif()
target_compile_definitions(psprecomp PUBLIC
    PSPRECOMP_REAL_THREADS=$<BOOL:${PSPRECOMP_REAL_THREADS}>
)
if(WIN32)
    target_link_libraries(psprecomp PUBLIC xinput9_1_0 winmm windowscodecs ole32 shell32)   # controller input, audio out, mod PNGs
endif()
if(PSPRECOMP_REAL_THREADS AND WIN32)
    target_link_libraries(psprecomp PUBLIC winmm opengl32)   # timeBeginPeriod
endif()
if(PSPRECOMP_REAL_THREADS AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(psprecomp PUBLIC Threads::Threads)
endif()

# --- vendored ATRAC3/ATRAC3+ decoder (FFmpeg via PPSSPP, LGPL 2.1) ------------

set(AT3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/at3_standalone)
add_library(at3_standalone STATIC
    ${AT3_DIR}/atrac.cpp ${AT3_DIR}/atrac3.cpp ${AT3_DIR}/atrac3plus.cpp
    ${AT3_DIR}/atrac3plusdec.cpp ${AT3_DIR}/atrac3plusdsp.cpp ${AT3_DIR}/get_bits.cpp
    ${AT3_DIR}/compat.cpp ${AT3_DIR}/fft.cpp ${AT3_DIR}/mem.cpp ${AT3_DIR}/at3_c.cpp)
set_target_properties(at3_standalone PROPERTIES CXX_STANDARD 17)
target_include_directories(at3_standalone PRIVATE ${AT3_DIR}/shim PUBLIC ${AT3_DIR})
target_link_libraries(psprecomp PUBLIC at3_standalone)

# --- vendored Dear ImGui (MIT): the PC menu and overlays (src/ui.cpp) ----------

if(WIN32)
    set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
    add_library(imgui STATIC ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
    target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)   # ui.cpp feeds the pad itself
    target_sources(psprecomp PRIVATE src/ui.cpp)
    target_link_libraries(psprecomp PUBLIC imgui)
endif()

# --- the recompiler -----------------------------------------------------------

add_subdirectory(tools/allegrexrecomp)

# --- tests (only when psprecomp is the top-level project, not a submodule) -----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()

