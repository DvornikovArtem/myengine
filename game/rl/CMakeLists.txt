set(RL_HEADERS
  include/rl/RlArena.h
  include/rl/RlBotForecast.h
  include/rl/RlCamera.h
  include/rl/RlCinematic.h
  include/rl/RlControls.h
  include/rl/RlMatch.h
  include/rl/RlName.h
  include/rl/RlNet.h
  include/rl/RlNetProtocol.h
  include/rl/net/LobbyLan.h
  include/rl/net/LobbyService.h
  include/rl/net/LobbySteam.h
  include/rl/RlReplay.h
  include/rl/net/NetLink.h
  include/rl/net/NetReliable.h
  include/rl/net/NetTransport.h
  include/rl/RlRng.h
  include/rl/RlSim.h
  include/rl/RlTraining.h
  include/rl/RlTrainingAutopilot.h
  include/rl/RlTuning.h
  include/rl/RlTypes.h
)

set(RL_SOURCES
  src/RlArena.cpp
  src/RlBots.cpp
  src/RlBots.h
  src/RlBotsBrain.cpp
  src/RlBotsBrain.h
  src/RlBotsV1.cpp
  src/RlBotsV1.h
  src/RlCamera.cpp
  src/RlCinematic.cpp
  src/RlCarController.cpp
  src/RlControls.cpp
  src/RlCarController.h
  src/RlJolt.cpp
  src/RlJolt.h
  src/RlMatch.cpp
  src/RlNetClient.cpp
  src/RlNetHost.cpp
  src/RlNetProtocol.cpp
  src/net/LobbyLan.cpp
  src/net/LobbySteamStub.cpp
  src/RlReplay.cpp
  src/RlV3.h
  src/net/NetLink.cpp
  src/net/NetReliable.cpp
  src/RlSim.cpp
  src/RlTraining.cpp
  src/RlTrainingAutopilot.cpp
  src/RlTuning.cpp
)

# The physics of the Rocket League game: Jolt world, arena mesh, car controller. No DX12, ImGui or ECS in here.
add_library(myengine_rl STATIC ${RL_HEADERS} ${RL_SOURCES})

target_include_directories(myengine_rl
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(myengine_rl
  PRIVATE
    Jolt
    TracyClient
    myengine_nlohmann_json
    ws2_32)

# Steam (docs/game/steam-setup.md): the lobby and the transport over the Steamworks SDK. OFF by default, the SDK is not in the repository.
set(RL_STEAM_SOURCES
  src/net/steam/SteamLobbyService.cpp
  src/net/steam/SteamTransport.cpp
  src/net/steam/SteamTransport.h)
option(MYENGINE_STEAM "Build the Steam lobby and transport (needs the Steamworks SDK in STEAMWORKS_SDK_DIR)" OFF)
set(STEAMWORKS_SDK_DIR "" CACHE PATH "Folder of the unpacked Steamworks SDK (the one with public/ and redistributable_bin/)")
option(MYENGINE_STEAM_CHECK "Compile the Steam sources against game/rl/steam_mock (syntax check only, nothing is linked)" OFF)

# Next to the exe of <target>: steam_api64.dll and steam_appid.txt (480). A no-op without MYENGINE_STEAM.
function(myengine_rl_steam_runtime target)
  if(MYENGINE_STEAM)
    file(WRITE "${CMAKE_BINARY_DIR}/steam_appid.txt" "480\n")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.dll" "${CMAKE_BINARY_DIR}/steam_appid.txt" "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
  endif()
endfunction()

if(MYENGINE_STEAM)
  if(NOT EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
    message(FATAL_ERROR "MYENGINE_STEAM is ON but STEAMWORKS_SDK_DIR ('${STEAMWORKS_SDK_DIR}') has no public/steam/steam_api.h. See docs/game/steam-setup.md")
  endif()
  target_sources(myengine_rl PRIVATE ${RL_STEAM_SOURCES})
  # SYSTEM: the headers of Valve are not ours (strncpy, a BOM in some of them): /W4 does not apply
  target_include_directories(myengine_rl SYSTEM PRIVATE "${STEAMWORKS_SDK_DIR}/public")
  target_compile_definitions(myengine_rl PUBLIC MYENGINE_STEAM=1)
  target_link_libraries(myengine_rl PUBLIC "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.lib")
elseif(MYENGINE_STEAM_CHECK)
  add_library(myengine_rl_steam_check OBJECT ${RL_STEAM_SOURCES})
  target_include_directories(myengine_rl_steam_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/steam_mock ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_definitions(myengine_rl_steam_check PRIVATE MYENGINE_STEAM=1)
  myengine_apply_compiler_options(myengine_rl_steam_check)
  set_property(TARGET myengine_rl_steam_check PROPERTY FOLDER "Game")
endif()

myengine_apply_compiler_options(myengine_rl)
set_property(TARGET myengine_rl PROPERTY FOLDER "Game")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RL_HEADERS} ${RL_SOURCES})
