set(FLYBRAIN_HEADERS
  include/flybrain/Connectome.h
  include/flybrain/FlyBrain.h
)

set(FLYBRAIN_SOURCES
  src/Connectome.cpp
  src/FbrainFormat.h
  src/FlyBrain.cpp
)

# The fly brain of FlyDoom (docs/flybrain/flybrain.md): the connectome from a .fbrain file and its LIF simulation on the
# job system. Knows nothing about DOOM, rendering or the ECS.
add_library(myengine_flybrain STATIC ${FLYBRAIN_HEADERS} ${FLYBRAIN_SOURCES})

target_include_directories(myengine_flybrain
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(myengine_flybrain
  PRIVATE
    myengine_engine)

myengine_apply_compiler_options(myengine_flybrain)
set_property(TARGET myengine_flybrain PROPERTY FOLDER "Game")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${FLYBRAIN_HEADERS} ${FLYBRAIN_SOURCES})
