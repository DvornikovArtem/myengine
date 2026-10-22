set(RL_HEADERS
  include/rl/RlArena.h
  include/rl/RlBotForecast.h
  include/rl/RlCamera.h
  include/rl/RlControls.h
  include/rl/RlMatch.h
  include/rl/RlNet.h
  include/rl/RlNetProtocol.h
  include/rl/net/NetLink.h
  include/rl/net/NetReliable.h
  include/rl/net/NetTransport.h
  include/rl/RlRng.h
  include/rl/RlSim.h
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
  src/RlCarController.cpp
  src/RlControls.cpp
  src/RlCarController.h
  src/RlJolt.cpp
  src/RlJolt.h
  src/RlMatch.cpp
  src/RlNetClient.cpp
  src/RlNetHost.cpp
  src/RlNetProtocol.cpp
  src/net/NetLink.cpp
  src/net/NetReliable.cpp
  src/RlSim.cpp
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

myengine_apply_compiler_options(myengine_rl)
set_property(TARGET myengine_rl PROPERTY FOLDER "Game")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RL_HEADERS} ${RL_SOURCES})
