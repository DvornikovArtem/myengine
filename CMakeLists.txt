cmake_minimum_required(VERSION 3.24)

project(myengine VERSION 0.1.0 LANGUAGES CXX)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "_CMake")
set(CMAKE_SUPPRESS_REGENERATION ON)

option(MYENGINE_COPY_RUNTIME_ASSETS "Copy assets/config to target output directory" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(CopyRuntimeAssets)

set(MYENGINE_DIRECTXTK_ROOT "${CMAKE_SOURCE_DIR}/external/directxtk")
set(MYENGINE_DIRECTXTK_INCLUDE_DIR "${MYENGINE_DIRECTXTK_ROOT}/Include")
set(MYENGINE_DIRECTXTK_SIMPLEMATH_HEADER "${MYENGINE_DIRECTXTK_INCLUDE_DIR}/directxtk/SimpleMath.h")
set(MYENGINE_DIRECTXTK_LIB_PATH "${MYENGINE_DIRECTXTK_ROOT}/lib/x64/DirectXTK12.lib")

if(NOT EXISTS "${MYENGINE_DIRECTXTK_SIMPLEMATH_HEADER}")
  message(FATAL_ERROR "DirectXTK SimpleMath header not found: ${MYENGINE_DIRECTXTK_SIMPLEMATH_HEADER}")
endif()

add_library(myengine_directxtk INTERFACE)
target_include_directories(myengine_directxtk
  INTERFACE
    "${MYENGINE_DIRECTXTK_INCLUDE_DIR}")

if(EXISTS "${MYENGINE_DIRECTXTK_LIB_PATH}")
  add_library(myengine_directxtk12 STATIC IMPORTED GLOBAL)
  set_target_properties(myengine_directxtk12 PROPERTIES
    IMPORTED_LOCATION "${MYENGINE_DIRECTXTK_LIB_PATH}")
  target_link_libraries(myengine_directxtk INTERFACE myengine_directxtk12)
else()
  message(WARNING "DirectXTK12.lib not found at ${MYENGINE_DIRECTXTK_LIB_PATH}. Header-only parts will still be available.")
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

set(MYENGINE_ASSIMP_ROOT "${CMAKE_SOURCE_DIR}/external/assimp")
set(MYENGINE_ASSIMP_INCLUDE_DIR "${MYENGINE_ASSIMP_ROOT}/Include")
set(MYENGINE_ASSIMP_HEADER "${MYENGINE_ASSIMP_INCLUDE_DIR}/assimp/Importer.hpp")
set(MYENGINE_ASSIMP_LIB_DEBUG "${MYENGINE_ASSIMP_ROOT}/lib/Debug/assimp-vc143-mtd.lib")
set(MYENGINE_ASSIMP_LIB_RELEASE "${MYENGINE_ASSIMP_ROOT}/lib/Release/assimp-vc143-mt.lib")

if(NOT EXISTS "${MYENGINE_ASSIMP_HEADER}")
  message(FATAL_ERROR "Assimp header not found: ${MYENGINE_ASSIMP_HEADER}")
endif()

add_library(myengine_assimp INTERFACE)
target_include_directories(myengine_assimp
  INTERFACE
    "${MYENGINE_ASSIMP_INCLUDE_DIR}")

if(EXISTS "${MYENGINE_ASSIMP_LIB_DEBUG}" OR EXISTS "${MYENGINE_ASSIMP_LIB_RELEASE}")
  add_library(myengine_assimp_lib UNKNOWN IMPORTED GLOBAL)
  if(EXISTS "${MYENGINE_ASSIMP_LIB_DEBUG}")
    set_property(TARGET myengine_assimp_lib PROPERTY IMPORTED_LOCATION_DEBUG "${MYENGINE_ASSIMP_LIB_DEBUG}")
  endif()
  if(EXISTS "${MYENGINE_ASSIMP_LIB_RELEASE}")
    set_property(TARGET myengine_assimp_lib PROPERTY IMPORTED_LOCATION_RELEASE "${MYENGINE_ASSIMP_LIB_RELEASE}")
    set_property(TARGET myengine_assimp_lib PROPERTY IMPORTED_LOCATION_RELWITHDEBINFO "${MYENGINE_ASSIMP_LIB_RELEASE}")
    set_property(TARGET myengine_assimp_lib PROPERTY IMPORTED_LOCATION_MINSIZEREL "${MYENGINE_ASSIMP_LIB_RELEASE}")
  elseif(EXISTS "${MYENGINE_ASSIMP_LIB_DEBUG}")
    set_property(TARGET myengine_assimp_lib PROPERTY IMPORTED_LOCATION_RELEASE "${MYENGINE_ASSIMP_LIB_DEBUG}")
    set_property(TARGET myengine_assimp_lib PROPERTY IMPORTED_LOCATION_RELWITHDEBINFO "${MYENGINE_ASSIMP_LIB_DEBUG}")
    set_property(TARGET myengine_assimp_lib PROPERTY IMPORTED_LOCATION_MINSIZEREL "${MYENGINE_ASSIMP_LIB_DEBUG}")
  endif()
  if(EXISTS "${MYENGINE_ASSIMP_LIB_DEBUG}")
    set_property(TARGET myengine_assimp_lib PROPERTY IMPORTED_LOCATION "${MYENGINE_ASSIMP_LIB_DEBUG}")
  elseif(EXISTS "${MYENGINE_ASSIMP_LIB_RELEASE}")
    set_property(TARGET myengine_assimp_lib PROPERTY IMPORTED_LOCATION "${MYENGINE_ASSIMP_LIB_RELEASE}")
  endif()

  if(NOT EXISTS "${MYENGINE_ASSIMP_LIB_RELEASE}" AND EXISTS "${MYENGINE_ASSIMP_LIB_DEBUG}")
    message(WARNING "Assimp release library not found. Using Debug library for all configurations.")
  endif()

  target_link_libraries(myengine_assimp INTERFACE myengine_assimp_lib)
else()
  message(FATAL_ERROR "Assimp library not found in ${MYENGINE_ASSIMP_ROOT}/lib")
endif()

add_subdirectory(engine)
add_subdirectory(app)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT myengine)
