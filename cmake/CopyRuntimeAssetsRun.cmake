# Runs at build time (cmake -P) from the shared copy target of myengine_copy_runtime_assets.
#   DST_DIR      the output directory of the executables
#   SOURCE_DIR   repository root
#   COPY_ASSETS  ON: copy assets/ and config/
#   COPY_RUNTIME ON: copy the contents of external/runtime/ (python314.dll, python314.zip)
#   COPY_ONNXRUNTIME ON: copy onnxruntime.dll of external/onnxruntime/lib (Face ID)
#
# file(COPY) skips a file whose copy in the destination has the same timestamp. A rebuild that changed nothing costs
# about 0.1 s, and a runtime DLL that a running process holds open is not rewritten.
if(NOT DST_DIR OR NOT SOURCE_DIR)
  message(FATAL_ERROR "CopyRuntimeAssetsRun.cmake: DST_DIR and SOURCE_DIR are required")
endif()

if(COPY_ASSETS)
  # A folder of assets may be a link (a junction or a symlink: large data kept on another disk). file(COPY) tries to
  # recreate the link itself and fails where that is not allowed ("cannot duplicate symlink"), so linked folders are
  # left out of the main copy and their contents are copied through the link instead.
  set(_linked_dirs "")
  set(_exclude_links "")
  foreach(_root assets config)
    file(GLOB_RECURSE _entries LIST_DIRECTORIES true "${SOURCE_DIR}/${_root}/*")
    foreach(_entry IN LISTS _entries)
      if(IS_SYMLINK "${_entry}" AND IS_DIRECTORY "${_entry}")
        file(RELATIVE_PATH _relative "${SOURCE_DIR}" "${_entry}")
        list(APPEND _linked_dirs "${_relative}")
        string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" _escaped "${_relative}")
        list(APPEND _exclude_links REGEX "/${_escaped}$" EXCLUDE)
      endif()
    endforeach()
  endforeach()

  # The fly brain connectome (160 MB, assets/flybrain/*.fbrain) is read from the sources, not copied next to every executable
  file(COPY "${SOURCE_DIR}/assets" DESTINATION "${DST_DIR}"
    PATTERN "*.fbrain" EXCLUDE PATTERN "*.fbrain.partial" EXCLUDE ${_exclude_links})
  file(COPY "${SOURCE_DIR}/config" DESTINATION "${DST_DIR}" ${_exclude_links})
  foreach(_relative IN LISTS _linked_dirs)
    # the folder the link points to; the trailing slash copies its contents
    file(REAL_PATH "${SOURCE_DIR}/${_relative}" _target)
    file(COPY "${_target}/" DESTINATION "${DST_DIR}/${_relative}"
      PATTERN "*.fbrain" EXCLUDE PATTERN "*.fbrain.partial" EXCLUDE)
  endforeach()
endif()

if(COPY_ONNXRUNTIME AND EXISTS "${SOURCE_DIR}/external/onnxruntime/lib/onnxruntime.dll")
  file(COPY "${SOURCE_DIR}/external/onnxruntime/lib/onnxruntime.dll" DESTINATION "${DST_DIR}")
endif()

if(COPY_RUNTIME)
  # the trailing slash copies the contents of the folder, not the folder itself
  file(COPY "${SOURCE_DIR}/external/runtime/" DESTINATION "${DST_DIR}")
endif()
