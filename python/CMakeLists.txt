# The Python module (JITNODES_BUILD_PYTHON). Needs, as siblings of this checkout by default:
#   pybind11 (the full source tree) and a Python 3.13 x64 SDK (include/ and libs/ - e.g. python-build-standalone),
#   plus newui's scriptgen tool (JITNODES_NEWUI_DIR) which generates the bindings from the // @scriptable marks.
#
#   jitnodesPython   static library: the generated bindings and the handles; link it into an application that embeds Python
#                    (see pyHandles.h: registerModule, wrapNetwork)
#   pyjitnodes       the standalone module, built as jitnodes.pyd, for `import jitnodes` from any Python
#   jitnodesPythonEmbeddedTests   an executable that embeds Python and checks that a handle to a destroyed network goes stale

set(JITNODES_PYBIND11_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../pybind11" CACHE PATH "pybind11 source tree")
set(JITNODES_PYTHON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../python" CACHE PATH "Python 3.13 x64 (python.exe, include/, libs/)")

# Without these pybind11 picks whatever Python it finds first (the system one) instead of the bundled one.
set(PYBIND11_FINDPYTHON ON)
set(Python_ROOT_DIR ${JITNODES_PYTHON_DIR})
set(Python_FIND_STRATEGY LOCATION)
set(Python_FIND_REGISTRY NEVER)
set(Python_FIND_VIRTUALENV ONLY)
add_subdirectory(${JITNODES_PYBIND11_DIR} ${CMAKE_CURRENT_BINARY_DIR}/pybind11)

include(${JITNODES_NEWUI_DIR}/cmake/ScriptGen.cmake)

add_library(jitnodesPython STATIC pyHandles.cpp pyArrays.cpp pyRuntime.cpp pyGuard.cpp pyAssets.cpp pyAssetCheck.cpp)
target_link_libraries(jitnodesPython PUBLIC jitnodes::jitnodes pybind11::pybind11)
target_include_directories(jitnodesPython PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  # /bigobj: the generated file is one function per class with a long call chain; instrumented builds (ASAN) exceed the default section limit.
  target_compile_options(jitnodesPython PRIVATE /W4 /permissive- /utf-8 /bigobj)
endif()

newui_add_scriptgen_output(jitnodesPython
  SCAN_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include/jitnodes
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include ${JITNODES_JSON5_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/asmjit/src
  CXX_STANDARD 20
  REGISTER_FUNCTION registerJitnodesScripting
  STUBS ${CMAKE_CURRENT_BINARY_DIR}/jitnodes/_core.pyi
  STRICT)

# jitnodes/__init__.pyi: the stubs of the handles, derived from the generated jitnodes/_core.pyi.
add_custom_command(TARGET jitnodesPython POST_BUILD
  COMMAND ${Python_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/genHandleStubs.py
          ${CMAKE_CURRENT_BINARY_DIR}/jitnodes/_core.pyi ${CMAKE_CURRENT_BINARY_DIR}/jitnodes/__init__.pyi
  COMMENT "Generating jitnodes/__init__.pyi")

pybind11_add_module(pyjitnodes MODULE NO_EXTRAS pyModule.cpp)
target_link_libraries(pyjitnodes PRIVATE jitnodesPython)
set_target_properties(pyjitnodes PROPERTIES OUTPUT_NAME jitnodes)

if(JITNODES_BUILD_TESTS)
  add_executable(jitnodesPythonEmbeddedTests tests/embeddedTests.cpp)
  target_link_libraries(jitnodesPythonEmbeddedTests PRIVATE jitnodesPython pybind11::embed)

  # The runtime is given the bundled Python's folder; its DLL must be found next to the test or on PATH.
  get_filename_component(python_home ${Python_EXECUTABLE} DIRECTORY)
  add_test(NAME jitnodesPythonEmbedded COMMAND jitnodesPythonEmbeddedTests ${python_home})
  set_tests_properties(jitnodesPythonEmbedded PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${python_home}")

  add_test(NAME jitnodesPythonScripts
    COMMAND ${Python_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/runScripts.py $<TARGET_FILE_DIR:pyjitnodes>)
endif()
