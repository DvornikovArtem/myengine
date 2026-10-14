# Runs at build time (cmake -P) from the shared copy target of myengine_copy_runtime_assets.
#   DST_DIR      the output directory of the executables
#   SOURCE_DIR   repository root
#   COPY_ASSETS  ON: copy assets/ and config/
#   COPY_RUNTIME ON: copy the contents of external/runtime/ (python314.dll, python314.zip)
#
# file(COPY) skips a file whose copy in the destination has the same timestamp. A rebuild that changed nothing costs
# about 0.1 s, and a runtime DLL that a running process holds open is not rewritten.
if(NOT DST_DIR OR NOT SOURCE_DIR)
  message(FATAL_ERROR "CopyRuntimeAssetsRun.cmake: DST_DIR and SOURCE_DIR are required")
endif()

if(COPY_ASSETS)
  file(COPY "${SOURCE_DIR}/assets" DESTINATION "${DST_DIR}")
  file(COPY "${SOURCE_DIR}/config" DESTINATION "${DST_DIR}")
endif()

if(COPY_RUNTIME)
  # the trailing slash copies the contents of the folder, not the folder itself
  file(COPY "${SOURCE_DIR}/external/runtime/" DESTINATION "${DST_DIR}")
endif()
