# myengine

A learning DirectX 12 engine skeleton created for ITMO University practical work.

This repository contains:
- `myengine_engine` - static library with reusable engine runtime systems.
- `myengine` - executable application that uses the engine.

## 1. What This Project Demonstrates

- Win32 application lifecycle (`Initialize -> Run -> Shutdown`)
- Main game loop with high-precision `deltaTime`
- State machine (`Loading`, `Menu`, `Gameplay`)
- Logging system (file + debugger output)
- Rendering adapter abstraction (`IRenderAdapter`)
- DirectX 12 backend implementation (`Dx12RenderAdapter`)
- Multi-window rendering from JSON config
- Per-window input-driven primitive transform

## 2. Requirements

- Windows 10/11
- Visual Studio 2022 (Desktop C++)
- CMake 3.24+
- Windows SDK with DirectX 12 headers/libraries

## 3. Quick Start (Recommended)

From repository root:

1. Configure:
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

## 4. Manual CMake Commands

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target myengine
```

Run executable:

```powershell
./build/app/Debug/myengine.exe
```

## 5. Build Output Layout

After a successful Debug build:

- `build/myengine.sln` - generated Visual Studio solution
- `build/app/Debug/myengine.exe` - application executable
- `build/app/Debug/assets/...` - copied runtime assets
- `build/app/Debug/config/app.json` - copied runtime config
- `build/app/Debug/logs/myengine.log` - runtime log file

## 6. Runtime Behavior (How It Works)

### 6.1 Startup flow

1. `app/src/main.cpp` resolves config file path
2. Config is parsed into `AppConfig`
3. `Application::Initialize` starts logger, creates render adapter
4. Windows from `config/app.json` are created
5. One DX12 surface (swap chain) is created per window
6. Initial state is set to `LoadingState`
7. Main loop starts

### 6.2 Main loop

Per frame (`Application::Run`):

1. Process Win32 messages
2. Update timer (`deltaTime`)
3. Apply input to active window primitive
4. Update current game state
5. Render each alive window
6. If all windows are closed, quit

### 6.3 States

- `LoadingState`: short bootstrap stage, then switches to `MenuState`
- `MenuState`: 
  - `Enter` -> switch to `GameplayState`
  - `Esc` -> quit application
- `GameplayState`:
  - `Esc` -> switch back to `MenuState`

State label is reflected in window titles.

### 6.4 Input mapping

Input is applied to the window that currently owns input focus:

- Arrow keys: move primitive
- Left mouse hold: zoom in
- Left mouse double-click + hold second click: continuous zoom out until default scale (`1.0`)
- Right mouse hold: rotate primitive

All transform changes use `deltaTime`.

### 6.5 Rendering path

For each window and each frame:

1. Transition backbuffer `PRESENT -> RENDER_TARGET`
2. Clear with per-window `clearColor` from JSON
3. Draw a colored triangle with transform matrix
4. Transition `RENDER_TARGET -> PRESENT`
5. Present swap chain

## 7. Configuration (`config/app.json`)

The config controls windows and input speeds.

Example schema:

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
- If file is missing or invalid, defaults are used
- If `windows` is empty, default windows are used

## 8. File-by-File Reference

### 8.1 Repository root files

- `CMakeLists.txt` - root CMake entry point
- `setup.bat` - configure helper
- `build.bat` - build helper
- `run.bat` - run helper
- `rebuild.bat` - configure + build helper
- `clean.bat` - remove build directory

### 8.2 `cmake/`

- `cmake/CompilerOptions.cmake` - shared compiler flags and definitions
- `cmake/CopyRuntimeAssets.cmake` - post-build copy of `assets/` and `config/`

### 8.3 `app/`

- `app/CMakeLists.txt` - executable target definition
- `app/src/main.cpp` - application entry point and config path resolution
- `app/src/states/LoadingState.h/.cpp` - loading state implementation
- `app/src/states/MenuState.h/.cpp` - menu state implementation
- `app/src/states/GameplayState.h/.cpp` - gameplay state implementation

### 8.4 `engine/include/myengine/` headers

- `core/Application.h` - main runtime coordinator interface
- `core/Window.h` - Win32 window wrapper interface
- `core/Timer.h` - high precision timer interface
- `core/Logger.h` - logging interface
- `core/Types.h` - shared engine types/events/colors
- `state/IGameState.h` - state contract
- `state/StateMachine.h` - state machine interface
- `input/InputManager.h` - input cache interface
- `render/IRenderAdapter.h` - rendering abstraction
- `render/RenderTypes.h` - render handles and transforms
- `render/dx12/Dx12Context.h` - DX12 object holder structure
- `render/dx12/Dx12RenderAdapter.h` - DX12 renderer interface
- `scene/TestPrimitive.h` - primitive transform model
- `config/AppConfig.h` - app config structures and loader API

### 8.5 `engine/src/` sources

- `core/Application.cpp` - lifecycle, loop, message handling, per-window input application, rendering dispatch
- `core/Window.cpp` - Win32 class registration and window procedure forwarding
- `core/Timer.cpp` - QueryPerformanceCounter-based timing
- `core/Logger.cpp` - timestamped logging to file/debug output
- `state/StateMachine.cpp` - state transitions and event/update/render forwarding
- `input/InputManager.cpp` - key/mouse pressed state tracking
- `config/AppConfig.cpp` - JSON parsing with fallback defaults
- `render/dx12/Dx12RenderAdapter.cpp` - DX12 device/swapchain/pipeline/frame rendering

### 8.6 `engine/third_party/`

- `d3dx12.h` - Microsoft DX12 helper header
- `json.hpp` - nlohmann single-header JSON library

### 8.7 Runtime content

- `assets/shaders/primitive.hlsl` - vertex/pixel shader for test triangle
- `config/app.json` - runtime windows/input configuration

### 8.8 Logging and diagnostics

- `build/app/<Config>/logs/myengine.log` - runtime log path