set(APP_HEADERS
  src/states/LoadingState.h
  src/states/MenuState.h
  src/states/GameplayState.h
)

set(APP_CPP
  src/main.cpp
  src/states/LoadingState.cpp
  src/states/MenuState.cpp
  src/states/GameplayState.cpp
)

set(APP_SHADER_FILES
  ${CMAKE_SOURCE_DIR}/assets/shaders/primitive.hlsl
  ${CMAKE_SOURCE_DIR}/assets/shaders/textured_lit.hlsl
  ${CMAKE_SOURCE_DIR}/assets/shaders/textured_lit.shader.json
)

set_source_files_properties(${APP_SHADER_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)

add_executable(myengine WIN32 ${APP_HEADERS} ${APP_CPP} ${APP_SHADER_FILES})
target_link_libraries(myengine PRIVATE myengine_engine myengine_directxtk myengine_directxtex)
target_include_directories(myengine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(myengine PRIVATE MYENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

myengine_apply_compiler_options(myengine)
myengine_copy_runtime_assets(myengine)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Include" FILES ${APP_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES ${APP_CPP})
source_group("Shaders" FILES ${APP_SHADER_FILES})
