cmake_minimum_required(VERSION 3.24)

project(myengine VERSION 0.1.0 LANGUAGES CXX)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_CMake")
set(CMAKE_SUPPRESS_REGENERATION ON)

option(MYENGINE_COPY_RUNTIME_ASSETS "Copy assets/config to target output directory" ON)
option(MYENGINE_BUILD_TESTS "Build myengine tests" ON)
# The game for players (tools/package_game.ps1, docs/game/packaging.md): RocketArena.exe without the path to the sources,
# without the assistant and its MCP bridge, without tests; logs and settings go to %LOCALAPPDATA%\RocketArena.
# Build it in its own folder (build-shipping/) in the Release configuration: Tracy is only on in RelWithDebInfo.
option(MYENGINE_SHIPPING "Build the shipping game (RocketArena.exe)" OFF)
# Checks for C++ errors (tools/check.ps1 runs them, docs/dev/cpp-checks.md). Each one needs its own build directory:
#   MYENGINE_ASAN    AddressSanitizer (MSVC /fsanitize=address) for our targets, build-asan/
#   MYENGINE_ANALYZE MSVC /analyze for our targets, build-analyze/
option(MYENGINE_ASAN "Build our targets with AddressSanitizer (MSVC)" OFF)
option(MYENGINE_ANALYZE "Build our targets with MSVC /analyze" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MYENGINE_ASAN AND MSVC)
  # AddressSanitizer does not work with the run-time checks and with Edit and Continue
  foreach(flags_var CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
    string(REGEX REPLACE "/RTC[1csu]+" "" ${flags_var} "${${flags_var}}")
    string(REPLACE "/ZI" "/Zi" ${flags_var} "${${flags_var}}")
  endforeach()
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(CopyRuntimeAssets)
include(Wwise)
include(FaceId)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
add_subdirectory(external/freetype EXCLUDE_FROM_ALL)
if(NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype ALIAS freetype)
endif()

set(MYENGINE_DIRECTXTK_ROOT "${CMAKE_SOURCE_DIR}/external/directxtk")
set(MYENGINE_DIRECTXTK_INCLUDE_DIR "${MYENGINE_DIRECTXTK_ROOT}/Include")
set(MYENGINE_DIRECTXTK_SIMPLEMATH_HEADER "${MYENGINE_DIRECTXTK_INCLUDE_DIR}/directxtk/SimpleMath.h")
set(MYENGINE_DIRECTXTK_LIB_DEBUG   "${MYENGINE_DIRECTXTK_ROOT}/lib/x64/Debug/DirectXTK12.lib")
set(MYENGINE_DIRECTXTK_LIB_RELEASE "${MYENGINE_DIRECTXTK_ROOT}/lib/x64/Release/DirectXTK12.lib")

if(NOT EXISTS "${MYENGINE_DIRECTXTK_SIMPLEMATH_HEADER}")
  message(FATAL_ERROR "DirectXTK SimpleMath header not found: ${MYENGINE_DIRECTXTK_SIMPLEMATH_HEADER}")
endif()

add_library(myengine_directxtk INTERFACE)
target_include_directories(myengine_directxtk
  INTERFACE
    "${MYENGINE_DIRECTXTK_INCLUDE_DIR}")

if(EXISTS "${MYENGINE_DIRECTXTK_LIB_DEBUG}" AND EXISTS "${MYENGINE_DIRECTXTK_LIB_RELEASE}")
  add_library(myengine_directxtk12 STATIC IMPORTED GLOBAL)
  set_target_properties(myengine_directxtk12 PROPERTIES
    IMPORTED_LOCATION_DEBUG "${MYENGINE_DIRECTXTK_LIB_DEBUG}"
    IMPORTED_LOCATION_RELEASE "${MYENGINE_DIRECTXTK_LIB_RELEASE}"
    IMPORTED_LOCATION_RELWITHDEBINFO "${MYENGINE_DIRECTXTK_LIB_RELEASE}"
    IMPORTED_LOCATION_MINSIZEREL "${MYENGINE_DIRECTXTK_LIB_RELEASE}")
  target_link_libraries(myengine_directxtk INTERFACE myengine_directxtk12)
else()
  message(WARNING "DirectXTK12.lib (Debug/Release) not found in ${MYENGINE_DIRECTXTK_ROOT}/lib/x64")
endif()

set(MYENGINE_DIRECTXTEX_ROOT "${CMAKE_SOURCE_DIR}/external/directxtex")
set(MYENGINE_DIRECTXTEX_INCLUDE_DIR "${MYENGINE_DIRECTXTEX_ROOT}/Include")
set(MYENGINE_DIRECTXTEX_HEADER "${MYENGINE_DIRECTXTEX_INCLUDE_DIR}/directxtex/DirectXTex.h")
set(MYENGINE_DIRECTXTEX_LIB_DEBUG "${MYENGINE_DIRECTXTEX_ROOT}/lib/x64/Debug/DirectXTex.lib")
set(MYENGINE_DIRECTXTEX_LIB_RELEASE "${MYENGINE_DIRECTXTEX_ROOT}/lib/x64/Release/DirectXTex.lib")

if(NOT EXISTS "${MYENGINE_DIRECTXTEX_HEADER}")
  message(FATAL_ERROR "DirectXTex header not found: ${MYENGINE_DIRECTXTEX_HEADER}")
endif()

add_library(myengine_directxtex INTERFACE)
target_include_directories(myengine_directxtex
  INTERFACE
    "${MYENGINE_DIRECTXTEX_INCLUDE_DIR}")

if(EXISTS "${MYENGINE_DIRECTXTEX_LIB_DEBUG}" OR EXISTS "${MYENGINE_DIRECTXTEX_LIB_RELEASE}")
  add_library(myengine_directxtex_lib STATIC IMPORTED GLOBAL)
  if(EXISTS "${MYENGINE_DIRECTXTEX_LIB_DEBUG}")
    set_property(TARGET myengine_directxtex_lib PROPERTY IMPORTED_LOCATION_DEBUG "${MYENGINE_DIRECTXTEX_LIB_DEBUG}")
  endif()
  if(EXISTS "${MYENGINE_DIRECTXTEX_LIB_RELEASE}")
    set_property(TARGET myengine_directxtex_lib PROPERTY IMPORTED_LOCATION_RELEASE "${MYENGINE_DIRECTXTEX_LIB_RELEASE}")
    set_property(TARGET myengine_directxtex_lib PROPERTY IMPORTED_LOCATION_RELWITHDEBINFO "${MYENGINE_DIRECTXTEX_LIB_RELEASE}")
    set_property(TARGET myengine_directxtex_lib PROPERTY IMPORTED_LOCATION_MINSIZEREL "${MYENGINE_DIRECTXTEX_LIB_RELEASE}")
  elseif(EXISTS "${MYENGINE_DIRECTXTEX_LIB_DEBUG}")
    set_property(TARGET myengine_directxtex_lib PROPERTY IMPORTED_LOCATION_RELEASE "${MYENGINE_DIRECTXTEX_LIB_DEBUG}")
    set_property(TARGET myengine_directxtex_lib PROPERTY IMPORTED_LOCATION_RELWITHDEBINFO "${MYENGINE_DIRECTXTEX_LIB_DEBUG}")
    set_property(TARGET myengine_directxtex_lib PROPERTY IMPORTED_LOCATION_MINSIZEREL "${MYENGINE_DIRECTXTEX_LIB_DEBUG}")
    message(WARNING "DirectXTex release library not found. Using Debug library for all configurations.")
  endif()
  if(EXISTS "${MYENGINE_DIRECTXTEX_LIB_DEBUG}")
    set_property(TARGET myengine_directxtex_lib PROPERTY IMPORTED_LOCATION "${MYENGINE_DIRECTXTEX_LIB_DEBUG}")
  elseif(EXISTS "${MYENGINE_DIRECTXTEX_LIB_RELEASE}")
    set_property(TARGET myengine_directxtex_lib PROPERTY IMPORTED_LOCATION "${MYENGINE_DIRECTXTEX_LIB_RELEASE}")
  endif()
  target_link_libraries(myengine_directxtex INTERFACE myengine_directxtex_lib)
else()
  message(WARNING "DirectXTex static library not found. Texture loading through DirectXTex will fail at link time.")
endif()

set(MYENGINE_STB_ROOT "${CMAKE_SOURCE_DIR}/external/stb")
set(MYENGINE_STB_INCLUDE_DIR "${MYENGINE_STB_ROOT}/Include/stb")
set(MYENGINE_STB_HEADER "${MYENGINE_STB_INCLUDE_DIR}/stb_image.h")
if(NOT EXISTS "${MYENGINE_STB_HEADER}")
  message(FATAL_ERROR "stb_image.h not found: ${MYENGINE_STB_HEADER}")
endif()
add_library(myengine_stb INTERFACE)
target_include_directories(myengine_stb
  INTERFACE
    "${MYENGINE_STB_INCLUDE_DIR}")

set(MYENGINE_NLOHMANN_JSON_ROOT "${CMAKE_SOURCE_DIR}/external/nlohmann/Include")
set(MYENGINE_NLOHMANN_JSON_HEADER "${MYENGINE_NLOHMANN_JSON_ROOT}/nlohmann/json.hpp")
if(NOT EXISTS "${MYENGINE_NLOHMANN_JSON_HEADER}")
  message(FATAL_ERROR "nlohmann/json.hpp not found: ${MYENGINE_NLOHMANN_JSON_HEADER}")
endif()
add_library(myengine_nlohmann_json INTERFACE)
target_include_directories(myengine_nlohmann_json
  INTERFACE
    "${MYENGINE_NLOHMANN_JSON_ROOT}")

set(MYENGINE_DIRECTX_HEADERS_ROOT "${CMAKE_SOURCE_DIR}/external/directx-headers/Include")
set(MYENGINE_D3DX12_HEADER "${MYENGINE_DIRECTX_HEADERS_ROOT}/directx-headers/directx/d3dx12.h")
if(NOT EXISTS "${MYENGINE_D3DX12_HEADER}")
  message(FATAL_ERROR "d3dx12.h not found: ${MYENGINE_D3DX12_HEADER}")
endif()
add_library(myengine_directxheaders INTERFACE)
target_include_directories(myengine_directxheaders
  INTERFACE
    "${MYENGINE_DIRECTX_HEADERS_ROOT}")

# Python 3.14 + pybind11 for scripting (LR2). Everything is vendored: the installed Python is not used.
# Runtime (python314.dll + python314.zip) lives in external/runtime and is copied next to the exe.
set(MYENGINE_PYTHON_ROOT "${CMAKE_SOURCE_DIR}/external/python")
set(MYENGINE_PYTHON_HEADER "${MYENGINE_PYTHON_ROOT}/include/Python.h")
set(MYENGINE_PYTHON_LIB "${MYENGINE_PYTHON_ROOT}/libs/python314.lib")
set(MYENGINE_PYBIND11_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/pybind11/include")
if(NOT EXISTS "${MYENGINE_PYTHON_HEADER}")
  message(FATAL_ERROR "Python.h not found: ${MYENGINE_PYTHON_HEADER}")
endif()
if(NOT EXISTS "${MYENGINE_PYTHON_LIB}")
  message(FATAL_ERROR "python314.lib not found: ${MYENGINE_PYTHON_LIB}")
endif()
if(NOT EXISTS "${MYENGINE_PYBIND11_INCLUDE_DIR}/pybind11/embed.h")
  message(FATAL_ERROR "pybind11 not found: ${MYENGINE_PYBIND11_INCLUDE_DIR}")
endif()
add_library(myengine_python_lib UNKNOWN IMPORTED GLOBAL)
set_target_properties(myengine_python_lib PROPERTIES IMPORTED_LOCATION "${MYENGINE_PYTHON_LIB}")
add_library(myengine_python INTERFACE)
# SYSTEM: /W4 does not apply to third-party headers
target_include_directories(myengine_python SYSTEM
  INTERFACE
    "${MYENGINE_PYTHON_ROOT}/include"
    "${MYENGINE_PYBIND11_INCLUDE_DIR}")
target_link_libraries(myengine_python INTERFACE myengine_python_lib)

add_subdirectory(external/tracy EXCLUDE_FROM_ALL)
target_compile_definitions(TracyClient PUBLIC $<$<CONFIG:RelWithDebInfo>:TRACY_ENABLE> $<$<CONFIG:RelWithDebInfo>:TRACY_FIBERS> $<$<CONFIG:RelWithDebInfo>:TRACY_ON_DEMAND>)
# Jolt Physics (v5.6.0, submodule) for the Rocket League game (game/rl). Deterministic float mode so that a rollback replay
# gives the same state; AVX2 can be turned off for machines without it.
option(MYENGINE_JOLT_AVX2 "Compile Jolt with AVX2" ON)
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(USE_AVX2 ${MYENGINE_JOLT_AVX2} CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
foreach(opt JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(external/JoltPhysics/Build EXCLUDE_FROM_ALL)
# SYSTEM: /W4 does not apply to Jolt headers
get_target_property(MYENGINE_JOLT_INCLUDES Jolt INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(Jolt PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${MYENGINE_JOLT_INCLUDES}")
set_property(TARGET Jolt PROPERTY FOLDER "External")

# The version of the game and of the exe resources comes from project(... VERSION) above and nowhere else:
# generated/myengine/core/Version.h (the menu, the crash report) and app/resources/RocketArena.rc.in (VERSIONINFO of the exe)
set(MYENGINE_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE MYENGINE_GIT_COMMIT_OUT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE MYENGINE_GIT_RESULT)
  if(MYENGINE_GIT_RESULT EQUAL 0 AND MYENGINE_GIT_COMMIT_OUT)
    set(MYENGINE_GIT_COMMIT "${MYENGINE_GIT_COMMIT_OUT}")
  endif()
endif()
configure_file(cmake/Version.h.in "${CMAKE_BINARY_DIR}/generated/myengine/core/Version.h" @ONLY)

add_subdirectory(engine)
add_subdirectory(game/rl)
add_subdirectory(game/doom)
add_subdirectory(app)

if(MYENGINE_BUILD_TESTS AND NOT MYENGINE_SHIPPING)
  enable_testing()
  add_subdirectory(tests)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT myengine)
