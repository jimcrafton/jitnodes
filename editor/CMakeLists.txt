if(NOT WIN32)
  message(FATAL_ERROR "JITNODES_BUILD_EDITOR needs Windows: newui is a Win32 UI framework")
endif()
if(CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR "JITNODES_BUILD_EDITOR supports single-config generators only (use the Ninja presets)")
endif()

get_filename_component(newuiDir "${JITNODES_NEWUI_DIR}" ABSOLUTE)
if(NOT EXISTS "${newuiDir}/CMakeLists.txt")
  message(FATAL_ERROR "JITNODES_NEWUI_DIR='${newuiDir}' is not a newui checkout")
endif()
if(NOT EXISTS "${newuiDir}/tools/reflectgen/.venv/Scripts/python.exe")
  message(FATAL_ERROR "newui's reflectgen venv is missing: follow 'Setup' in ${newuiDir}/tools/reflectgen/README.md")
endif()

# newui is built in its own project so its asmjit (embedded in Blend2D under the abi_bl namespace), C++17 settings,
# tests and examples stay out of this one. Building it bumps VERSION.state in the newui checkout, as any newui build does.
include(ExternalProject)
set(newuiBinary "${CMAKE_BINARY_DIR}/newui")
set(newuiLib "${newuiBinary}/newui.lib")
set(newuiAutomationLib "${newuiBinary}/newui_automation.lib")
set(blend2dLib "${newuiBinary}/3rdparty/blend2d/blend2d.lib")

set(newuiArgs -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
if(CMAKE_C_COMPILER)
  list(APPEND newuiArgs -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
endif()
if(CMAKE_MAKE_PROGRAM)
  list(APPEND newuiArgs -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
endif()

ExternalProject_Add(newuiExternal
  SOURCE_DIR "${newuiDir}"
  BINARY_DIR "${newuiBinary}"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${newuiArgs}
  BUILD_COMMAND ${CMAKE_COMMAND} --build "${newuiBinary}" --target newui newui_automation
  INSTALL_COMMAND ""
  BUILD_ALWAYS ON
  BUILD_BYPRODUCTS "${newuiLib}" "${newuiAutomationLib}" "${blend2dLib}"
  USES_TERMINAL_BUILD ON)

file(MAKE_DIRECTORY "${newuiBinary}/include")
add_library(newui::newui STATIC IMPORTED GLOBAL)
set_target_properties(newui::newui PROPERTIES
  IMPORTED_LOCATION "${newuiLib}"
  INTERFACE_INCLUDE_DIRECTORIES
    "${newuiDir}/include;${newuiBinary}/include;${newuiDir}/3rdparty/json5/include;${newuiDir}/3rdparty/wil/include;${newuiDir}/3rdparty/blend2d"
  INTERFACE_COMPILE_DEFINITIONS "BL_STATIC"
  INTERFACE_LINK_LIBRARIES
    "${blend2dLib};Comctl32.lib;Uxtheme.lib;Ole32.lib;Uuid.lib;Shell32.lib;Advapi32.lib;Dwmapi.lib;Shlwapi.lib;d3d11.lib;dxgi.lib;dxguid.lib")
add_dependencies(newui::newui newuiExternal)

add_library(newui::automation STATIC IMPORTED GLOBAL)
set_target_properties(newui::automation PROPERTIES
  IMPORTED_LOCATION "${newuiAutomationLib}"
  INTERFACE_LINK_LIBRARIES newui::newui)

add_library(editorCore STATIC editorModel.cpp graphLayout.cpp graphController.cpp viewportController.cpp profileController.cpp keyframeEditor.cpp)
target_link_libraries(editorCore PUBLIC jitnodes::jitnodes)
target_include_directories(editorCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  target_compile_options(editorCore PRIVATE /W4 /permissive- /utf-8)
endif()

if(JITNODES_BUILD_TESTS)
  add_executable(editorModelTests editorModelTests.cpp)
  target_link_libraries(editorModelTests PRIVATE editorCore)
  add_test(NAME editorModelTests COMMAND editorModelTests)
endif()

add_executable(jitnodesEditor WIN32 main.cpp editorWindow.cpp graphCanvas.cpp parameterPanel.cpp viewportCanvas.cpp profileCanvas.cpp keyframeDialog.cpp)
target_link_libraries(jitnodesEditor PRIVATE editorCore newui::newui)
option(JITNODES_GUI_AUTOMATION "Build the editor with newui's automation server (--automation <pipe>)" ON)
if(JITNODES_GUI_AUTOMATION)
  target_link_libraries(jitnodesEditor PRIVATE newui::automation)
  target_compile_definitions(jitnodesEditor PRIVATE JITNODES_GUI_AUTOMATION)
endif()
if(MSVC)
  target_compile_definitions(jitnodesEditor PRIVATE _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING)
  target_compile_options(jitnodesEditor PRIVATE /W3 /permissive /utf-8 /Zc:__cplusplus)
  target_link_options(jitnodesEditor PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

add_custom_command(TARGET jitnodesEditor POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:jitnodesEditor>/Resources"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/mainWindow.newui"
    "$<TARGET_FILE_DIR:jitnodesEditor>/Resources/mainWindow.newui"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/keyframeDialog.newui"
    "$<TARGET_FILE_DIR:jitnodesEditor>/Resources/keyframeDialog.newui"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/../examples/assets"
    "$<TARGET_FILE_DIR:jitnodesEditor>/Assets")
