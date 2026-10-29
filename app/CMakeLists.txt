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

# The icon and VERSIONINFO of the exe (app/resources): the version comes from project(... VERSION) in the root CMakeLists.txt
if(MYENGINE_SHIPPING)
  set(MYENGINE_RC_PRODUCT "Rocket Arena")
  set(MYENGINE_RC_FILE "RocketArena.exe")
  set(MYENGINE_RC_DESCRIPTION "Rocket Arena - car football for friends")
else()
  set(MYENGINE_RC_PRODUCT "myengine")
  set(MYENGINE_RC_FILE "myengine.exe")
  set(MYENGINE_RC_DESCRIPTION "myengine - the editor and the Rocket Arena game")
endif()
configure_file(resources/RocketArena.rc.in "${CMAKE_CURRENT_BINARY_DIR}/RocketArena.rc" @ONLY)
configure_file(resources/RocketArena.ico "${CMAKE_CURRENT_BINARY_DIR}/RocketArena.ico" COPYONLY)
set(APP_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/RocketArena.rc")
# Per-Monitor V2 DPI awareness (resources/myengine.manifest): a .manifest among the sources is merged into the manifest of the exe by the linker
set(APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/resources/myengine.manifest")
set_source_files_properties(${APP_RESOURCES} PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/RocketArena.ico")

add_executable(myengine WIN32 ${APP_HEADERS} ${APP_CPP} ${APP_SHADER_FILES} ${APP_RESOURCES} ${APP_MANIFEST})
target_link_libraries(myengine PRIVATE myengine_engine myengine_brainview myengine_directxtk myengine_directxtex)
target_include_directories(myengine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MYENGINE_SHIPPING)
  # The game for players: RocketArena.exe, without the repository path (docs/game/packaging.md)
  set_target_properties(myengine PROPERTIES OUTPUT_NAME RocketArena)
  # A PDB for the minidumps of the players (core/CrashHandler.h): the names of the functions of a crash. It stays out of the package
  # (package_game.ps1 keeps it in dist/symbols); the exe names it without the folder, so no path of the repository is in the exe.
  target_link_options(myengine PRIVATE /DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:%_PDB%)
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
