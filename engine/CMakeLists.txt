set(ENGINE_HEADERS
  include/myengine/config/AppConfig.h
  include/myengine/core/Application.h
  include/myengine/core/Logger.h
  include/myengine/core/ServiceLocator.h
  include/myengine/core/Timer.h
  include/myengine/core/Types.h
  include/myengine/core/Window.h
  include/myengine/editor/EditorCommandHistory.h
  include/myengine/editor/EditorState.h
  include/myengine/editor/TransformGizmo.h
  include/myengine/ecs/Component.h
  include/myengine/ecs/Entity.h
  include/myengine/ecs/Registry.h
  include/myengine/ecs/System.h
  include/myengine/ecs/World.h
  include/myengine/ecs/components/CameraComponent.h
  include/myengine/ecs/components/CameraControllerComponent.h
  include/myengine/ecs/components/ColliderComponent.h
  include/myengine/ecs/components/HierarchyComponent.h
  include/myengine/ecs/components/MeshRendererComponent.h
  include/myengine/ecs/components/MotionComponent.h
  include/myengine/ecs/components/PlayerControllerComponent.h
  include/myengine/ecs/components/RigidbodyComponent.h
  include/myengine/ecs/components/TagComponent.h
  include/myengine/ecs/components/TransformComponent.h
  include/myengine/ecs/components/Vector3.h
  include/myengine/ecs/components/WindowBindingComponent.h
  include/myengine/ecs/systems/CameraControlSystem.h
  include/myengine/ecs/systems/DebugRenderSystem.h
  include/myengine/ecs/systems/MotionSystem.h
  include/myengine/ecs/systems/PhysicsSystem.h
  include/myengine/ecs/systems/PlayerControlSystem.h
  include/myengine/ecs/systems/RenderSystem.h
  include/myengine/events/EventBus.h
  include/myengine/input/InputManager.h
  include/myengine/physics/PhysicsEvents.h
  include/myengine/physics/PhysicsWorldState.h
  include/myengine/render/IRenderAdapter.h
  include/myengine/render/RenderTypes.h
  include/myengine/render/dx12/Dx12Context.h
  include/myengine/render/dx12/Dx12RenderAdapter.h
  include/myengine/resource/ResourceManager.h
  include/myengine/scene/SceneSerializer.h
  include/myengine/scene/TransformUtils.h
  include/myengine/spatial/Octree.h
  include/myengine/spatial/UniformGrid2D.h
  include/myengine/spatial/UniformGrid3D.h
  include/myengine/state/IGameState.h
  include/myengine/state/StateMachine.h
  include/myengine/ui/SceneEditor.h
  include/myengine/ui/UiManager.h
)

set(ENGINE_SOURCES
  src/config/AppConfig.cpp
  src/core/Application.cpp
  src/core/Logger.cpp
  src/core/Timer.cpp
  src/core/Window.cpp
  src/editor/EditorCommandHistory.cpp
  src/editor/TransformGizmo.cpp
  src/ecs/World.cpp
  src/ecs/systems/CameraControlSystem.cpp
  src/ecs/systems/DebugRenderSystem.cpp
  src/ecs/systems/MotionSystem.cpp
  src/ecs/systems/PhysicsSystem.cpp
  src/ecs/systems/PlayerControlSystem.cpp
  src/ecs/systems/RenderSystem.cpp
  src/input/InputManager.cpp
  src/render/dx12/Dx12RenderAdapter.cpp
  src/resource/ResourceManager.cpp
  src/scene/SceneSerializer.cpp
  src/scene/TransformUtils.cpp
  src/state/StateMachine.cpp
  src/ui/SceneEditor.cpp
  src/ui/UiManager.cpp
)

set(ENGINE_OWN_HEADERS ${ENGINE_HEADERS})
set(ENGINE_OWN_SOURCES ${ENGINE_SOURCES})

set(IMGUI_ROOT "${CMAKE_SOURCE_DIR}/external/imgui")
set(IMGUIZMO_ROOT "${CMAKE_SOURCE_DIR}/external/ImGuizmo")

set(IMGUI_HEADERS
  ${IMGUI_ROOT}/Include/imgui/imconfig.h
  ${IMGUI_ROOT}/Include/imgui/imgui.h
  ${IMGUI_ROOT}/Include/imgui/imgui_internal.h
  ${IMGUI_ROOT}/Include/imgui/imstb_rectpack.h
  ${IMGUI_ROOT}/Include/imgui/imstb_textedit.h
  ${IMGUI_ROOT}/Include/imgui/imstb_truetype.h
  ${IMGUI_ROOT}/Include/imgui/misc/imgui_stdlib.h
)

set(IMGUI_SOURCES
  ${IMGUI_ROOT}/src/imgui.cpp
  ${IMGUI_ROOT}/src/imgui_demo.cpp
  ${IMGUI_ROOT}/src/imgui_draw.cpp
  ${IMGUI_ROOT}/src/imgui_tables.cpp
  ${IMGUI_ROOT}/src/imgui_widgets.cpp
  ${IMGUI_ROOT}/src/misc/imgui_stdlib.cpp
)

set(IMGUIZMO_HEADERS
  ${IMGUIZMO_ROOT}/ImGuizmo.h
)

set(IMGUIZMO_SOURCES
  ${IMGUIZMO_ROOT}/ImGuizmo.cpp
)

list(APPEND ENGINE_HEADERS ${IMGUI_HEADERS})
list(APPEND ENGINE_SOURCES ${IMGUI_SOURCES})
list(APPEND ENGINE_HEADERS ${IMGUIZMO_HEADERS})
list(APPEND ENGINE_SOURCES ${IMGUIZMO_SOURCES})

add_library(myengine_engine STATIC ${ENGINE_HEADERS} ${ENGINE_SOURCES})

target_include_directories(myengine_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${IMGUI_ROOT}/Include
    ${IMGUI_ROOT}/Include/imgui
    ${IMGUIZMO_ROOT})

target_compile_definitions(myengine_engine
  PUBLIC
    MYENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(myengine_engine
  PUBLIC
    myengine_assimp
    myengine_stb
    myengine_nlohmann_json
    myengine_directxheaders
    myengine_directxtk
    myengine_directxtex
    d3d12
    dxgi
    d3dcompiler
    dxguid
    imm32
    user32
    dwmapi
    gdi32
    shell32)

myengine_apply_compiler_options(myengine_engine)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include" PREFIX "Include" FILES ${ENGINE_OWN_HEADERS})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source" FILES ${ENGINE_OWN_SOURCES})
source_group(TREE "${IMGUI_ROOT}" PREFIX "ThirdParty\\ImGui" FILES ${IMGUI_HEADERS} ${IMGUI_SOURCES})
source_group(TREE "${IMGUIZMO_ROOT}" PREFIX "ThirdParty\\ImGuizmo" FILES ${IMGUIZMO_HEADERS} ${IMGUIZMO_SOURCES})
