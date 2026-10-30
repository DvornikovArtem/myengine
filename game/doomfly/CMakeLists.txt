set(DOOMFLY_HEADERS
  include/doomfly/FlyControls.h
  include/doomfly/FlyEyes.h
  include/doomfly/FlyModes.h
  include/doomfly/FlyPlayer.h
)

set(DOOMFLY_SOURCES
  src/FlyControls.cpp
  src/FlyEyes.cpp
  src/FlyModes.cpp
  src/FlyPlayer.cpp
)

# The fly that plays DOOM (docs/game/flydoom-fly.md): the eyes (the frame to the photoreceptors), the controls (descending neurons to the tic command),
# the reinforcement and the pace of the game. Knows the brain (game/flybrain) and the game (game/doom); no window, no DX12, no ECS.
add_library(myengine_doomfly STATIC ${DOOMFLY_HEADERS} ${DOOMFLY_SOURCES})

target_include_directories(myengine_doomfly
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # the panel of the brain is drawn into an ImDrawList (the headers of ImGui are the engine's private ones)
    ${CMAKE_SOURCE_DIR}/external/imgui/Include
    ${CMAKE_SOURCE_DIR}/external/imgui/Include/imgui)

target_link_libraries(myengine_doomfly
  PUBLIC
    myengine_flybrain
    myengine_doom
  PRIVATE
    myengine_brainview
    myengine_engine
    myengine_nlohmann_json)

myengine_apply_compiler_options(myengine_doomfly)
set_property(TARGET myengine_doomfly PROPERTY FOLDER "Game")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${DOOMFLY_HEADERS} ${DOOMFLY_SOURCES})
