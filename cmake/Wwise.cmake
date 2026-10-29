# Wwise (README.md, section "Wwise SDK"; docs/engine/wwise.md): the sound engine of Audiokinetic. OFF by default (until the game plays through it): then nothing here
# is looked for and the build is what it was. The SDK of Wwise is NOT in the repository: with MYENGINE_WWISE=ON the installed SDK is used, the one that the Audiokinetic
# Launcher puts on the machine. It is looked for in this order:
#
#   cmake -S . -B build -DMYENGINE_WWISE=ON -DWWISE_SDK_DIR=<...>/Wwise_.../SDK    the SDK that is named
#   cmake -S . -B build -DMYENGINE_WWISE=ON                                        the environment variable WWISESDK (the Launcher sets it)
#
# and if neither is there, CMake stops with the text of what to do. The libraries are taken for the compiler of the build: Visual Studio 2026 -> x64_vc180,
# Visual Studio 2022 -> x64_vc170 (the platform has to be installed in the Launcher). Nothing of the SDK (headers, libraries, files of the samples) is copied: the build reads it
# where it is installed, so the path of the SDK stays in the build folder only.
#
# Makes two targets when it is ON (and sets MYENGINE_WWISE_SDK to the SDK that is used):
#   myengine_wwise     the SDK: the headers as SYSTEM, the libraries of the configuration, the system libraries that they need
#   myengine_wwise_io  the low-level IO of the samples of the SDK (a file hook for the streaming manager), compiled here without our warning options

# The banks that the project makes belong to one version of the SDK
set(MYENGINE_WWISE_VERSION_MAJOR 2025)
set(MYENGINE_WWISE_VERSION_MINOR 1)
set(MYENGINE_WWISE_VERSION_SUBMINOR 12)
set(MYENGINE_WWISE_VERSION_BUILD 9287)
set(MYENGINE_WWISE_VERSION "${MYENGINE_WWISE_VERSION_MAJOR}.${MYENGINE_WWISE_VERSION_MINOR}.${MYENGINE_WWISE_VERSION_SUBMINOR}.${MYENGINE_WWISE_VERSION_BUILD}")

option(MYENGINE_WWISE "Build the Wwise audio runtime (the installed SDK ${MYENGINE_WWISE_VERSION} is needed: README.md, section Wwise SDK)" OFF)
set(WWISE_SDK_DIR "" CACHE PATH "The installed SDK of Wwise (the folder SDK with include/ and x64_vc180/ or x64_vc170/); empty: the environment variable WWISESDK")

if(NOT MYENGINE_WWISE)
  return()
endif()

set(_wwise_header "include/AK/SoundEngine/Common/AkSoundEngine.h")

# The libraries of the SDK are made per toolset of Visual Studio, with the dynamic run-time library (the "(StaticCRT)" ones are not ours):
# v145 (Visual Studio 2026, MSVC_VERSION 1950 and up) -> x64_vc180, v143 (Visual Studio 2022, 1930-1949) -> x64_vc170
if(NOT MSVC)
  message(FATAL_ERROR "MYENGINE_WWISE is ON, but the compiler is not MSVC: the libraries of the Wwise SDK are for Visual Studio 2026 (v145) or Visual Studio 2022 (v143). Without Wwise build with MYENGINE_WWISE=OFF (the default).")
elseif(MSVC_VERSION GREATER_EQUAL 1950)
  set(_wwise_platform "x64_vc180")
  set(_wwise_vs "Visual Studio 2026")
elseif(MSVC_VERSION GREATER_EQUAL 1930)
  set(_wwise_platform "x64_vc170")
  set(_wwise_vs "Visual Studio 2022")
else()
  message(FATAL_ERROR "MYENGINE_WWISE is ON, but the compiler (MSVC_VERSION ${MSVC_VERSION}) is older than Visual Studio 2022: the libraries of the Wwise SDK are for Visual Studio 2026 (v145) or Visual Studio 2022 (v143). "
                      "Without Wwise build with MYENGINE_WWISE=OFF (the default).")
