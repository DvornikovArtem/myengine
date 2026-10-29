# myengine_copy_runtime_assets(<target>)
#
# Puts assets/, config/ and the Python runtime (external/runtime) next to the executable of <target>.
#
# The copy is not a POST_BUILD step of <target>: executables of one CMake directory (tests/) share one output folder,
# and with a parallel build several POST_BUILD copies wrote the same files at once and sometimes failed. Instead, one
# custom target per output folder does the copying, and every executable of that folder depends on it, so the copy
# runs once per build and finishes before any of them is linked. The target runs on every build of a dependent
# target; cmake/CopyRuntimeAssetsRun.cmake only copies the files that changed.
#
# All targets of one CMake directory must share the output folder. A directory that sets RUNTIME_OUTPUT_DIRECTORY on
# some targets before calling this function gets a separate copy target for each value.
function(myengine_copy_runtime_assets target_name)
  set(copy_assets OFF)
  if(MYENGINE_COPY_RUNTIME_ASSETS)
    set(copy_assets ON)
  else()
    message(STATUS "Runtime asset copy disabled for ${target_name} (MYENGINE_COPY_RUNTIME_ASSETS=OFF)")
  endif()

  set(copy_runtime OFF)
  if(EXISTS "${CMAKE_SOURCE_DIR}/external/runtime")
    set(copy_runtime ON)
  endif()

  # ONNX Runtime of Face ID (cmake/FaceId.cmake): onnxruntime.dll next to every executable that links the engine
  set(copy_onnxruntime OFF)
  if(MYENGINE_FACEID)
    set(copy_onnxruntime ON)
  endif()

  if(NOT copy_assets AND NOT copy_runtime AND NOT copy_onnxruntime)
    return()
  endif()

  get_target_property(custom_out_dir ${target_name} RUNTIME_OUTPUT_DIRECTORY)
  if(NOT custom_out_dir)
    set(custom_out_dir "")
  endif()
  file(RELATIVE_PATH binary_rel "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  string(REGEX REPLACE "[^A-Za-z0-9]+" "_" key "${binary_rel}_${custom_out_dir}")
  string(REGEX REPLACE "^_+|_+$" "" key "${key}")
  set(copy_target "myengine_copy_runtime_${key}")

  if(NOT TARGET ${copy_target})
    add_custom_target(${copy_target}
      COMMAND ${CMAKE_COMMAND}
        "-DDST_DIR=$<TARGET_FILE_DIR:${target_name}>"
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DCOPY_ASSETS=${copy_assets}"
        "-DCOPY_RUNTIME=${copy_runtime}"
        "-DCOPY_ONNXRUNTIME=${copy_onnxruntime}"
        -P "${CMAKE_SOURCE_DIR}/cmake/CopyRuntimeAssetsRun.cmake"
      COMMENT "Copying runtime assets next to ${target_name}"
      VERBATIM)
    set_property(TARGET ${copy_target} PROPERTY FOLDER "_CMake")
  endif()

  add_dependencies(${target_name} ${copy_target})
endfunction()
