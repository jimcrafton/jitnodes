cmake_minimum_required(VERSION 3.24)
project(jitnodes VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(JITNODES_BUILD_TESTS    "Build jitnodes unit tests"  ON)
option(JITNODES_BUILD_EXAMPLES "Build jitnodes examples"    ON)
option(JITNODES_BUILD_TOOLS    "Build the jitnodesCli tool"  ON)
option(JITNODES_SANITIZE       "Build the library and tests with AddressSanitizer (and UBSan on clang/gcc)" OFF)
option(JITNODES_BUILD_EDITOR   "Build the newui based editor (Windows, builds newui as an external project)" OFF)
set(JITNODES_NEWUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../newui" CACHE PATH "newui source checkout used by the editor")

# json5 (P-i-N/json5, MIT, header-only) is used for graph files; vendored under 3rdparty.
set(JITNODES_JSON5_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json5/include" CACHE PATH "Directory containing json5/json5.hpp")

# asmjit is built as a static library from the vendored copy.
set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
set(ASMJIT_NO_NATVIS ON CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/asmjit EXCLUDE_FROM_ALL)

if(JITNODES_SANITIZE)
  if(MSVC)
    add_compile_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

add_library(jitnodes STATIC
  src/core/arena.cpp
  src/core/threadPool.cpp
  src/core/stackGuard.cpp
  src/geo/geometry.cpp
  src/geo/topology.cpp
  src/geo/svgExport.cpp
  src/jit/expr.cpp
  src/jit/exprParser.cpp
  src/jit/fuse.cpp
  src/jit/kernel.cpp
  src/jit/vecEmitter.cpp
  src/jit/scalarEval.cpp
  src/graph/animation.cpp
  src/graph/node.cpp
  src/graph/network.cpp
  src/graph/builtinNodes.cpp
  src/graph/topologyNodes.cpp
  src/graph/subnet.cpp
  src/graph/serialize.cpp
  src/graph/history.cpp
)
add_library(jitnodes::jitnodes ALIAS jitnodes)

target_include_directories(jitnodes PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(jitnodes PUBLIC asmjit::asmjit)
target_include_directories(jitnodes SYSTEM PRIVATE ${JITNODES_JSON5_DIR})

if(MSVC)
  target_compile_options(jitnodes PRIVATE /W4 /permissive- /utf-8)
  target_compile_definitions(jitnodes PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(jitnodes PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(JITNODES_BUILD_TESTS)
  enable_testing()
  add_executable(jitnodesTests tests/testMain.cpp)
  target_link_libraries(jitnodesTests PRIVATE jitnodes::jitnodes)
  target_compile_definitions(jitnodesTests PRIVATE JITNODES_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME jitnodesTests COMMAND jitnodesTests)

  # Thousands of nodes and tens of thousands of points; ctest runs the quick size, run jitnodesStress itself for the full one.
  add_executable(jitnodesStress tests/stressTest.cpp)
  target_link_libraries(jitnodesStress PRIVATE jitnodes::jitnodes)
  if(MSVC)
    target_compile_options(jitnodesStress PRIVATE /W4 /permissive- /utf-8)
  endif()
  add_test(NAME jitnodesStressQuick COMMAND jitnodesStress --quick)
endif()

if(JITNODES_BUILD_EXAMPLES)
  add_executable(basicGraph examples/basicGraph.cpp)
  target_link_libraries(basicGraph PRIVATE jitnodes::jitnodes)
  add_executable(kernelBench examples/kernelBench.cpp)
  target_link_libraries(kernelBench PRIVATE jitnodes::jitnodes)
endif()

if(JITNODES_BUILD_TOOLS)
  add_executable(jitnodesCli tools/jitnodesCli.cpp)
  target_link_libraries(jitnodesCli PRIVATE jitnodes::jitnodes)
endif()

if(JITNODES_BUILD_EDITOR)
  add_subdirectory(editor)
endif()