endif()
string(CONCAT _wwise_help
  "Install Wwise ${MYENGINE_WWISE_VERSION} in the Audiokinetic Launcher (the package SDK (C++), the platform Windows (Microsoft ${_wwise_vs} / ${_wwise_platform}, x64) that is the Visual Studio of this build), "
  "restart the terminal so that WWISESDK is set, or pass -DWWISE_SDK_DIR=<...>/Wwise_${MYENGINE_WWISE_VERSION}/SDK. "
  "Steps: README.md, section \"Wwise SDK\", and docs/engine/wwise.md. Without Wwise build with MYENGINE_WWISE=OFF (the default).")

# --- which SDK
if(NOT "${WWISE_SDK_DIR}" STREQUAL "")
  set(_wwise_given "${WWISE_SDK_DIR}")
  set(_wwise_given_from "WWISE_SDK_DIR")
elseif(NOT "$ENV{WWISESDK}" STREQUAL "")
  set(_wwise_given "$ENV{WWISESDK}")
  set(_wwise_given_from "the environment variable WWISESDK")
else()
  message(FATAL_ERROR "MYENGINE_WWISE is ON, but the Wwise SDK is not found: WWISE_SDK_DIR is empty and the environment variable WWISESDK is not set. ${_wwise_help}")
endif()
file(TO_CMAKE_PATH "${_wwise_given}" _wwise_dir)
string(REGEX REPLACE "/+$" "" _wwise_dir "${_wwise_dir}")
if(NOT EXISTS "${_wwise_dir}/${_wwise_header}" AND EXISTS "${_wwise_dir}/SDK/${_wwise_header}")
  set(_wwise_dir "${_wwise_dir}/SDK")   # the folder of the installation, not the one of the SDK in it
endif()
if(NOT EXISTS "${_wwise_dir}/${_wwise_header}")
  message(FATAL_ERROR "The Wwise SDK is not found: ${_wwise_given_from} is '${_wwise_given}', and it has no ${_wwise_header} (it has to be the folder SDK of an installed "
                      "Wwise ${MYENGINE_WWISE_VERSION}). ${_wwise_help}")
endif()
set(MYENGINE_WWISE_SDK "${_wwise_dir}")
set(_wwise_include "${_wwise_dir}/include")
set(_wwise_samples "${_wwise_dir}/samples/SoundEngine")
set(_wwise_lib_root "${_wwise_dir}/${_wwise_platform}")   # <root>/<Debug|Profile|Release>/lib

# --- the version: the banks of the project are made for one
set(_wwise_found "")
foreach(_part MAJOR MINOR SUBMINOR BUILD)
  file(STRINGS "${_wwise_include}/AK/AkWwiseSDKVersion.h" _line REGEX "^#define[ \t]+AK_WWISESDK_VERSION_${_part}[ \t]+[0-9]+" LIMIT_COUNT 1)
  string(REGEX MATCH "[0-9]+$" _number "${_line}")
  list(APPEND _wwise_found "${_number}")
endforeach()
list(GET _wwise_found 0 _wwise_found_major)
list(GET _wwise_found 1 _wwise_found_minor)
list(JOIN _wwise_found "." _wwise_found_version)
if(NOT "${_wwise_found_major}.${_wwise_found_minor}" STREQUAL "${MYENGINE_WWISE_VERSION_MAJOR}.${MYENGINE_WWISE_VERSION_MINOR}")
  message(FATAL_ERROR "The Wwise SDK in ${_wwise_dir} is ${_wwise_found_version}, the project needs ${MYENGINE_WWISE_VERSION_MAJOR}.${MYENGINE_WWISE_VERSION_MINOR} (exactly ${MYENGINE_WWISE_VERSION}): "
                      "the headers and the libraries of another series do not fit the code, and the SoundBanks of one series do not load in another. ${_wwise_help}")
endif()
if(NOT "${_wwise_found_version}" STREQUAL "${MYENGINE_WWISE_VERSION}")
  message(WARNING "The Wwise SDK in ${_wwise_dir} is ${_wwise_found_version}, the SoundBanks of the project are made by ${MYENGINE_WWISE_VERSION}: banks of one build may not load in another. "
                  "Install ${MYENGINE_WWISE_VERSION} (README.md, section \"Wwise SDK\").")
endif()

