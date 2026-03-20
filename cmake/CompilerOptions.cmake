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
        /DWIN32_LEAN_AND_MEAN
        /DNOMINMAX)

    target_compile_definitions(${target_name}
      PRIVATE
        UNICODE
        _UNICODE)
  endif()
endfunction()
