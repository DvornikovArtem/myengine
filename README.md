# myengine

A learning DirectX 12 engine skeleton created for ITMO University practical work.

This repository contains:
- `myengine_engine` - static library with reusable engine runtime systems
- `myengine` - executable application that uses the engine

## 1. What This Project Demonstrates

- Win32 application lifecycle (`Initialize -> Run -> Shutdown`)
- High-precision frame timer (`deltaTime` via `QueryPerformanceCounter`)
- State machine (`LoadingState`, `MenuState`, `GameplayState`)
- Logging system (file + debugger output)
- Rendering abstraction (`IRenderAdapter`) with DX12 backend (`Dx12RenderAdapter`)
- ECS core (`Entity`, `Component`, `System`, `Registry`, `World`)
- ECS systems:
  - `CameraControlSystem`
  - `MotionSystem`
  - `RenderSystem`
- Scene hierarchy (`HierarchyComponent`) with parent-child transforms
- Scene serialization/deserialization to JSON (`SceneSerializer`)
- 2D spatial index for broad-phase culling (`UniformGrid2D`)
- Texture loading through `ResourceManager` + `DirectXTex` (DDS/TGA/HDR/WIC path)
- `DirectXTK::SimpleMath` usage for camera math
- Multi-window runtime from JSON config

## 2. Requirements

- Windows 10/11
- Visual Studio 2022 (Desktop C++)
- CMake 3.24+
- Windows SDK with DX12 libraries

External folders expected in repository:
- `external/directxtk`
- `external/directxtex`

## 3. Quick Start (Scripts)

From repository root:

1. Configure project files:
```bat
setup.bat
```

2. Build Debug:
```bat
build.bat
```

3. Run Debug:
```bat
run.bat
```

Additional helpers:

- Reconfigure + rebuild:
```bat
rebuild.bat
```

- Remove build directory:
```bat
clean.bat
```

## 4. Manual Build Commands

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target myengine
./build/app/Debug/myengine.exe
```

## 5. Runtime Overview

Startup:
1. `app/src/main.cpp` loads config (`config/app.json`)
2. `Application::Initialize` creates logger, render adapter, windows and DX12 surfaces
3. ECS systems are registered (`CameraControlSystem`, `MotionSystem`, `RenderSystem`)
4. Scene path is set to `assets/scenes/scene`
5. If scene file exists -> load JSON; otherwise build demo scene and save it
6. Main loop starts

Per frame (`Application::Run`):
1. Process Win32 messages
2. Update timer (`deltaTime`)
3. Update ECS systems
4. Update current game state
5. Render each active window through `RenderSystem`

Shutdown:
1. Save scene back to `assets/scenes/scene`
2. Shutdown render adapter and release runtime objects

## 6. Input and Controls (Current)

### 6.1 State controls

- `MenuState`:
  - `Enter` -> switch to `GameplayState`
  - `Esc` -> exit app
- `GameplayState`:
  - `Esc` -> back to `MenuState`

### 6.2 Camera controls

Camera controls are active only while right mouse button is held in a window:
- On RMB down:
  - this window becomes input owner
  - cursor is hidden
  - cursor is recentered each move
- On RMB up:
  - camera control mode is disabled
  - cursor is shown

Movement keys (`CameraControlSystem`):
- `W` / `Up` - forward
- `S` / `Down` - backward
- `A` / `Left` - strafe left
- `D` / `Right` - strafe right
- `Space` - move up (world Y)
- `Left Shift` / `Shift` - move down (world Y)

Mouse:
- Mouse move - yaw/pitch rotation (pitch is clamped to avoid looking exactly up/down)
- Mouse wheel - changes movement speed multiplicatively

## 7. ECS Architecture (Current)

Core:
- `ecs/Entity.h` - entity id type
- `ecs/Component.h` - component marker type
- `ecs/Registry.h` - per-component-type storage and typed iteration
- `ecs/World.h/.cpp` - entity lifecycle, hierarchy links, system lists
- `ecs/System.h` - update/render system interfaces

Components:
- `TransformComponent`
- `TagComponent`
- `MeshRendererComponent`
- `HierarchyComponent`
- `MotionComponent`
- `CameraComponent`
- `CameraControllerComponent`
- `WindowBindingComponent`

Systems:
- `MotionSystem` - applies linear/angular velocities
- `CameraControlSystem` - camera movement and mouse look
- `RenderSystem`:
  - resolves camera view/projection
  - resolves hierarchy world transforms
  - runs broad-phase selection via `UniformGrid2D`
  - preloads textures
  - submits draw calls through `IRenderAdapter`

## 8. Rendering and Textures

`IRenderAdapter` API includes:
- `Initialize`
- `PreloadTexture`
- `CreateSurface`
- `ResizeSurface`
- `BeginFrame`
- `SetViewProjection`
- `DrawPrimitive`
- `EndFrame`
- `Shutdown`

DX12 implementation (`Dx12RenderAdapter`):
- Supports primitive rendering (`line`, `triangle`, `quad`, `cube`)
- Uses per-window swap chains and depth buffer
- Uploads textures into SRV heap and reuses cached descriptors

`ResourceManager` texture path:
- file load via `DirectXTex`
  - `.dds` -> `LoadFromDDSFile`
  - `.tga` -> `LoadFromTGAFile`
  - `.hdr` -> `LoadFromHDRFile`
  - others -> WIC loader (`LoadFromWICFile`)
- convert/decompress to `RGBA8` for current GPU upload path

## 9. Scene Serialization

Scene is stored in:
- `assets/scenes/scene`

`SceneSerializer` persists:
- entity ids
- transform/tag/mesh renderer
- hierarchy links
- motion
- window binding
- camera + camera controller params

## 10. Configuration (`config/app.json`)

Example:

```json
{
  "windows": [
    {
      "title": "myengine - main",
      "width": 1280,
      "height": 720,
      "clearColor": [0.08, 0.12, 0.20, 1.0]
    }
  ],
  "input": {
    "moveSpeed": 0.8,
    "scaleSpeed": 0.7,
    "rotateSpeedDeg": 120.0
  }
}
```

Notes:
- If config is missing/invalid, defaults are used
- If `windows` is empty, default window config is used

## 11. Build Notes

- Root CMake integrates both `DirectXTK` and `DirectXTex`
- `myengine_engine` links both interfaces publicly
- `myengine` executable links engine + interfaces

## 12. Repository Map

- `CMakeLists.txt` - root build configuration and external libs wiring
- `engine/CMakeLists.txt` - engine target and source list
- `app/CMakeLists.txt` - executable target
- `engine/include/myengine/...` - engine public headers
- `engine/src/...` - engine implementation
- `app/src/...` - application entry point and states
- `assets/shaders/primitive.hlsl` - shader used by renderer
- `assets/textures/debug.bmp` - sample texture
- `assets/scenes/scene` - serialized scene file
- `config/app.json` - runtime config

## 13. Runtime Files

Default executable path:
- `build/app/<Config>/myengine.exe`

Log file:
- `build/app/<Config>/logs/myengine.log`