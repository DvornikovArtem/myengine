set(ENGINE_HEADERS
  include/myengine/config/AppConfig.h
  include/myengine/core/Application.h
  include/myengine/core/Logger.h
  include/myengine/core/Timer.h
  include/myengine/core/Types.h
  include/myengine/core/Window.h
  include/myengine/input/InputManager.h
  include/myengine/render/IRenderAdapter.h
  include/myengine/render/RenderTypes.h
  include/myengine/render/dx12/Dx12Context.h
  include/myengine/render/dx12/Dx12RenderAdapter.h
  include/myengine/scene/TestPrimitive.h
  include/myengine/state/IGameState.h
  include/myengine/state/StateMachine.h
)

set(ENGINE_SOURCES
  src/config/AppConfig.cpp
  src/core/Application.cpp
  src/core/Logger.cpp
  src/core/Timer.cpp
  src/core/Window.cpp
  src/input/InputManager.cpp
  src/render/dx12/Dx12RenderAdapter.cpp
  src/state/StateMachine.cpp
)

add_library(myengine_engine STATIC ${ENGINE_HEADERS} ${ENGINE_SOURCES})

target_include_directories(myengine_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

target_link_libraries(myengine_engine
  PUBLIC
    d3d12
    dxgi
    d3dcompiler
    dxguid
    user32
    gdi32
    shell32)

myengine_apply_compiler_options(myengine_engine)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Include" FILES ${ENGINE_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES ${ENGINE_SOURCES})
