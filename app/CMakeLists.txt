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
  ${CMAKE_SOURCE_DIR}/assets/shaders/debug_line.hlsl
  ${CMAKE_SOURCE_DIR}/assets/shaders/debug_line.shader.json
  ${CMAKE_SOURCE_DIR}/assets/shaders/textured_lit.hlsl
  ${CMAKE_SOURCE_DIR}/assets/shaders/textured_lit.shader.json
  ${CMAKE_SOURCE_DIR}/assets/shaders/ui_overlay.hlsl
  ${CMAKE_SOURCE_DIR}/assets/shaders/ui_overlay.shader.json
)

set_source_files_properties(${APP_SHADER_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)

add_executable(myengine WIN32 ${APP_HEADERS} ${APP_CPP} ${APP_SHADER_FILES})
target_link_libraries(myengine PRIVATE myengine_engine myengine_directxtk myengine_directxtex)
target_include_directories(myengine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MYENGINE_SHIPPING)
  # The game for players: RocketArena.exe, without the repository path (docs/game/packaging.md)
  set_target_properties(myengine PROPERTIES OUTPUT_NAME RocketArena)
else()
  target_compile_definitions(myengine PRIVATE MYENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

myengine_apply_compiler_options(myengine)
myengine_copy_runtime_assets(myengine)
# With MYENGINE_STEAM: steam_api64.dll and steam_appid.txt next to the exe (docs/game/steam-setup.md)
myengine_rl_steam_runtime(myengine)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Include" FILES ${APP_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES ${APP_CPP})
source_group("Shaders" FILES ${APP_SHADER_FILES})

# MCP server for the assistant: the Claude Code CLI starts it and it talks to the editor through a named pipe.
# The shipping build has no assistant.
if(NOT MYENGINE_SHIPPING)
  add_executable(myengine_mcp mcp_bridge/main.cpp)
  target_include_directories(myengine_mcp PRIVATE ${CMAKE_SOURCE_DIR}/engine/include)
  target_link_libraries(myengine_mcp PRIVATE myengine_nlohmann_json)
  myengine_apply_compiler_options(myengine_mcp)
  add_dependencies(myengine myengine_mcp)
endif()
