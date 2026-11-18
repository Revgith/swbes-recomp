# ebootdec: separate GPLv3 helper that decrypts the player's EBOOT. Builds on
# its own too: cmake -S . -B build && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(ebootdec C CXX)
file(GLOB ebootdec_srcs ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/libkirk/*.c)
add_executable(ebootdec ${ebootdec_srcs})
set_target_properties(ebootdec PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(MSVC)
  target_compile_options(ebootdec PRIVATE /W0)
  target_link_options(ebootdec PRIVATE /PDBALTPATH:%_PDB%)
  set_property(TARGET ebootdec PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
