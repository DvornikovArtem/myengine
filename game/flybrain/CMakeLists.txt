set(FLYBRAIN_HEADERS
  include/flybrain/Circuit.h
  include/flybrain/Connectome.h
  include/flybrain/FlyBrain.h
  include/flybrain/FlySwarmBrain.h
  include/flybrain/KcVision.h
)

set(FLYBRAIN_SOURCES
  src/Circuit.cpp
  src/Connectome.cpp
  src/FbrainFormat.h
  src/FlyBrain.cpp
  src/FlyBrainGpu.cpp
  src/FlyBrainGpu.h
  src/FlyBrainPlasticity.cpp
  src/FlyBrainPlasticity.h
  src/FlySwarmBrain.cpp
  src/KcVision.cpp
)

# The fly brain of FlyDoom (docs/flybrain/flybrain.md): the connectome from a .fbrain file and its LIF simulation on the
# job system, or on the GPU with DX12 compute shaders (assets/shaders/flybrain.hlsl). Knows nothing about DOOM or the ECS.
add_library(myengine_flybrain STATIC ${FLYBRAIN_HEADERS} ${FLYBRAIN_SOURCES})

target_include_directories(myengine_flybrain
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # the shader compiler of the renderer (ShaderCompiler.h) is an internal header of the engine
    ${CMAKE_SOURCE_DIR}/engine/src/render/dx12)

target_link_libraries(myengine_flybrain
  PRIVATE
    myengine_engine)

myengine_apply_compiler_options(myengine_flybrain)
set_property(TARGET myengine_flybrain PROPERTY FOLDER "Game")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${FLYBRAIN_HEADERS} ${FLYBRAIN_SOURCES})
