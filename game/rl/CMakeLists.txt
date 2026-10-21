set(RL_HEADERS
  include/rl/RlArena.h
  include/rl/RlSim.h
  include/rl/RlTuning.h
  include/rl/RlTypes.h
)

set(RL_SOURCES
  src/RlArena.cpp
  src/RlCarController.cpp
  src/RlCarController.h
  src/RlJolt.cpp
  src/RlJolt.h
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
    myengine_nlohmann_json)

myengine_apply_compiler_options(myengine_rl)
set_property(TARGET myengine_rl PROPERTY FOLDER "Game")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RL_HEADERS} ${RL_SOURCES})
