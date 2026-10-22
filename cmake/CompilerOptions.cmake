# AddressSanitizer needs its run-time DLL next to every executable. One custom target per output folder does the copy
# (the same scheme as myengine_copy_runtime_assets: a POST_BUILD step of every test raced on the same files).
function(myengine_copy_asan_runtime target_name)
  get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  set(runtime_dlls "")
  foreach(dll clang_rt.asan_dynamic-x86_64.dll clang_rt.asan_dbg_dynamic-x86_64.dll)
    if(EXISTS "${compiler_dir}/${dll}")
      list(APPEND runtime_dlls "${compiler_dir}/${dll}")
    endif()
  endforeach()
  if(NOT runtime_dlls)
    message(WARNING "MYENGINE_ASAN: clang_rt.asan_*_dynamic-x86_64.dll not found next to ${CMAKE_CXX_COMPILER}")
    return()
  endif()

  file(RELATIVE_PATH binary_rel "${CMAKE_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  string(REGEX REPLACE "[^A-Za-z0-9]+" "_" key "${binary_rel}")
  string(REGEX REPLACE "^_+|_+$" "" key "${key}")
  set(copy_target "myengine_asan_runtime_${key}")
  if(NOT TARGET ${copy_target})
    # The folder does not exist yet when the executable has not been linked
    add_custom_target(${copy_target}
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target_name}>"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${runtime_dlls} "$<TARGET_FILE_DIR:${target_name}>"
      COMMENT "Copying the AddressSanitizer runtime next to ${target_name}"
      VERBATIM)
    set_property(TARGET ${copy_target} PROPERTY FOLDER "_CMake")
  endif()
  add_dependencies(${target_name} ${copy_target})
endfunction()

function(myengine_apply_compiler_options target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_17)

  if(MSVC)
    target_compile_options(${target_name}
      PRIVATE
        /W4
        /permissive-
        /EHsc
        /FS
        /utf-8
        /Zc:__cplusplus
        /GT
        /DWIN32_LEAN_AND_MEAN
        /DNOMINMAX)

    target_compile_definitions(${target_name}
      PRIVATE
        UNICODE
        _UNICODE)

    if(MYENGINE_ASAN)
      target_compile_options(${target_name} PRIVATE /fsanitize=address)
      # The STL annotates std::vector / std::string for ASan and the linker rejects a mix of annotated and plain
      # objects; the prebuilt DirectXTK / DirectXTex are plain, so the annotations are off for all of ours
      # MYENGINE_ASAN_BUILD: the tests with a time limit relax it (ASan makes the code several times slower)
      target_compile_definitions(${target_name}
        PRIVATE
          _DISABLE_VECTOR_ANNOTATION
          _DISABLE_STRING_ANNOTATION
          _DISABLE_OPTIONAL_ANNOTATION   # the newer MSVC STL annotates std::optional too (LNK2038 "annotate_optional" against the plain DirectXTK / DirectXTex)
          MYENGINE_ASAN_BUILD)

      get_target_property(target_type ${target_name} TYPE)
      if(target_type STREQUAL "EXECUTABLE")
        target_link_options(${target_name} PRIVATE /INCREMENTAL:NO)
        myengine_copy_asan_runtime(${target_name})
      endif()
    endif()

    get_target_property(own_target_type ${target_name} TYPE)
    if(own_target_type STREQUAL "EXECUTABLE")
      # LNK4099: the prebuilt DirectXTK12 / DirectXTex (external/) were built with /Zi and their .pdb files are not shipped with the .lib files
      target_link_options(${target_name} PRIVATE /ignore:4099)
    endif()

    if(MYENGINE_ANALYZE)
      # SYSTEM headers (python, pybind11, DirectX) are external and are not analyzed
      target_compile_options(${target_name} PRIVATE /analyze /analyze:external-)
    endif()
  endif()
endfunction()
