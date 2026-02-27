function(myengine_copy_runtime_assets target_name)
  add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/assets"
      "$<TARGET_FILE_DIR:${target_name}>/assets")

  add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_SOURCE_DIR}/config"
      "$<TARGET_FILE_DIR:${target_name}>/config")
endfunction()