# --- the platform: the libraries are read where they are installed
foreach(_flavour Debug Profile Release)
  if(NOT EXISTS "${_wwise_lib_root}/${_flavour}/lib/AkSoundEngine.lib")
    message(FATAL_ERROR "The Wwise SDK in ${_wwise_dir} has no ${_wwise_platform}/${_flavour}/lib/AkSoundEngine.lib: the platform Windows (Microsoft ${_wwise_vs} / ${_wwise_platform}, x64) "
                        "of the package SDK (C++) is not installed, and this build uses ${_wwise_vs} (MSVC_VERSION ${MSVC_VERSION}). Mark that platform in the Audiokinetic Launcher (Modify the installation of Wwise ${MYENGINE_WWISE_VERSION}). ${_wwise_help}")
  endif()
endforeach()

# Debug -> Debug, RelWithDebInfo -> Profile (optimised, with the profiler and the communication), Release and MinSizeRel -> Release (AK_OPTIMIZED: no communication, no monitor)
set(_wwise_lib_dir "${_wwise_lib_root}/$<IF:$<CONFIG:Debug>,Debug,$<IF:$<CONFIG:RelWithDebInfo>,Profile,Release>>/lib")
set(_wwise_optimized "$<CONFIG:Release,MinSizeRel>")

add_library(myengine_wwise INTERFACE)
# SYSTEM: the headers of Audiokinetic are not ours (/W4 does not apply to them)
target_include_directories(myengine_wwise SYSTEM INTERFACE "${_wwise_include}")
target_link_libraries(myengine_wwise INTERFACE
  "${_wwise_lib_dir}/AkSoundEngine.lib"
  "${_wwise_lib_dir}/AkMemoryMgr.lib"
  "${_wwise_lib_dir}/AkStreamMgr.lib"
  # the codecs of the banks (Vorbis is what Wwise converts to by default), registered by AK_STATIC_LINK_PLUGIN in WwiseRuntime.cpp
  "${_wwise_lib_dir}/AkVorbisDecoder.lib"
  "${_wwise_lib_dir}/AkOpusDecoder.lib"
  # the communication with the authoring (Profiler, Remote Connect): only in Debug and Profile
  "$<$<NOT:${_wwise_optimized}>:${_wwise_lib_dir}/CommunicationCentral.lib>"
  ws2_32 Msacm32 Dbghelp Winmm)
# Release libraries are built with AK_OPTIMIZED: the headers have to agree
target_compile_definitions(myengine_wwise INTERFACE "$<${_wwise_optimized}:AK_OPTIMIZED>")

# The low-level IO of the samples (reads the files of the banks, asynchronously): C++ of Audiokinetic, compiled from the SDK folder
set(_wwise_io_sources
  "${_wwise_samples}/Win32/AkDefaultIOHookDeferred.cpp"
  "${_wwise_samples}/Common/AkMultipleFileLocation.cpp"
  "${_wwise_samples}/Common/AkFileLocationBase.cpp"
  "${_wwise_samples}/Common/AkGeneratedSoundBanksResolver.cpp"
  "${_wwise_samples}/Common/AkJobWorkerMgr.cpp")
foreach(_source IN LISTS _wwise_io_sources)
  if(NOT EXISTS "${_source}")
    message(FATAL_ERROR "The Wwise SDK in ${_wwise_dir} has no ${_source}: the samples of the SDK (samples/SoundEngine/Win32 and Common) are part of the installation of the package SDK (C++). ${_wwise_help}")
  endif()
endforeach()
add_library(myengine_wwise_io STATIC ${_wwise_io_sources})
target_include_directories(myengine_wwise_io SYSTEM PUBLIC "${_wwise_samples}/Win32" "${_wwise_samples}/Common")
target_link_libraries(myengine_wwise_io PUBLIC myengine_wwise)
target_compile_features(myengine_wwise_io PUBLIC cxx_std_17)
if(MSVC)
  # not our code: no /W4 /WX (myengine_apply_compiler_options is not applied), no warnings at all
  target_compile_options(myengine_wwise_io PRIVATE /W0 /EHsc /permissive-)
endif()
# UNICODE: the files of the samples call the generic names of the Windows API with AkOSChar (wchar_t)
target_compile_definitions(myengine_wwise_io PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set_property(TARGET myengine_wwise_io PROPERTY FOLDER "External")

message(STATUS "Wwise ${_wwise_found_version}: ${MYENGINE_WWISE_SDK}, platform ${_wwise_platform} (${_wwise_vs})")
