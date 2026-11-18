# swbes-recomp -- Star Wars Battlefront: Elite Squadron (ULES-01214), statically recompiled.
cmake_minimum_required(VERSION 3.16)
project(swbes_recomp C CXX)
# The release version: VERSION is the one place to bump it (scripts/package.ps1
# names the zip from it, the game shows it in the F1 menu).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SWBES_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)

set(PSPRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/psprecomp" CACHE PATH "psprecomp source tree")
set(PSPRECOMP_REAL_THREADS ON CACHE BOOL "game needs real guest threads" FORCE)
# Separate GPLv3 helper that decrypts the player's EBOOT on first start; added
# before the global flags below so it builds as it would on its own.
add_subdirectory(tools/ebootdec)
if(MSVC)
  # Speed: every getenv() goes through a cache (envcache.h, force-included);
  # no stack cookies; no incremental-link thunks on every call.
  add_compile_options(/FI${PSPRECOMP_DIR}/include/psprecomp/envcache.h /GS-)
  add_link_options(/INCREMENTAL:NO /PDBALTPATH:%_PDB%)   # exe names its .pdb without the build machine's path
  # Profile-guided optimisation (scripts in scripts/pgo.ps1): GEN builds an
  # instrumented exe that records a training run; USE rebuilds with it.
  set(SWBES_PGO "" CACHE STRING "MSVC profile-guided optimisation: GEN, USE or empty")
  if(SWBES_PGO STREQUAL "GEN")
    add_compile_options(/GL)
    add_link_options(/LTCG /GENPROFILE)
  elseif(SWBES_PGO STREQUAL "USE")
    add_compile_options(/GL)
    add_link_options(/LTCG /USEPROFILE)
  endif()
endif()
add_subdirectory(${PSPRECOMP_DIR} psprecomp)

set(SWBES_GEN "${PROJECT_BINARY_DIR}/gen" CACHE PATH "Generated C (scripts/build.ps1)")
option(SWBES_TRACE "Record function entries for crash reports" ON)

file(GLOB srcs CONFIGURE_DEPENDS "${SWBES_GEN}/swbes_*.c")
if(srcs)
  add_executable(swbes games/swbes/host.c games/swbes/gamedata.c ${srcs})
  if(SWBES_TRACE)
    target_compile_definitions(swbes PRIVATE PSPRECOMP_TRACE)
    target_compile_definitions(psprecomp PUBLIC PSPRECOMP_TRACE)
  endif()
  target_link_libraries(swbes PRIVATE psprecomp)
  target_compile_definitions(swbes PRIVATE SWBES_VERSION="${SWBES_VERSION}")
  add_dependencies(swbes ebootdec)
  if(MSVC)
    target_compile_options(swbes PRIVATE /W0 /O2 /Oi /Ob2 /bigobj)
    # A windowed app (no console window); main() stays the entry point and
    # output goes to swbes.log unless the caller redirected it.
    target_link_options(swbes PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
  endif()
  # Icon and the version shown in the exe's Properties.
  string(REPLACE "." "," SWBES_VERSION_COMMAS "${SWBES_VERSION}")
  # Drop an icon at games/swbes/swbes.ico to give the exe and window one.
  set(SWBES_ICON_LINE "")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/swbes/swbes.ico")
    set(SWBES_ICON_LINE "1 ICON \"swbes.ico\"")
  endif()
  configure_file(games/swbes/swbes.rc.in "${PROJECT_BINARY_DIR}/swbes.rc" @ONLY)
  target_sources(swbes PRIVATE "${PROJECT_BINARY_DIR}/swbes.rc")
  target_include_directories(swbes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/games/swbes")
  # Movies (psprecomp/src/movie.c) decode video with ffmpeg; ship it beside
  # the exe. SWBES_FFMPEG picks a specific one, else the one on PATH.
  # Default: the minimal LGPL H.264-only build (scripts/build_ffmpeg.sh).
  find_program(SWBES_FFMPEG ffmpeg HINTS "${PROJECT_SOURCE_DIR}/third_party/ffmpeg-min/bin" NO_DEFAULT_PATH)
  find_program(SWBES_FFMPEG ffmpeg DOC "ffmpeg.exe copied next to swbes.exe for movie playback")
  if(SWBES_FFMPEG)
    add_custom_command(TARGET swbes POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SWBES_FFMPEG}" "$<TARGET_FILE_DIR:swbes>/ffmpeg.exe")
  else()
    message(WARNING "ffmpeg not found: movies will be skipped (set SWBES_FFMPEG)")
  endif()
endif()
