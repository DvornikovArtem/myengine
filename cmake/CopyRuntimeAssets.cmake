function(myengine_copy_runtime_assets target_name)
  if(MYENGINE_COPY_RUNTIME_ASSETS)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:${target_name}>/assets")

    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/config"
        "$<TARGET_FILE_DIR:${target_name}>/config")
  else()
    message(STATUS "Runtime asset copy disabled for ${target_name} (MYENGINE_COPY_RUNTIME_ASSETS=OFF)")
  endif()

  if(EXISTS "${CMAKE_SOURCE_DIR}/external/runtime")
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/external/runtime"
        "$<TARGET_FILE_DIR:${target_name}>")
  endif()
endfunction()
