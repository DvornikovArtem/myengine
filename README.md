# myengine

A learning DirectX 12 engine skeleton created for ITMO University practical work.

This repository contains:
- `myengine_engine` - static library with reusable engine runtime systems
- `myengine` - executable application that uses the engine

Архитектура, порядок инициализации и UML-диаграммы: [`docs/architecture.md`](docs/architecture.md).

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
- Visual Studio 2026 (Visual Studio 18) or Visual Studio 2022 (Visual Studio 17), with Desktop development with C++
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

`setup.bat` detects Visual Studio 18 (2026) first and falls back to Visual Studio 17 (2022). A build directory configured with a different generator is not removed automatically; use a new build directory or remove its cache manually.

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
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DMYENGINE_COPY_RUNTIME_ASSETS=ON
# Use "Visual Studio 17 2022" when Visual Studio 18 is not installed.
cmake --build build --config Debug --target myengine
./build/app/Debug/myengine.exe
```

### 4.1 Wwise SDK (optional)

The game can play its sound through Wwise (Audiokinetic) instead of XAudio2. This is the `MYENGINE_WWISE` option, **OFF by default**: without the SDK the build above works as it always did, and nothing in this section is needed. The Wwise SDK is **not in this repository** (it is Audiokinetic's software and is under their license): each developer installs it once on their machine, the way the Steamworks SDK is installed (`-SteamSdk`). Only our own code that uses Wwise is in the repository, together with the Wwise project (`wwise/RocketArena`) and its built banks (`assets/audio/wwise/*.bnk`).

Install the SDK (steps from the [Audiokinetic Launcher guide](https://www.audiokinetic.com/en/public-library/Launcher_2026.1.1.6296/?source=InstallGuide&id=install_wwise_through_launcher)):

1. Create an Audiokinetic account: the Launcher opens the Customer Portal on the first *Sign in* ([guide](https://www.audiokinetic.com/en/public-library/Launcher_2026.1.1.6296/?source=InstallGuide&id=log_in)).
2. Install the Audiokinetic Launcher (download from audiokinetic.com, [guide](https://www.audiokinetic.com/en/public-library/launcher_2026.1.1.6296/?source=InstallGuide&id=getting_started)) and sign in.
3. In the Launcher open the *Wwise* page, choose the version **2025.1.12.9287** in the *Install New Version* list and click *Install*. The project needs exactly this version: the banks of the repository are made by it. A different build of 2025.1 builds with a warning (banks of one build may not load in another), another series (not 2025.1) is refused by CMake.
4. On the packages and platforms page mark:
   - package **SDK (C++)** (required: the headers, the libraries, the samples of the sound engine);
   - deployment platform **Microsoft / Windows / Visual Studio 2026** (the folder `x64_vc180` of the SDK) if you build with Visual Studio 2026, and/or **Microsoft / Windows / Visual Studio 2022** (`x64_vc170`) if you build with Visual Studio 2022: mark the one that is your Visual Studio, or both (CMake takes the platform of the compiler and names the missing one in its error);
   - package **Authoring** only if you want to open `wwise/RocketArena/RocketArena.wproj` and change the sounds (not needed to build or play);
   - no plug-ins are needed. Click *Install*.
5. Set the environment variables: in the Launcher, next to the installed version, open the menu and choose *Set Environment Variables* (it offers `WWISEROOT` and `WWISESDK`). **Restart the terminal** (and the IDE) after that, so that new processes see `WWISESDK`.

Build with the SDK:

```powershell
# WWISESDK is set (by the Launcher, step 5):
cmake -S . -B build-wwise -DMYENGINE_WWISE=ON
# or name the SDK yourself (the folder SDK of the installation) when the variable is not set:
cmake -S . -B build-wwise -DMYENGINE_WWISE=ON -DWWISE_SDK_DIR=<path to Wwise_2025.1.12.9287>/SDK
cmake --build build-wwise --config Debug --target myengine
```

CMake looks at `-DWWISE_SDK_DIR` first, then at `WWISESDK`; if neither is there, or the version or the platform is wrong, it stops with a message that says what is missing. `tools/package_game.ps1` (the package of the game) builds with Wwise when it finds the same SDK (`-WwiseSdk <folder>` names it); without the SDK it prints a warning and builds the package with XAudio2, like `-NoWwise` does. Details: [docs/engine/wwise.md](docs/engine/wwise.md) (in Russian).

License: the Wwise license terms are Audiokinetic's, and each developer accepts them for themselves when installing (the Launcher shows them). On [their pricing page](https://www.audiokinetic.com/en/wwise/pricing/) the free trial (no license key) is for non-commercial projects and is limited to 200 media assets in the SoundBanks; our project has 24 media files. A free non-commercial license without that limit is given when the project is [registered](https://www.audiokinetic.com/register-project/).

## 5. Runtime Overview

Startup:
1. `app/src/main.cpp` loads config (`config/app.json`)
2. `Application::Initialize` creates logger, render adapter, windows and DX12 surfaces
3. ECS systems are registered (`CameraControlSystem`, `MotionSystem`, `RenderSystem`)
4. Scene path is set to `assets/scenes/scene.json`
5. If scene file exists -> load JSON; otherwise build demo scene and save it
6. Main loop starts

Per frame (`Application::Run`):
1. Process Win32 messages
2. Update timer (`deltaTime`)
3. Update ECS systems
4. Update current game state
5. Render each active window through `RenderSystem`

Shutdown:
1. Save scene back to `assets/scenes/scene.json`
2. Shutdown render adapter and release runtime objects

## 6. Input and Controls (Current)

### 6.1 State controls

- `MenuState`:
  - `Enter` -> switch to `GameplayState`
  - `Esc` -> exit app (the editor never enters `MenuState` now)
- `GameplayState`:
  - `Esc` is handled by the editor: Play -> stop Play (same as the Stop button), Edit -> nothing; it never closes the app

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
- `E` / `Space` - move up (world Y)
- `Q` / `Left Shift` / `Shift` - move down (world Y)

Mouse:
- Mouse move - yaw/pitch rotation (pitch is clamped to avoid looking exactly up/down)
- Mouse wheel - changes movement speed multiplicatively

### 6.3 Player controls

Player controls are active in Play mode, in the active window, while RMB camera navigation is off.
Movement uses fixed world axes and does not depend on the camera rotation:

- `W` / `Up` - move along +Z
- `S` / `Down` - move along -Z
- `A` / `Left` - move along -X
- `D` / `Right` - move along +X
- `Space` - jump when grounded

Diagonal input is normalized so it does not increase movement speed.

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
- `assets/scenes/scene.json`

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
- Assets, config and the Python runtime are copied next to each executable by one shared target per output folder (see [docs/build-runtime-assets.md](docs/build-runtime-assets.md), in Russian)

## 12. Repository Map

- `CMakeLists.txt` - root build configuration and external libs wiring
- `engine/CMakeLists.txt` - engine target and source list
- `app/CMakeLists.txt` - executable target
- `engine/include/myengine/...` - engine public headers
- `engine/src/...` - engine implementation
- `app/src/...` - application entry point and states
- `assets/shaders/primitive.hlsl` - shader used by renderer
- `assets/textures/debug.bmp` - sample texture
- `assets/scenes/scene.json` - serialized scene file
- `config/app.json` - runtime config

## 13. Runtime Files

Default executable path:
- `build/app/<Config>/myengine.exe`

Log file:
- `build/app/<Config>/logs/myengine.log`

## 14. Fonts and Icons

Editor UI fonts live in `assets/fonts`:
- `Inter-Regular.ttf`, `Inter-SemiBold.ttf` - Inter (SIL Open Font License 1.1)
- `JetBrainsMono-Regular.ttf` - JetBrains Mono (SIL Open Font License 1.1)
- `exo2/` - Exo 2 (SIL Open Font License 1.1, text in `exo2/OFL.txt`), the font of the game UI: static styles Black Italic, Extra Bold Italic, Bold and Semi Bold cut from the variable fonts of github.com/google/fonts (`ofl/exo2`) by `tools/make_exo2.py`; the variable files are not kept
- `lucide.ttf` - Lucide icons, lucide-static 0.460.0 (ISC License, text in `assets/fonts/LICENSE-lucide.txt`); the icon codes used by the editor are listed in `engine/src/ui/editor/EditorIcons.h` and must be regenerated if the font version changes
