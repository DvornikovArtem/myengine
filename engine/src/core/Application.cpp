// Application.cpp

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>

#include <myengine/core/AppPaths.h>
#include <myengine/core/Application.h>
#include <myengine/core/FramePacer.h>
#include <imm.h>
#include "UiScript.h"
#include <myengine/core/ServiceLocator.h>
#include <myengine/editor/EditorState.h>
#include <myengine/editor/ThumbnailService.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/MotionComponent.h>
#include <myengine/ecs/components/PlayerControllerComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/ecs/systems/CameraControlSystem.h>
#include <myengine/audio/AudioSystem.h>
#include <myengine/ecs/systems/DebugRenderSystem.h>
#include <myengine/ecs/systems/FxSystem.h>
#include <myengine/fx/ParticleSystem.h>
#include <myengine/ecs/systems/MotionSystem.h>
#include <myengine/ecs/systems/PhysicsSystem.h>
#include <myengine/ecs/systems/PlayerControlSystem.h>
#include <myengine/ecs/systems/RenderSystem.h>
#include <myengine/ecs/systems/RlGameSystem.h>
#include <myengine/game/RlGameFlow.h>
#include <myengine/game/RlGameSettings.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/physics/PhysicsEvents.h>
#include <myengine/render/dx12/Dx12RenderAdapter.h>
#include <myengine/scene/SceneEvents.h>
#include <myengine/scene/SceneSerializer.h>
#include <myengine/scripting/ScriptSystem.h>
#include <myengine/ui/rl/RlUiGallery.h>
#include <myengine/ui/rl/RlUiHost.h>

#include <tracy/Tracy.hpp>

namespace myengine::core
{
    namespace
    {
        // The scene opened without --scene (benchmark.json stays available through --scene for LR1 measurements)
        constexpr char kDefaultScenePath[] = "assets/scenes/coin_guard_demo.json";

        // Buttons that hold the OS mouse capture (bits of Application::mouseCaptureButtons_)
        constexpr std::uint32_t kCaptureLeftButton = 1u << 0;
        constexpr std::uint32_t kCaptureRightButton = 1u << 1;

        std::wstring Utf8ToWide(const std::string& text)
        {
            if (text.empty())
            {
                return std::wstring();
            }

            const int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            if (sizeNeeded <= 0)
            {
                return std::wstring();
            }

            std::wstring result(sizeNeeded - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), sizeNeeded);
            return result;
        }

        // A thumbnail is never worth the editor: an exception from the preview service is logged (once) and the
        // frame goes on, otherwise it leaves the main loop and std::terminate aborts the process
        template <typename Step>
        void RunThumbnailStep(Logger& logger, const char* name, Step&& step)
        {
            static bool reported = false;
            try
            {
                step();
            }
            catch (const std::exception& error)
            {
                if (!reported)
                {
                    reported = true;
                    logger.Error(std::string("ThumbnailService::") + name + " failed: " + error.what());
                }
            }
        }

        std::filesystem::path GetExecutableDirectory()
        {
            wchar_t modulePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            return std::filesystem::path(modulePath).parent_path();
        }

        bool TryBuildEditorRenderRegion(
            const Window& window,
            render::IntRect& outRegion,
            std::uint32_t& outWidth,
            std::uint32_t& outHeight)
        {
            const auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
            if (!editorState.showViewport)
            {
                return false;
            }

            const auto* windowState = editorState.FindWindowState(window.Id());
            if (windowState == nullptr || !windowState->viewport.IsValid())
            {
                return false;
            }

            const int windowWidth = static_cast<int>(window.Width());
            const int windowHeight = static_cast<int>(window.Height());
            const int left = std::clamp(static_cast<int>(std::floor(windowState->viewport.x)), 0, windowWidth);
            const int top = std::clamp(static_cast<int>(std::floor(windowState->viewport.y)), 0, windowHeight);
            const int right = std::clamp(static_cast<int>(std::ceil(windowState->viewport.x + windowState->viewport.width)), 0, windowWidth);
            const int bottom = std::clamp(static_cast<int>(std::ceil(windowState->viewport.y + windowState->viewport.height)), 0, windowHeight);

            if (right <= left || bottom <= top)
            {
                return false;
            }

            outRegion = {left, top, right, bottom};
            outWidth = static_cast<std::uint32_t>(right - left);
            outHeight = static_cast<std::uint32_t>(bottom - top);
            return true;
        }
    }

    Application::Application(HINSTANCE instance) : instance_(instance) {}

    // The result lines of a command-line request: to the console of the parent (a GUI process has none of its own)
    void WriteToConsole(const std::string& text)
    {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (output == nullptr || output == INVALID_HANDLE_VALUE)
        {
            if (AttachConsole(ATTACH_PARENT_PROCESS))
            {
                output = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            }
        }
        if (output != nullptr && output != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
    }

    Application::~Application()
    {
        Shutdown();
    }

    bool Application::Initialize(const config::AppConfig& config, const std::filesystem::path& requestedScene, const bool requestedPlay)
    {
        config_ = config;

        // The scene to start with: the command line first, then "startScene" / "startInPlay" of app.json
        // The game mode (--game or "startInGame" of app.json) plays the Rocket League map with the screens of the game instead of the editor
        gameMode_ = gameMode_ || config.startInGame;
        char* keepIme = nullptr;
        std::size_t keepImeLength = 0;
        const bool keepTextServices = _dupenv_s(&keepIme, &keepImeLength, "MYENGINE_KEEP_IME") == 0 && keepIme != nullptr && *keepIme != '\0';  // for the comparison of the frame times
        std::free(keepIme);
        if (gameMode_ && !keepTextServices)
        {
            // The text services of Windows (the IME and the Text Services Framework) look at every key and mouse message, and on some machines they take 6 - 20 ms for
            // each one (G8: every press of a key was a frame of that length). The game has no composition to take in: the keyboard layout is enough.
            ImmDisableIME(static_cast<DWORD>(-1));
            ImmDisableTextFrameService(0);
        }
        const bool sceneFromConfig = requestedScene.empty() && !config.startScene.empty();
        std::filesystem::path scenePath = sceneFromConfig ? std::filesystem::u8path(config.startScene) : requestedScene;
        if (gameMode_ && scenePath.empty())
        {
            scenePath = "assets/scenes/rocket_league.json";
        }
        const bool startInPlay = requestedPlay || gameMode_ || (sceneFromConfig && config.startInPlay);

        const auto logPath = apppaths::LogDirectory() / "myengine.log";
        if (!logger_.Initialize(logPath))
        {
            return false;
        }

        logger_.Info("Application initialization started");

        if (config_.windows.empty())
        {
            config_ = config::AppConfig::Default();
        }

        logger_.Info("Configured windows count: " + std::to_string(config_.windows.size()));

        // Before any subsystem that may schedule jobs (ResourceManager, PhysicsSystem)
        jobs::Initialize();
        logger_.Info("Job system initialized: " + std::to_string(jobs::GetWorkerCount(jobs::Priority::High)) + " high, " + 
            std::to_string(jobs::GetWorkerCount(jobs::Priority::Streaming)) + " streaming worker(s)");

        // The project decides where scripts, prefabs, maps and assets are looked up. Without --project it is the
        // project of the engine folder (myengine.myproject, or the assets/ layout when the file is missing).
        auto& project = ServiceLocator::GetProjectContext();
        {
            if (!projectFile_.empty())
            {
                std::string projectError;
                if (!project.Load(projectFile_, &projectError))
                {
                    logger_.Error("Failed to open the project: " + projectError);
                    MessageBoxW(nullptr, Utf8ToWide("Failed to open the project:\n" + projectError).c_str(), L"myengine", MB_OK | MB_ICONERROR);
                    return false;
                }

                project::AddRecentProject(project::DefaultRecentProjectsFile(), project.ProjectFile());
            }
            else
            {
                std::string ignored;
                if (!project.Load(ProjectContext::EngineRoot() / kEngineProjectFileName, &ignored))
                {
                    project.InitializeDefault();
                }
            }

            logger_.Info("Project: " + project.Name() + " (" + project.Root().u8string() + ")");
        }

        // Python lives on this (main) thread for the whole session. Scripts are read from the project folder,
        // so hot reload sees the edits; the copy next to the exe is the fallback.
        {
            const auto sourceScriptsDir = project.ScriptsDir();
            std::error_code scriptsDirError;
            scripting::ScriptRuntimeDesc scriptDesc;
            scriptDesc.exeDir = GetExecutableDirectory();
            scriptDesc.scriptsDir = std::filesystem::is_directory(sourceScriptsDir, scriptsDirError)
                ? sourceScriptsDir
                : scriptDesc.exeDir / "assets/scripts";
            scriptDesc.logger = &logger_;
            if (!scriptRuntime_.Initialize(scriptDesc))
            {
                logger_.Warning("Scripting is disabled, the engine continues without scripts");
            }
        }

        // Like scripts, prefabs use the source assets for hot reload, with a packaged copy as the fallback.
        {
            const auto sourcePrefabsDir = project.PrefabsDir();
            std::error_code error;
            const auto prefabsDir = std::filesystem::is_directory(sourcePrefabsDir, error)
                ? sourcePrefabsDir : GetExecutableDirectory() / "assets/prefabs";
            if (!prefabLibrary_.Initialize(prefabsDir, &logger_))
            {
                logger_.Warning("Prefab spawning is disabled, the engine continues without prefabs");
            }
        }

        // GPU capture tools hook the API when their DLL is in the process: before the device is created
        {
            const auto envEnabled = [](const char* name)
            {
                char* value = nullptr;
                std::size_t length = 0;
                const bool set = _dupenv_s(&value, &length, name) == 0 && value != nullptr && value[0] != '\0' && value[0] != '0';
                std::free(value);
                return set;
            };

            const bool wantsCapture = diagnostics_.captureFrame != 0;
            if (diagnostics_.renderDoc || wantsCapture || envEnabled("MYENGINE_RENDERDOC"))
            {
                std::string message;
                const bool loaded = gpuCapture_.LoadRenderDoc(message);
                if (loaded)
                {
                    logger_.Info("GPU capture: " + message);
                }
                else
                {
                    logger_.Warning("GPU capture: " + message);
                }
                if (wantsCapture && !loaded)
                {
                    const std::string text = "--gpu-capture-frame needs RenderDoc: " + message;
                    logger_.Error(text);
                    WriteToConsole(text + "\n");
                    exitCode_ = 3;
                    return false;
                }
            }
            if (diagnostics_.pix || envEnabled("MYENGINE_PIX"))
            {
                std::string message;
                if (gpuCapture_.LoadPix(message))
                {
                    logger_.Info("GPU capture: " + message);
                }
                else
                {
                    logger_.Warning("GPU capture: " + message);
                }
            }
        }

        renderAdapter_ = std::make_unique<render::dx12::Dx12RenderAdapter>(logger_);
        if (!renderAdapter_->Initialize())
        {
            logger_.Error("Render adapter initialization failed");
            return false;
        }

        resourceManager_ = std::make_unique<resource::ResourceManager>(*renderAdapter_, logger_);
        resourceManager_->SetHotReloadAsync(true);  // the scan for changed files does not hold the frame

        {
            // Asset previews: images are cached under <project>/Saved/Thumbnails (%LOCALAPPDATA%\RocketArena\Saved in shipping)
            editor::ThumbnailServiceConfig thumbnailConfig;
            if (const auto& projectContext = ServiceLocator::GetProjectContext(); projectContext.IsInitialized())
            {
                thumbnailConfig.cacheDirectory = apppaths::SavedDirectory(projectContext.Root()) / "Thumbnails";
            }
            thumbnailService_ = std::make_unique<editor::ThumbnailService>(*renderAdapter_, *resourceManager_, logger_, std::move(thumbnailConfig));
        }

        WindowId nextWindowId = 1;
        windows_.reserve(config_.windows.size());

        for (const auto& windowConfig : config_.windows)
        {
            WindowDesc desc;
            desc.id = nextWindowId++;
            desc.title = Utf8ToWide(windowConfig.title);
            desc.width = windowConfig.width;
            desc.height = windowConfig.height;

            auto window = std::make_unique<Window>(desc);
            if (!window->Create(this))
            {
                logger_.Error("Failed to create a window");
                return false;
            }

            auto surface = renderAdapter_->CreateSurface(window->Handle(), windowConfig.width, windowConfig.height);
            if (!surface.IsValid())
            {
                logger_.Error("Failed to create DX12 surface for a window");
                return false;
            }

            WindowRuntime runtime;
            runtime.window = std::move(window);
            runtime.baseTitle = desc.title;
            runtime.surface = surface;
            runtime.clearColor = windowConfig.clearColor;

            runtime.window->Show();
            windows_.push_back(std::move(runtime));
        }

        ConfigureInputBindings();

        ui::SceneEditorServices sceneEditorServices;
        sceneEditorServices.world = &world_;
        sceneEditorServices.resourceManager = resourceManager_.get();
        sceneEditorServices.thumbnails = thumbnailService_.get();
        sceneEditorServices.logger = &logger_;
        sceneEditorServices.prefabLibrary = &prefabLibrary_;
        sceneEditorServices.requestQuit = [this]() { RequestQuit(); };
        sceneEditorServices.saveScene = [this]() { return SaveSceneToDisk(); };
        sceneEditorServices.loadScene = [this](std::function<void(bool)> done) { BeginRevertScene(std::move(done)); };
        sceneEditorServices.openScene = [this](const std::string& scenePath, std::function<void(bool)> done)
        {
            BeginOpenScene(scenePath, std::move(done));
        };
        sceneEditorServices.loading = &loadingService_;
        sceneEditorServices.saveSceneAs = [this](const std::string& scenePath) { return SaveSceneAs(scenePath); };
        sceneEditorServices.restartWithProject = [this](const std::string& projectFile, const bool discardScene)
        {
            return RestartWithProject(std::filesystem::u8path(projectFile), discardScene);
        };
        sceneEditorServices.captureSceneSnapshot = [this]() { return CaptureSceneSnapshot(); };
        sceneEditorServices.restoreSceneSnapshot = [this](std::string_view snapshot) { return RestoreSceneSnapshot(snapshot); };
        sceneEditorServices.isVSyncEnabled = [this]() { return renderAdapter_ != nullptr && renderAdapter_->IsVSyncEnabled(); };
        sceneEditorServices.setVSync = [this](const bool enabled)
        {
            if (renderAdapter_ != nullptr)
            {
                renderAdapter_->SetVSync(enabled);
            }
        };
        sceneEditorServices.setWireframe = [this](const bool enabled)
        {
            if (renderAdapter_ != nullptr)
            {
                renderAdapter_->SetWireframe(enabled);
            }
        };
        sceneEditorServices.shaderStatus = [this]()
        {
            ui::ShaderStatusView view;
            const auto ansiToUtf8 = [](const std::string& text)
            {
                if (text.empty())
                {
                    return text;
                }
                const int wideLength = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
                if (wideLength <= 0)
                {
                    return text;
                }
                std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
                MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), wideLength);
                const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
                std::string utf8(static_cast<std::size_t>(std::max(utf8Length, 0)), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, utf8.data(), utf8Length, nullptr, nullptr);
                return utf8;
            };
            if (renderAdapter_ == nullptr)
            {
                return view;
            }
            const render::ShaderStatistics statistics = renderAdapter_->GetShaderStatistics();
            view.available = true;
            view.programCount = statistics.programs;
            view.pipelineStates = statistics.pipelineStates;
            view.rootSignatures = statistics.rootSignatures;
            view.reloads = statistics.reloads;
            view.reloadFailures = statistics.reloadFailures;
            view.summary = "programs " + std::to_string(statistics.programs) + " | PSO " + std::to_string(statistics.pipelineStates) +
                " | root signatures " + std::to_string(statistics.rootSignatures) + " | waiting for the GPU " + std::to_string(statistics.retiredObjects) +
                " | reloads " + std::to_string(statistics.reloads) + " ok / " + std::to_string(statistics.reloadFailures) + " failed";
            for (const render::ShaderProgramInfo& program : renderAdapter_->GetShaderPrograms())
            {
                // The renderer's names are in the ANSI code page (they come from the paths of the compiler); the editor shows UTF-8
                view.programs.push_back({std::filesystem::path(program.name).filename().u8string(), program.version, ansiToUtf8(program.lastError)});
            }
            return view;
        };
        sceneEditorServices.captureGpuFrame = [this](std::string& message) { return RequestGpuCapture(message); };
        sceneEditorServices.reloadScripts = [this]()
        {
            if (scriptSystem_ != nullptr)
            {
                scriptSystem_->RequestReloadAll();
            }
        };
        sceneEditorServices.describeScriptFields = [this](const std::string& module, const std::string& className)
        {
            return scriptRuntime_.DescribeFields(module, className);
        };
        sceneEditorServices.scriptStatus = [this](const ecs::EntityId entity, const std::size_t scriptIndex)
        {
            return scriptSystem_ != nullptr ? scriptSystem_->GetInstanceStatus(entity, scriptIndex) : std::string();
        };
        sceneEditorServices.liveScriptFields = [this](const ecs::EntityId entity, const std::size_t scriptIndex)
        {
            return scriptSystem_ != nullptr ? scriptSystem_->GetLiveFields(entity, scriptIndex) : nlohmann::json::object();
        };
        sceneEditorServices.scriptHudLines = [this]()
        {
            std::vector<std::string> lines;
            if (scriptSystem_ != nullptr)
            {
                for (const auto& line : scriptSystem_->GetHudLines())
                {
                    lines.push_back(line.second);
                }
            }
            if (rlGameSystem_ != nullptr)
            {
                for (auto& line : rlGameSystem_->HudLines())
                {
                    lines.push_back(std::move(line));
                }
            }
            return lines;
        };
        sceneEditorServices.rlTuning = [this]() { return rlGameSystem_ != nullptr ? rlGameSystem_->EditableTuning() : nullptr; };
        sceneEditorServices.rlTuningEdited = [this]()
        {
            if (rlGameSystem_ != nullptr)
            {
                rlGameSystem_->TuningEdited();
            }
        };
        sceneEditorServices.rlTuningSave = [this]() { return rlGameSystem_ != nullptr ? rlGameSystem_->SaveTuning() : std::string(); };
        sceneEditorServices.rlTuningReload = [this]() { return rlGameSystem_ != nullptr ? rlGameSystem_->ReloadTuning() : std::string(); };
        sceneEditorServices.scriptConsole.execute = [this](const std::string& source)
        {
            return scriptSystem_ != nullptr ? scriptSystem_->ExecuteConsole(source) : scripting::ScriptConsoleResult{};
        };
        sceneEditorServices.scriptConsole.reset = [this]() { scriptRuntime_.ResetConsole(); };
        sceneEditorServices.scriptConsole.generation = [this]() { return scriptRuntime_.GetConsoleGeneration(); };
        sceneEditorServices.scriptConsole.errors = [this]()
        {
            if (scriptSystem_ == nullptr)
            {
                return std::vector<scripting::ScriptError>{};
            }
            const auto& errors = scriptSystem_->GetRecentErrors();
            return std::vector<scripting::ScriptError>(errors.begin(), errors.end());
        };
        sceneEditorServices.scriptConsole.clearErrors = [this]()
        {
            if (scriptSystem_ != nullptr)
            {
                scriptSystem_->ClearRecentErrors();
            }
        };
        if (!uiManager_.Initialize(*renderAdapter_, logger_, std::move(sceneEditorServices)))
        {
            logger_.Error("UI manager initialization failed");
            return false;
        }

        // the Rocket League game (G3, G4): does nothing unless the scene has an RlArena; it is added to the world below
        auto rlGame = std::make_unique<ecs::systems::RlGameSystem>(input_);
        rlGame->SetOnlineOptions(rlOnline_);
        rlGame->SetGameMode(gameMode_);
        rlGameSystem_ = rlGame.get();

        // The game mode: the screens of the game (menus, HUD) instead of the editor
        if (gameMode_)
        {
            game::RlGameFlowServices services;
            services.system = rlGameSystem_;
            services.ui = &uiManager_;
            services.quit = [this]() { RequestQuit(); };
            services.applyVideo = [this](const game::RlVideoSettings& video) { ApplyVideoSettings(video); };
            services.resolutions = [this]() { return ListDisplayResolutions(); };
            services.windowSize = [this]() { return GetWindowSize(); };
            services.sceneReady = [this]() { return sceneLoaded_; };
            services.loadingProgress = [this]()
            {
                const auto operations = loadingService_.Visible();
                return operations.empty() ? -1.0f : operations.front().progress;
            };
            services.fps = [this]() { return averagedFps_; };
            services.log = [this](const std::string& text) { logger_.Info(text); };
            services.setLighting = [this](const bool menu)
            {
                if (renderAdapter_ == nullptr)
                {
                    return;
                }
                render::SceneLighting lighting;  // the defaults are the light of the field
                if (menu)
                {
                    // the night behind the menus: the field is dark, the car on the plinth stands in the light of the sun
                    lighting.color = {0.95f, 0.98f, 1.1f};
                    lighting.skyAmbient = {0.12f, 0.14f, 0.23f};
                    lighting.groundAmbient = {0.04f, 0.05f, 0.08f};
                }
                renderAdapter_->SetSceneLighting(lighting);
            };
            // MYENGINE_GAME_SETTINGS=<file>: another settings file (the tests)
            {
                char* settingsPath = nullptr;
                std::size_t settingsPathLength = 0;
                if (_dupenv_s(&settingsPath, &settingsPathLength, "MYENGINE_GAME_SETTINGS") == 0 && settingsPath != nullptr && *settingsPath != 0)
                {
                    services.settingsFile = std::filesystem::path(settingsPath);
                }
                std::free(settingsPath);
            }
            gameFlow_ = std::make_unique<game::RlGameFlow>(services);
            gameFlow_->Install();

            // MYENGINE_UI_SCRIPT=<file>: timed keys, clicks and screenshots instead of a person (docs/game/rl-ui.md)
            char* scriptPath = nullptr;
            std::size_t scriptPathLength = 0;
            if (_dupenv_s(&scriptPath, &scriptPathLength, "MYENGINE_UI_SCRIPT") == 0 && scriptPath != nullptr && *scriptPath != 0)
            {
                std::ifstream scriptFile(std::filesystem::path(scriptPath), std::ios::binary);
                std::stringstream scriptText;
                scriptText << scriptFile.rdbuf();
                std::string scriptError;
                auto script = std::make_unique<UiScript>();
                if (!scriptFile || !script->Load(scriptText.str(), scriptError))
                {
                    logger_.Error(std::string("MYENGINE_UI_SCRIPT: ") + (scriptFile ? scriptError : std::string("cannot read ") + scriptPath));
                }
                else
                {
                    uiScript_ = std::move(script);
                    UiScript* raw = uiScript_.get();
                    gameFlow_->SetRawInputHook([raw](rlui::RawInput& input) { raw->ApplyTo(input); });
                    logger_.Info(std::string("UI script: ") + scriptPath);
                }
            }
            std::free(scriptPath);
        }

        // MYENGINE_UI_GALLERY=<screen> or "all": the gallery of the UI kit of the game instead of the editor (docs/game/rl-ui.md)
        if (!gameMode_)
        {
            char* gallery = nullptr;
            std::size_t galleryLength = 0;
            _dupenv_s(&gallery, &galleryLength, "MYENGINE_UI_GALLERY");
            if (gallery != nullptr && *gallery != '\0')
            {
                struct Cycle
                {
                    rlui::GalleryScreen screen = rlui::GalleryScreen::Main;
                    bool all = false;
                    double clock = 0.0;
                    int shown = -1;
                };
                auto cycle = std::make_shared<Cycle>();
                cycle->all = std::string(gallery) == "all";
                if (!cycle->all && !rlui::ParseGalleryScreen(gallery, cycle->screen))
                {
                    logger_.Warning(std::string("MYENGINE_UI_GALLERY: unknown screen '") + gallery + "', showing main");
                }
                uiGallery_ = std::make_unique<rlui::Host>();
                uiGallery_->Install(uiManager_, {});
                uiGallery_->SetScreen([cycle](rlui::Frame& frame)
                {
                    cycle->clock += static_cast<double>(frame.DeltaTime());
                    if (cycle->all)
                    {
                        cycle->screen = static_cast<rlui::GalleryScreen>(static_cast<int>(cycle->clock / 4.0) % static_cast<int>(rlui::GalleryScreen::Count));
                    }
                    if (cycle->shown != static_cast<int>(cycle->screen))
                    {
                        cycle->shown = static_cast<int>(cycle->screen);
                        frame.ResetFocus();
                    }
                    rlui::DrawGalleryScreen(frame, cycle->screen);
                });
                logger_.Info(std::string("UI gallery: ") + gallery);
            }
            std::free(gallery);
        }

        for (const auto& runtime : windows_)
        {
            if (runtime.window != nullptr && runtime.surface.IsValid())
            {
                uiManager_.RegisterWindow(
                    runtime.window->Id(),
                    runtime.window->Handle(),
                    runtime.surface,
                    runtime.window->Width(),
                    runtime.window->Height());
            }
        }

        world_.AddUpdateSystem(std::make_unique<ecs::systems::CameraControlSystem>(input_, inputOwnerWindowId_));
        world_.AddUpdateSystem(std::make_unique<ecs::systems::PlayerControlSystem>(input_));
        world_.AddUpdateSystem(std::move(rlGame));
        world_.AddUpdateSystem(std::make_unique<ecs::systems::MotionSystem>());
        world_.AddUpdateSystem(std::make_unique<ecs::systems::PhysicsSystem>());
        // After physics: scripts see this frame's collision/trigger events
        {
            auto scriptSystem = std::make_unique<scripting::ScriptSystem>(scriptRuntime_, input_, prefabLibrary_, logger_);
            scriptSystem_ = scriptSystem.get();
            world_.AddUpdateSystem(std::move(scriptSystem));
        }
        // Sound and particles (G7c): the device and the sound bank, the presets of the effects; the systems play the components of the scene
        {
            auto& audioSystem = audio::GetAudioSystem();
            audio::AudioConfig audioConfig;
            {
                char* off = nullptr;
                std::size_t offLength = 0;
                if (_dupenv_s(&off, &offLength, "MYENGINE_AUDIO") == 0 && off != nullptr)
                {
                    audioConfig.useDevice = std::string(off) != "0"; // MYENGINE_AUDIO=0: no device (machines without sound, the capture of a frame)
                    std::free(off);
                }
            }
            audioSystem.SetLogger(&logger_);
            audioSystem.Initialize(audioConfig);
            audioSystem.LoadBank(ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/audio_bank.json"));
            std::vector<std::string> fxMessages;
            const std::uint32_t effects = fx::GetParticleSystem().LoadEffects(ServiceLocator::GetProjectContext().ResolveContentPath("assets/data/fx"), &fxMessages);
            logger_.Info("Particle effects loaded: " + std::to_string(effects));
            for (const std::string& message : fxMessages)
            {
                logger_.Warning("Particle effect: " + message);
            }
        }
        world_.AddUpdateSystem(std::make_unique<ecs::systems::FxUpdateSystem>());
        world_.AddRenderSystem(std::make_unique<ecs::systems::RenderSystem>());
        world_.AddRenderSystem(std::make_unique<ecs::systems::FxRenderSystem>());
        world_.AddRenderSystem(std::make_unique<ecs::systems::DebugRenderSystem>());
        // --scene is a file path or a path inside the project; without it the project's default map is opened
        std::error_code scenePathError;
        if (scenePath.empty())
        {
            sceneSavePath_ = project.DefaultMap();
        }
        else if (scenePath.is_absolute() || std::filesystem::exists(scenePath, scenePathError))
        {
            sceneSavePath_ = std::filesystem::absolute(scenePath).lexically_normal();
        }
        else
        {
            sceneSavePath_ = project.ResolveContentPath(scenePath);
        }

        gpuCapture_.SetOutput(apppaths::SavedDirectory(project.Root()) / "Captures", sceneSavePath_.stem().u8string());

        BindRuntimeEventListeners();

        if (resourceManager_ != nullptr)
        {
            const auto manifestPath = project.ContentDir() / "manifests/demo_assets.json";
            std::error_code manifestError;
            if (std::filesystem::exists(manifestPath, manifestError))
            {
                resourceManager_->LoadManifest(manifestPath);
            }
        }

        // What follows the start map: the state of the editor and the mode
        const auto finishStartup = [this, startInPlay]()
        {
            auto& startProject = core::ServiceLocator::GetProjectContext();
            sceneLoaded_ = true;
            auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
            editorState.selectedEntity = ecs::kInvalidEntity;
            editorState.mapPath = startProject.ToProjectRelative(sceneSavePath_);
            editorState.showProjectBrowser = showProjectBrowser_;
            if (startInPlay)
            {
                editorState.playModeSnapshot = CaptureSceneSnapshot();
                editorState.mode = editor::RuntimeMode::Play;
                core::ServiceLocator::GetPhysicsWorldState().physicsPaused = false;
            }
            else
            {
                editorState.playModeSnapshot.clear();
                editorState.mode = editor::RuntimeMode::Edit;
                core::ServiceLocator::GetPhysicsWorldState().physicsPaused = true;
            }

            timer_.Reset();
            logger_.Info("Application initialization finished");
        };

        // The map file is there: read it in the background and show the splash meanwhile. Without a file (a project
        // without a map yet) the demo scene is built at once, as always.
        std::error_code mapExistsError;
        if (deferredSceneLoad_ && std::filesystem::exists(sceneSavePath_, mapExistsError))
        {
            const bool explicitScene = !scenePath.empty();
            const std::string subtitle = "Opening project " + project.Name();
            StartSceneLoad(sceneSavePath_, true, "myengine", subtitle, editor::LoadingMode::Splash,
                [this, explicitScene, finishStartup](const bool loaded)
                {
                    if (!loaded)
                    {
                        // A typo or invalid explicit scene must not overwrite the file with the default demo.
                        if (explicitScene)
                        {
                            logger_.Error("Failed to load the requested scene: " + sceneSavePath_.u8string());
                            sceneSavePath_.clear();
                            RequestQuit();
                            return;
                        }
                        BuildDemoScene();
                        scene::SaveWorldToJson(world_, sceneSavePath_, &logger_);
                        core::ServiceLocator::GetEventBus().Publish(scene::SceneLoadedEvent{&world_});
                    }
                    finishStartup();
                });
            timer_.Reset();
            logger_.Info("Application initialization finished (the map loads in the background)");
            return true;
        }

        if (!scene::LoadWorldFromJson(world_, sceneSavePath_, &logger_))
        {
            // A typo or invalid explicit scene must not overwrite the file with the default demo.
            if (!scenePath.empty())
            {
                logger_.Error("Failed to load the requested scene: " + sceneSavePath_.u8string());
                sceneSavePath_.clear();
                return false;
            }
            BuildDemoScene();
            scene::SaveWorldToJson(world_, sceneSavePath_, &logger_);
            core::ServiceLocator::GetEventBus().Publish(scene::SceneLoadedEvent{&world_});
        }
        else
        {
            RebindWindowControlledEntities();
        }
        finishStartup();
        return true;
    }

    int Application::Run()
    {
        MSG msg{};
        struct FrameTimingStats
        {
            double worldUpdateMs = 0.0;
            double scriptsMs = 0.0;
            double stateUpdateMs = 0.0;
            double hotReloadMs = 0.0;
            double uiUpdateMs = 0.0;
            double renderMs = 0.0;
            double totalMs = 0.0;
            std::uint32_t frames = 0;
        } frameTimingStats;


        logger_.Info("Main loop started");
        tracy::SetThreadName("Main");
        FramePacer framePacer;

        while (!quitRequested_)
        {
            ZoneScopedN("Frame");

            input_.BeginFrame();

            {
                ZoneScopedN("Frame::Messages");
                for (;;)
                {
                    const auto peekStart = std::chrono::steady_clock::now();
                    const bool have = PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE;
                    const auto peekEnd = std::chrono::steady_clock::now();
                    if (!have)
                    {
                        // the call that finds nothing can take long as well (the messages that are sent, not posted, are handled inside it)
                        const double peekMs = std::chrono::duration<double, std::milli>(peekEnd - peekStart).count();
                        if (peekMs > 6.0 && slowMessageLogs_ < 40)
                        {
                            ++slowMessageLogs_;
                            logger_.Warning("PeekMessage took " + std::to_string(peekMs) + " ms without a message for the loop");
                        }
                        break;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                    const auto dispatchEnd = std::chrono::steady_clock::now();
                    const double dispatchMs = std::chrono::duration<double, std::milli>(dispatchEnd - peekEnd).count();
                    const double peekMs = std::chrono::duration<double, std::milli>(peekEnd - peekStart).count();
                    if ((dispatchMs > 6.0 || peekMs > 6.0) && slowMessageLogs_ < 40)
                    {
                        ++slowMessageLogs_;
                        logger_.Warning("Slow window message 0x" + [&] { char text[16]; std::snprintf(text, sizeof(text), "%04x", static_cast<unsigned>(msg.message)); return std::string(text); }() +
                                        ": PeekMessage " + std::to_string(peekMs) + " ms, DispatchMessage " + std::to_string(dispatchMs) + " ms");
                    }

                    if (msg.message == WM_QUIT)
                    {
                        quitRequested_ = true;
                    }
                }
            }

            if (quitRequested_)
            {
                break;
            }

            if (restartPending_ && std::chrono::steady_clock::now() >= restartQuitTime_)
            {
                restartPending_ = false;
                RequestQuit();
                PostQuitMessage(0);
            }

            // A map that was read on a worker is applied here, between frames
            {
                ZoneScopedN("Frame::Background");
                PumpPendingSceneLoad();
                SyncBackgroundOperations();
                loadingService_.Update();
            }

            timer_.Tick();
            const float deltaTime = timer_.DeltaTime();
            const auto frameStartTime = std::chrono::steady_clock::now();

            if (uiScript_ != nullptr)
            {
                UiScript::Sink sink;
                const WindowId scriptWindow = windows_.empty() || windows_.front().window == nullptr ? 0 : windows_.front().window->Id();
                sink.key = [this, scriptWindow](const int code, const bool down)
                {
                    input_.SetActiveWindow(scriptWindow);
                    down ? input_.OnKeyDown(static_cast<std::uint32_t>(code)) : input_.OnKeyUp(static_cast<std::uint32_t>(code));
                };
                sink.mouseButton = [this, scriptWindow](const int button, const bool down)
                {
                    input_.SetActiveWindow(scriptWindow);
                    const MouseButton which = button == 1 ? MouseButton::Right : MouseButton::Left;
                    down ? input_.OnMouseDown(which) : input_.OnMouseUp(which);
                };
                sink.screenshot = [this](const std::string& path)
                {
                    if (renderAdapter_ != nullptr)
                    {
                        renderAdapter_->RequestScreenshot(path);
                    }
                };
                sink.note = [this](const std::string& text) { logger_.Info("UI script: " + text); };
                sink.game = [this](const std::string& command)
                {
                    if (rlGameSystem_ != nullptr)
                    {
                        rlGameSystem_->DebugCommand(command);
                    }
                };
                sink.quit = [this]() { RequestQuit(); };
                uiScript_->Advance(static_cast<double>(deltaTime), sink);
            }

            const auto worldUpdateStartTime = std::chrono::steady_clock::now();
            {
                ZoneScopedN("World::UpdateSystems");
                world_.UpdateSystems(deltaTime);
            }
            const auto worldUpdateEndTime = std::chrono::steady_clock::now();
            const double scriptsMs = core::ServiceLocator::GetEditorRuntimeState().scriptStats.updateMs;


            const auto stateUpdateStartTime = std::chrono::steady_clock::now();
            {
                ZoneScopedN("StateMachine::Update");
                stateMachine_.Update(*this, deltaTime);
            }
            const auto stateUpdateEndTime = std::chrono::steady_clock::now();
            if ((core::ServiceLocator::GetEditorRuntimeState().mode == editor::RuntimeMode::Play) != titleShowsPlay_)
            {
                UpdateWindowTitles();
            }
            

            const auto hotReloadStartTime = std::chrono::steady_clock::now();
            {
                ZoneScopedN("ResourceManager::UpdateHotReload");
                if (resourceManager_ != nullptr)
                {
                    resourceManager_->UpdateHotReload();
                }
                if (thumbnailService_ != nullptr)
                {
                    RunThumbnailStep(logger_, "Update", [&] { thumbnailService_->Update(); });
                }
            }
            const auto hotReloadEndTime = std::chrono::steady_clock::now();

            
            const auto uiUpdateStartTime = std::chrono::steady_clock::now();
            {
                ZoneScopedN("UiManager::Update");
                uiManager_.Update(deltaTime);
            }
            const auto uiUpdateEndTime = std::chrono::steady_clock::now();

            
            const auto renderStartTime = std::chrono::steady_clock::now();
            {
                ZoneScopedN("RenderFrame");
                RenderFrame();
            }
            const auto renderEndTime = std::chrono::steady_clock::now();
            ++renderedFrames_;
            HandleDiagnosticsAfterFrame();


            const auto millisecondsBetween =
                [](const std::chrono::steady_clock::time_point start, const std::chrono::steady_clock::time_point end)
                {
                    return std::chrono::duration<double, std::milli>(end - start).count();
                };

            frameTimingStats.worldUpdateMs += millisecondsBetween(worldUpdateStartTime, worldUpdateEndTime);
            frameTimingStats.scriptsMs += scriptsMs;
            frameTimingStats.stateUpdateMs += millisecondsBetween(stateUpdateStartTime, stateUpdateEndTime);
            frameTimingStats.hotReloadMs += millisecondsBetween(hotReloadStartTime, hotReloadEndTime);
            frameTimingStats.uiUpdateMs += millisecondsBetween(uiUpdateStartTime, uiUpdateEndTime);
            frameTimingStats.renderMs += millisecondsBetween(renderStartTime, renderEndTime);
            frameTimingStats.totalMs += millisecondsBetween(frameStartTime, renderEndTime);
            frameTimingStats.frames += 1;

            PublishFrameStatistics(
                deltaTime,
                millisecondsBetween(worldUpdateStartTime, worldUpdateEndTime),
                scriptsMs,
                millisecondsBetween(stateUpdateStartTime, stateUpdateEndTime),
                millisecondsBetween(hotReloadStartTime, hotReloadEndTime),
                millisecondsBetween(uiUpdateStartTime, uiUpdateEndTime),
                millisecondsBetween(renderStartTime, renderEndTime),
                millisecondsBetween(frameStartTime, renderEndTime));

            deltaLogAccumulator_ += deltaTime;
            if (deltaLogAccumulator_ >= 1.0f && frameTimingStats.frames > 0)
            {
                const double inverseFrameCount = 1.0 / static_cast<double>(frameTimingStats.frames);
                std::ostringstream timingMessage;
                timingMessage.setf(std::ios::fixed);
                timingMessage.precision(2);
                timingMessage
                    << "FrameTimes avg_ms total=" << frameTimingStats.totalMs * inverseFrameCount
                    << " world=" << frameTimingStats.worldUpdateMs * inverseFrameCount
                    << " scripts=" << frameTimingStats.scriptsMs * inverseFrameCount
                    << " state=" << frameTimingStats.stateUpdateMs * inverseFrameCount
                    << " hot_reload=" << frameTimingStats.hotReloadMs * inverseFrameCount
                    << " ui=" << frameTimingStats.uiUpdateMs * inverseFrameCount
                    << " render=" << frameTimingStats.renderMs * inverseFrameCount
                    << " | deltaTime=" << deltaTime;
                logger_.Debug(timingMessage.str());

                deltaLogAccumulator_ = 0.0f;
                frameTimingStats = {};
            }

            // the limit of the frames per second of the video settings
            framePacer.Wait(fpsLimit_);

            // a running match hides the cursor (only while this application is the one in front)
            if (gameMode_ && !cameraControlActive_)
            {
                const bool hide = core::ServiceLocator::GetEditorRuntimeState().gameHidesCursor && GetForegroundWindow() != nullptr &&
                                  !windows_.empty() && windows_.front().window != nullptr && GetForegroundWindow() == windows_.front().window->Handle();
                SetCursorVisible(!hide);
            }

            if (exitAfterFrames_ != 0 && ++framesRun_ >= exitAfterFrames_)
            {
                logger_.Info("Exit after " + std::to_string(framesRun_) + " frames");
                RequestQuit();
                PostQuitMessage(0);
            }

            if (AllWindowsClosed())
            {
                RequestQuit();
                PostQuitMessage(0);
            }

            FrameMark;
        }

        logger_.Info("Main loop finished");
        // WM_QUIT carries the code of PostQuitMessage; a loop that ended by RequestQuit has no message to take it from
        if (exitCode_ != 0)
        {
            return exitCode_;
        }
        return msg.message == WM_QUIT ? static_cast<int>(msg.wParam) : 0;
    }

    void Application::Shutdown()
    {
        cameraControlActive_ = false;
        SetCursorVisible(true);

        // A map that is still being read on a worker must finish before anything it may touch goes away
        WaitPendingSceneLoad();

        // 1. Release every Python object while the interpreter is still alive
        if (scriptSystem_ != nullptr)
        {
            scriptSystem_->Shutdown();
        }

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.mode == editor::RuntimeMode::Play && !editorState.playModeSnapshot.empty())
        {
            RestoreSceneSnapshot(editorState.playModeSnapshot);
            editorState.mode = editor::RuntimeMode::Edit;
            editorState.playModeSnapshot.clear();
            core::ServiceLocator::GetPhysicsWorldState().physicsPaused = true;
        }

        // The maps of a shipping build are never written: they lie next to the exe, where nothing may be changed
        if (sceneLoaded_ && !sceneSavePath_.empty() && !discardSceneOnExit_ && !apppaths::IsShipping() && !gameMode_)
        {
            scene::SaveWorldToJson(world_, sceneSavePath_, &logger_);
        }

        gameFlow_.reset();    // before the UiManager it is hooked into (it saves the settings)
        uiGallery_.reset();
        uiManager_.Shutdown();
        audio::GetAudioSystem().Shutdown();
        thumbnailService_.reset();
        resourceManager_.reset();

        // 2. Stop the prefab watcher while the Streaming workers still exist.
        prefabLibrary_.Shutdown();

        // 3. Py_Finalize: after the scene is saved, before the job system stops. Safe to call twice
        scriptRuntime_.Shutdown();

        // After ResourceManager: it waits for its loading jobs in the destructor
        jobs::Shutdown();

        if (renderAdapter_)
        {
            logger_.Info("Shutting down render adapter");
            renderAdapter_->Shutdown();
            renderAdapter_.reset();
        }

        windows_.clear();
        scriptSystem_ = nullptr;
        world_ = ecs::World{};
        sceneLoaded_ = false;
    }

    void Application::RequestQuit()
    {
        quitRequested_ = true;
    }

    void Application::SetDiagnosticsOptions(const DiagnosticsOptions& options)
    {
        diagnostics_ = options;
    }

    void Application::SetRlOnlineOptions(const ecs::systems::RlOnlineOptions& options)
    {
        rlOnline_ = options;
    }

    void Application::SetGameMode(const bool enabled)
    {
        gameMode_ = enabled;
    }

    bool Application::GameMode() const
    {
        return gameMode_;
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> Application::ListDisplayResolutions() const
    {
        if (windows_.empty() || windows_.front().window == nullptr)
        {
            return {};
        }
        return windows_.front().window->ListResolutions();
    }

    std::pair<std::uint32_t, std::uint32_t> Application::GetWindowSize() const
    {
        if (windows_.empty() || windows_.front().window == nullptr)
        {
            return {0, 0};
        }
        return {windows_.front().window->Width(), windows_.front().window->Height()};
    }

    void Application::ApplyVideoSettings(const game::RlVideoSettings& video)
    {
        if (renderAdapter_ != nullptr)
        {
            renderAdapter_->SetVSync(video.vsync);
        }
        fpsLimit_ = std::max(video.fpsLimit, 0);

        // The quality of the passes: the command line (--render-settings) decides when it was given
        char* fromCommandLine = nullptr;
        std::size_t fromCommandLineLength = 0;
        const bool quality = !(_dupenv_s(&fromCommandLine, &fromCommandLineLength, "MYENGINE_RENDER_SETTINGS") == 0 && fromCommandLine != nullptr && *fromCommandLine != '\0');
        std::free(fromCommandLine);
        if (quality && renderAdapter_ != nullptr)
        {
            render::RenderSettings settings = renderAdapter_->GetRenderSettings();
            settings.shadowQuality = static_cast<std::uint8_t>(std::clamp(video.shadows, 0, 2));
            settings.msaaSamples = video.msaa >= 4 ? 4 : 1;
            settings.bloom = video.bloom;
            renderAdapter_->SetRenderSettings(settings);
        }

        if (!windows_.empty() && windows_.front().window != nullptr)
        {
            Window& window = *windows_.front().window;
            const auto mode = static_cast<DisplayMode>(std::clamp(video.windowMode, 0, 2));
            const bool sameSize = lastVideoWidth_ == video.width && lastVideoHeight_ == video.height;
            if (!videoApplied_ || mode != window.GetDisplayMode() || (mode != DisplayMode::Borderless && !sameSize))
            {
                window.SetDisplayMode(mode, static_cast<std::uint32_t>(video.width), static_cast<std::uint32_t>(video.height));
                lastVideoWidth_ = video.width;
                lastVideoHeight_ = video.height;
                videoApplied_ = true;
            }
        }
    }

    int Application::GetExitCode() const
    {
        return exitCode_;
    }

    bool Application::RequestGpuCapture(std::string& message)
    {
        if (!gpuCapture_.IsRenderDocAvailable())
        {
            // F12 and the menu may be used in an editor that was started without RenderDoc: try to find it now. The
            // device exists already, so RenderDoc can not hook it: the capture needs a restart.
            std::string loadMessage;
            if (gpuCapture_.LoadRenderDoc(loadMessage))
            {
                message = "RenderDoc was found (" + loadMessage + ") but it has to be loaded before the device is created: "
                          "start the engine with --renderdoc (or MYENGINE_RENDERDOC=1) and capture again.";
                return false;
            }
            message = loadMessage;
            return false;
        }

        const bool triggered = gpuCapture_.TriggerNextFrame(message);
        if (triggered)
        {
            message = "GPU capture of the next frame requested; the file goes to " +
                      (apppaths::SavedDirectory(core::ServiceLocator::GetProjectContext().Root()) / "Captures").u8string();
            logger_.Info(message);
        }
        return triggered;
    }

    // After a frame: advances the capture request and the statistics request of the command line
    void Application::HandleDiagnosticsAfterFrame()
    {
        // A removed device ends a command-line request with a code of its own (the log has the breadcrumbs)
        const bool cliRequest = diagnostics_.captureFrame != 0 || (diagnostics_.frameStatsFrame != 0 && !frameStatsDone_) || captureTriggered_;
        if (cliRequest && renderAdapter_ != nullptr && renderAdapter_->IsDeviceRemoved())
        {
            const std::string text = "The GPU device was removed (see logs/myengine.log for the DRED breadcrumbs)";
            logger_.Error(text);
            WriteToConsole(text + "\n");
            exitCode_ = 6;
            RequestQuit();
            return;
        }

        if (diagnostics_.captureFrame != 0 && !captureTriggered_ && renderedFrames_ + 1 >= diagnostics_.captureFrame)
        {
            // RenderDoc captures the frame that follows the trigger: frame N is captured when it is asked for after N-1
            captureCountAtTrigger_ = gpuCapture_.CaptureCount();
            std::string message;
            if (gpuCapture_.TriggerNextFrame(message))
            {
                captureTriggered_ = true;
                captureTriggeredAtFrame_ = renderedFrames_;
                logger_.Info("GPU capture requested after frame " + std::to_string(renderedFrames_));
            }
            else
            {
                logger_.Error(message);
                WriteToConsole(message + "\n");
                exitCode_ = 3;
                RequestQuit();
                return;
            }
        }

        if (captureTriggered_)
        {
            if (gpuCapture_.CaptureCount() > captureCountAtTrigger_ && !gpuCapture_.IsCapturing())
            {
                const std::string path = gpuCapture_.LastCapturePath();
                logger_.Info("GPU capture written: " + path);
                WriteToConsole("GPU capture: " + path + "\n");
                captureTriggered_ = false;
                diagnostics_.captureFrame = 0;
                if (diagnostics_.exitAfterCapture && !(diagnostics_.frameStatsFrame != 0 && !frameStatsDone_))
                {
                    RequestQuit();
                }
            }
            else if (renderedFrames_ > captureTriggeredAtFrame_ + 60)
            {
                const std::string text = "GPU capture did not appear within 60 frames (is the window minimized?)";
                logger_.Error(text);
                WriteToConsole(text + "\n");
                exitCode_ = 4;
                RequestQuit();
                return;
            }
        }

        if (diagnostics_.frameStatsFrame != 0 && !frameStatsDone_ && renderedFrames_ >= diagnostics_.frameStatsFrame)
        {
            WriteFrameStats();
            frameStatsDone_ = true;
            if (!captureTriggered_)
            {
                RequestQuit();
            }
        }
    }

    void Application::WriteFrameStats()
    {
        render::GpuFrameStats stats;
        if (renderAdapter_ == nullptr || !renderAdapter_->GetLastFrameStats(stats, true))
        {
            const std::string text = "--frame-stats: the renderer has no statistics (no frame was finished)";
            logger_.Error(text);
            WriteToConsole(text + "\n");
            exitCode_ = 5;
            return;
        }

        const std::string map = sceneSavePath_.stem().u8string();
        const std::string adapter = renderAdapter_->GetAdapterName();
        std::filesystem::path target = diagnostics_.frameStatsOut;
        if (target.empty())
        {
            target = apppaths::SavedDirectory(core::ServiceLocator::GetProjectContext().Root()) / "FrameStats" /
                     std::filesystem::u8path("frame_stats_" + map + "_" + std::to_string(diagnostics_.frameStatsFrame) + ".json");
        }

        std::error_code error;
        if (!target.parent_path().empty())
        {
            std::filesystem::create_directories(target.parent_path(), error);
        }
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file << render::GpuFrameStatsToJson(stats, map, adapter);
        file.close();

        const std::string markdown = render::GpuFrameStatsToMarkdown(stats, map, adapter);
        logger_.Info("Frame statistics written: " + target.u8string());
        WriteToConsole(markdown + "\nJSON: " + target.u8string() + "\n");
    }

    LRESULT Application::HandleWindowMessage(Window& window, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        InputEvent event;
        event.windowId = window.Id();

        const bool hasMouseClientPosition =
            msg == WM_MOUSEMOVE ||
            msg == WM_LBUTTONDOWN ||
            msg == WM_LBUTTONUP ||
            msg == WM_LBUTTONDBLCLK ||
            msg == WM_RBUTTONDOWN ||
            msg == WM_RBUTTONUP ||
            msg == WM_RBUTTONDBLCLK ||
            msg == WM_MBUTTONDOWN ||
            msg == WM_MBUTTONUP ||
            msg == WM_MBUTTONDBLCLK;
        const float mouseClientX = hasMouseClientPosition
            ? static_cast<float>(static_cast<short>(LOWORD(lparam)))
            : 0.0f;
        const float mouseClientY = hasMouseClientPosition
            ? static_cast<float>(static_cast<short>(HIWORD(lparam)))
            : 0.0f;
        // A running game that uses the mouse buttons (the Rocket League game): over the viewport the clicks are its controls. They never go
        // through ImGui: a button that ImGui sees as held makes an item "active", and then it claims the keyboard (WantCaptureKeyboard)
        // and every key pressed while the button is held (steer while boosting with the left button) would be lost.
        const editor::EditorRuntimeState& runtimeState = core::ServiceLocator::GetEditorRuntimeState();
        const editor::WindowEditorState* windowEditorState = runtimeState.FindWindowState(window.Id());
        const bool gameMouseMode = runtimeState.mode == editor::RuntimeMode::Play && runtimeState.gameCapturesMouse;
        // (in the game mode the whole window is the game; in the editor only the viewport)
        const bool gameMouse = gameMouseMode && (gameMode_ || (windowEditorState != nullptr && windowEditorState->viewportHovered));
        const bool gameClick =
            ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) && gameMouse) ||
            (msg == WM_LBUTTONUP && gameMouseMode && input_.IsMouseDown(MouseButton::Left)) ||
            (msg == WM_RBUTTONUP && gameMouseMode && !cameraControlActive_ && input_.IsMouseDown(MouseButton::Right));
        const bool startSceneNavigationNow =
            (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) && !gameClick &&
            uiManager_.CanStartSceneNavigation(window.Id(), mouseClientX, mouseClientY);
        const bool bypassUiForCamera =
            gameClick ||
            startSceneNavigationNow ||
            cameraControlActive_ &&
            (msg == WM_RBUTTONDOWN ||
                msg == WM_RBUTTONUP ||
                msg == WM_RBUTTONDBLCLK ||
                msg == WM_MOUSEMOVE ||
                msg == WM_MOUSEWHEEL);

        if (!bypassUiForCamera)
        {
            uiManager_.HandleWindowMessage(window.Id(), window.Handle(), msg, wparam, lparam);
        }

        const bool uiWantsMouseCapture = !bypassUiForCamera && uiManager_.WantsMouseCapture(window.Id());
        const bool uiWantsKeyboardCapture = uiManager_.WantsKeyboardCapture(window.Id());
        const bool cameraConsumesKeyboard = cameraControlActive_ && inputOwnerWindowId_ == window.Id();
        const bool uiAllowsSceneNavigation =
            startSceneNavigationNow ||
            uiManager_.CanStartSceneNavigation(window.Id(), mouseClientX, mouseClientY);
        // the keys of a running game: only a text field that is being edited takes them from it (an ImGui item held by the mouse does not)
        const bool uiBlocksKeys = uiWantsKeyboardCapture && !(gameMouseMode && !uiManager_.WantsTextInput(window.Id()));

        switch (msg)
        {
            case WM_CLOSE:
            {
                DestroyWindow(window.Handle());
                return 0;
            }

            case WM_DESTROY:
            {
                if (auto* runtime = FindWindow(window.Id()); runtime != nullptr)
                {
                    runtime->closed = true;
                    runtime->controlledEntity = ecs::kInvalidEntity;
                }
                uiManager_.UnregisterWindow(window.Id());
                mouseCaptureButtons_ = 0; // the destroyed window takes the capture with it
                if (inputOwnerWindowId_ == window.Id())
                {
                    cameraControlActive_ = false;
                    SetCursorVisible(true);
                    SetInputOwnerWindow(0);
                }

                event.type = InputEventType::WindowClose;
                stateMachine_.HandleEvent(*this, event);

                if (AllWindowsClosed())
                {
                    RequestQuit();
                    PostQuitMessage(0);
                }

                return 0;
            }

            case WM_SIZE:
            {
                const auto width = static_cast<std::uint32_t>(LOWORD(lparam));
                const auto height = static_cast<std::uint32_t>(HIWORD(lparam));

                window.SetClientSize(width, height);

                if (width > 0 && height > 0)
                {
                    if (auto* runtime = FindWindow(window.Id()); runtime != nullptr && runtime->surface.IsValid() && renderAdapter_)
                    {
                        renderAdapter_->ResizeSurface(runtime->surface, width, height);
                    }
                }

                event.type = InputEventType::WindowResize;
                event.width = width;
                event.height = height;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_KILLFOCUS:
            {
                // Windows may deliver key-up to another app after Alt-Tab. Do not keep old keys held.
                if (input_.GetActiveWindowId() == window.Id())
                {
                    input_.ReleaseAllInputs();
                    if (runtimeState.gameCapturesMouse)
                    {
                        input_.OnMouseUp(MouseButton::Left);
                        if (!cameraControlActive_)
                        {
                            input_.OnMouseUp(MouseButton::Right);
                        }
                    }
                }
                if (inputOwnerWindowId_ == window.Id())
                {
                    cameraControlActive_ = false;
                    input_.OnMouseUp(MouseButton::Right);
                    SetCursorVisible(true);
                    SetInputOwnerWindow(0);
                }
                // The held buttons are dropped (ImGui clears them on focus loss), so is the capture
                if (mouseCaptureButtons_ != 0)
                {
                    mouseCaptureButtons_ = 0;
                    ReleaseCapture();
                }
                return 0;
            }

            case WM_CAPTURECHANGED:
            {
                // Someone else took the capture (a dialog, another application) while a button was held,
                // its button-up will never reach this window
                if (mouseCaptureButtons_ == 0 || reinterpret_cast<HWND>(lparam) == window.Handle())
                {
                    return 0;
                }

                mouseCaptureButtons_ = 0;
                // (lparam of this message is the window that took the capture, not a position)
                uiManager_.ReleaseMouseButtons(window.Id());
                input_.OnMouseUp(MouseButton::Left);
                if (cameraControlActive_ && inputOwnerWindowId_ == window.Id())
                {
                    cameraControlActive_ = false;
                    input_.OnMouseUp(MouseButton::Right);
                    SetCursorVisible(true);
                    SetInputOwnerWindow(0);
                }
                return 0;
            }

            case WM_KEYDOWN:
            {
                const auto key = static_cast<std::uint32_t>(wparam);

                if (uiBlocksKeys && !cameraConsumesKeyboard && key != VK_F3 && key != VK_F5)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                input_.OnKeyDown(key);
                event.type = InputEventType::KeyDown;
                event.key = key;

                logger_.Debug("KeyDown window=" + std::to_string(event.windowId) + " key=" + std::to_string(event.key));

                const bool firstPress = (lparam & (1 << 30)) == 0;
                if (firstPress)
                {
                    if (key == VK_F3)
                    {
                        TogglePhysicsDebugDraw();
                    }
                    else if (key == VK_F5 && scriptSystem_ != nullptr)
                    {
                        // Hot reload of every script (the file watcher does it on save anyway)
                        scriptSystem_->RequestReloadAll();
                    }
                }

                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_KEYUP:
            {
                // Release even if an editor widget captured the keyboard after our key-down.
                input_.OnKeyUp(static_cast<std::uint32_t>(wparam));
                if (uiBlocksKeys && !cameraConsumesKeyboard)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                event.type = InputEventType::KeyUp;
                event.key = static_cast<std::uint32_t>(wparam);
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            {
                // Without the capture a drag that ends outside the window never gets its button-up,
                // and ImGui keeps the button held until the next click
                AcquireMouseCapture(window, kCaptureLeftButton);

                if (gameMouse)
                {
                    // a running game that uses the mouse (the buttons over its viewport are its controls)
                    input_.SetActiveWindow(window.Id());
                    input_.OnMouseDown(MouseButton::Left);
                    return 0;
                }

                if (uiWantsMouseCapture)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                input_.OnMouseDown(MouseButton::Left);
                event.type = InputEventType::MouseDown;
                event.mouseButton = MouseButton::Left;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_LBUTTONUP:
            {
                ReleaseMouseCapture(kCaptureLeftButton);

                if (gameMouseMode && input_.IsMouseDown(MouseButton::Left))
                {
                    input_.OnMouseUp(MouseButton::Left);  // the release of a click that went to the game
                    return 0;
                }

                if (uiWantsMouseCapture)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                input_.OnMouseUp(MouseButton::Left);
                event.type = InputEventType::MouseUp;
                event.mouseButton = MouseButton::Left;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_RBUTTONDOWN:
            {
                if (gameMouse)
                {
                    // the game's right button (jump): no fly camera, no hidden cursor
                    input_.SetActiveWindow(window.Id());
                    AcquireMouseCapture(window, kCaptureRightButton);
                    input_.OnMouseDown(MouseButton::Right);
                    return 0;
                }

                if (!uiAllowsSceneNavigation)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                SetInputOwnerWindow(window.Id());
                cameraControlActive_ = true;
                AcquireMouseCapture(window, kCaptureRightButton);
                input_.OnMouseDown(MouseButton::Right);
                SetCursorVisible(false);
                WarpCursorToWindowCenter(window);
                event.type = InputEventType::MouseDown;
                event.mouseButton = MouseButton::Right;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_RBUTTONUP:
            {
                if (!cameraControlActive_)
                {
                    if (gameMouseMode && input_.IsMouseDown(MouseButton::Right))
                    {
                        ReleaseMouseCapture(kCaptureRightButton);
                        input_.OnMouseUp(MouseButton::Right);
                    }
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                cameraControlActive_ = false;
                ReleaseMouseCapture(kCaptureRightButton);
                input_.OnMouseUp(MouseButton::Right);
                SetCursorVisible(true);
                SetInputOwnerWindow(0);
                event.type = InputEventType::MouseUp;
                event.mouseButton = MouseButton::Right;
                stateMachine_.HandleEvent(*this, event);
                return 0;
            }

            case WM_MOUSEWHEEL:
            {
                if (uiWantsMouseCapture)
                {
                    return 0;
                }

                input_.SetActiveWindow(window.Id());
                if (cameraControlActive_ && inputOwnerWindowId_ == window.Id())
                {
                    const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wparam);
                    input_.OnMouseWheel(wheelDelta);
                }
                return 0;
            }

            case WM_MOUSEMOVE:
            {
                if (uiWantsMouseCapture)
                {
                    return 0;
                }

                if (cameraControlActive_ && inputOwnerWindowId_ == window.Id() && input_.IsMouseDown(MouseButton::Right))
                {
                    const int mouseX = static_cast<int>(static_cast<short>(LOWORD(lparam)));
                    const int mouseY = static_cast<int>(static_cast<short>(HIWORD(lparam)));
                    const int centerX = static_cast<int>(window.Width() / 2u);
                    const int centerY = static_cast<int>(window.Height() / 2u);
                    const int deltaX = mouseX - centerX;
                    const int deltaY = mouseY - centerY;

                    if (deltaX != 0 || deltaY != 0)
                    {
                        input_.AddMouseDelta(deltaX, deltaY);
                        WarpCursorToWindowCenter(window);
                    }
                }
                return 0;
            }

            default:
            {
                return DefWindowProcW(window.Handle(), msg, wparam, lparam);
            }
        }
    }

    state::StateMachine& Application::GetStateMachine()
    {
        return stateMachine_;
    }

    ecs::World& Application::GetWorld()
    {
        return world_;
    }

    input::InputManager& Application::GetInputManager()
    {
        return input_;
    }

    Logger& Application::GetLogger()
    {
        return logger_;
    }

    resource::ResourceManager& Application::GetResourceManager()
    {
        return *resourceManager_;
    }

    scripting::ScriptRuntime& Application::GetScriptRuntime()
    {
        return scriptRuntime_;
    }

    const std::filesystem::path& Application::GetSceneSavePath() const
    {
        return sceneSavePath_;
    }

    void Application::SetStateLabel(const std::string& label)
    {
        stateLabel_ = label;
        UpdateWindowTitles();
        uiManager_.SetStateLabel(label);
    }

    void Application::UpdateWindowTitles()
    {
        titleShowsPlay_ = core::ServiceLocator::GetEditorRuntimeState().mode == editor::RuntimeMode::Play;
        if (gameMode_)
        {
            for (auto& runtime : windows_)
            {
                if (!runtime.closed && runtime.window)
                {
                    runtime.window->SetTitle(L"Rocket Arena");
                }
            }
            return;
        }
        std::wstring suffix;
        if (const auto& project = core::ServiceLocator::GetProjectContext(); project.IsInitialized() && !sceneSavePath_.empty())
        {
            suffix += L" | " + Utf8ToWide(project.Name()) + L" \u2014 " + Utf8ToWide(sceneSavePath_.stem().u8string());
        }
        if (!stateLabel_.empty())
        {
            suffix += L" [" + Utf8ToWide(stateLabel_) + L"]";
        }
        if (titleShowsPlay_)
        {
            suffix += L" [PLAY]";
        }
        for (auto& runtime : windows_)
        {
            if (!runtime.closed && runtime.window)
            {
                runtime.window->SetTitle(runtime.baseTitle + suffix);
            }
        }
    }

    bool Application::SaveSceneToDisk()
    {
        if (!sceneLoaded_ || sceneSavePath_.empty() || apppaths::IsShipping())
        {
            return false;
        }

        return scene::SaveWorldToJson(world_, sceneSavePath_, &logger_);
    }

    bool Application::LoadSceneFromDisk()
    {
        if (sceneSavePath_.empty())
        {
            return false;
        }

        if (!scene::LoadWorldFromJson(world_, sceneSavePath_, &logger_))
        {
            return false;
        }

        RebindWindowControlledEntities();
        return true;
    }

    bool Application::OpenSceneFromDisk(const std::string& scenePath)
    {
        if (!sceneLoaded_ || scenePath.empty() || resourceManager_ == nullptr)
        {
            return false;
        }

        // Same lookup as for any asset: the project folder first, then the copy next to the executable
        const std::filesystem::path resolvedPath = resourceManager_->ResolvePath(std::filesystem::u8path(scenePath));
        if (!scene::LoadWorldFromJson(world_, resolvedPath, &logger_))
        {
            return false;
        }

        // From now on Save Scene and the save on exit go to this file
        sceneSavePath_ = resolvedPath;
        RebindWindowControlledEntities();
        core::ServiceLocator::GetEditorRuntimeState().mapPath =
            core::ServiceLocator::GetProjectContext().ToProjectRelative(sceneSavePath_);
        UpdateWindowTitles();
        return true;
    }

    struct Application::PendingSceneLoad
    {
        std::filesystem::path path;
        ecs::World loaded; // built on the worker, swapped into the live world on the main thread
        jobs::Context context;
        std::atomic<bool> finished{false};
        bool success = false;
        bool makeCurrent = false; // Open: the file becomes the one that Save writes. Revert: it already is.
        editor::LoadingId loadingId = 0;
        std::string detailPrefix; // "Loading map Main  .  " on the splash
        std::function<void(bool)> done;
    };

    bool Application::IsSceneLoaded() const
    {
        return sceneLoaded_;
    }

    editor::LoadingService& Application::GetLoadingService()
    {
        return loadingService_;
    }

    void Application::BeginOpenScene(const std::string& scenePath, std::function<void(bool)> done)
    {
        if (!sceneLoaded_ || scenePath.empty() || resourceManager_ == nullptr || pendingSceneLoad_ != nullptr)
        {
            if (done)
            {
                done(false);
            }
            return;
        }

        // Same lookup as for any asset: the project folder first, then the copy next to the executable
        const std::filesystem::path resolved = resourceManager_->ResolvePath(std::filesystem::u8path(scenePath));
        StartSceneLoad(resolved, true, "Loading map...", resolved.stem().u8string(), editor::LoadingMode::Modal, std::move(done));
    }

    void Application::BeginRevertScene(std::function<void(bool)> done)
    {
        if (sceneSavePath_.empty() || pendingSceneLoad_ != nullptr)
        {
            if (done)
            {
                done(false);
            }
            return;
        }

        StartSceneLoad(sceneSavePath_, false, "Reverting map...", sceneSavePath_.stem().u8string(), editor::LoadingMode::Modal,
            std::move(done));
    }

    void Application::StartSceneLoad(
        const std::filesystem::path& path,
        const bool makeCurrent,
        std::string label,
        std::string subtitle,
        const editor::LoadingMode mode,
        std::function<void(bool)> done)
    {
        auto load = std::make_unique<PendingSceneLoad>();
        load->path = path;
        load->makeCurrent = makeCurrent;
        load->done = std::move(done);
        load->loadingId = loadingService_.Begin(std::move(label), mode);
        loadingService_.SetSubtitle(load->loadingId, std::move(subtitle));
        // The splash has no second line for the map: its name goes in front of the stage ("Loading map Main - Parsing")
        if (mode == editor::LoadingMode::Splash)
        {
            load->detailPrefix = "Loading map " + path.stem().u8string() + "  \xC2\xB7  ";
            loadingService_.SetProgress(load->loadingId, -1.0f, "Loading map " + path.stem().u8string());
        }
        load->context.priority = jobs::Priority::Streaming;
        PendingSceneLoad* raw = load.get();
        pendingSceneLoad_ = std::move(load);

        // Nothing here touches the live world: the file is read and built into `raw->loaded`
        jobs::Execute(raw->context, [this, raw](jobs::JobArgs)
        {
            const auto progress = [this, raw](const float fraction, const char* stage)
            {
                std::string detail = stage != nullptr ? stage : "";
                if (!detail.empty())
                {
                    detail = raw->detailPrefix + detail;
                }
                loadingService_.SetProgress(raw->loadingId, fraction, std::move(detail));
            };
            raw->success = scene::ReadWorldFromFile(raw->loaded, raw->path, &logger_, progress);
            raw->finished.store(true, std::memory_order_release);
        });
    }

    void Application::PumpPendingSceneLoad()
    {
        if (pendingSceneLoad_ == nullptr || !pendingSceneLoad_->finished.load(std::memory_order_acquire))
        {
            return;
        }

        std::unique_ptr<PendingSceneLoad> load = std::move(pendingSceneLoad_);
        jobs::Wait(load->context);

        if (load->success)
        {
            scene::CommitLoadedWorld(world_, load->loaded);
            if (load->makeCurrent)
            {
                // From now on Save Scene and the save on exit go to this file
                sceneSavePath_ = load->path;
                core::ServiceLocator::GetEditorRuntimeState().mapPath =
                    core::ServiceLocator::GetProjectContext().ToProjectRelative(sceneSavePath_);
                UpdateWindowTitles();
            }
            RebindWindowControlledEntities();
            logger_.Info("SceneSerializer: scene loaded from " + load->path.u8string());
        }

        loadingService_.End(load->loadingId);
        if (load->done)
        {
            load->done(load->success);
        }
    }

    void Application::WaitPendingSceneLoad()
    {
        if (pendingSceneLoad_ != nullptr)
        {
            jobs::Wait(pendingSceneLoad_->context);
            loadingService_.End(pendingSceneLoad_->loadingId);
            pendingSceneLoad_.reset();
        }
    }

    void Application::SyncBackgroundOperations()
    {
        if (thumbnailService_ != nullptr)
        {
            const std::size_t queued = thumbnailService_->GetStats().queued;
            loadingService_.Sync("thumbnails", queued > 0, "Generating thumbnails", -1.0f, std::to_string(queued) + " left");
        }
        if (scriptSystem_ != nullptr)
        {
            loadingService_.Sync("scripts", scriptSystem_->IsReloadPending(), "Reloading scripts");
        }
        if (resourceManager_ != nullptr)
        {
            const std::size_t pending = resourceManager_->PendingLoadCount();
            loadingService_.Sync("assets", pending > 0, "Loading assets", -1.0f, std::to_string(pending) + " left");
        }
    }

    bool Application::SaveSceneAs(const std::string& scenePath)
    {
        if (!sceneLoaded_ || scenePath.empty() || apppaths::IsShipping())
        {
            return false;
        }

        // A file that does not exist yet resolves inside the project
        const std::filesystem::path target = core::ServiceLocator::GetProjectContext().ResolveContentPath(std::filesystem::u8path(scenePath));
        if (!scene::SaveWorldToJson(world_, target, &logger_))
        {
            return false;
        }

        sceneSavePath_ = target;
        core::ServiceLocator::GetEditorRuntimeState().mapPath =
            core::ServiceLocator::GetProjectContext().ToProjectRelative(sceneSavePath_);
        UpdateWindowTitles();
        return true;
    }

    bool Application::RestartWithProject(const std::filesystem::path& projectFile, const bool discardScene)
    {
        const std::filesystem::path executable = ProjectContext::ExecutableDirectory() / "myengine.exe";
        std::wstring modulePath(MAX_PATH * 2, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        const std::filesystem::path self = length > 0 && length < modulePath.size()
            ? std::filesystem::path(modulePath.substr(0, length))
            : executable;

        // The new process waits for this one to finish (the log file, the window class and the GPU are shared)
        std::wstring commandLine = L"\"" + self.wstring() + L"\" --project \"" + projectFile.wstring() +
            L"\" --wait-for-pid " + std::to_wstring(GetCurrentProcessId());

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const std::wstring workingDirectory = self.parent_path().wstring();
        if (!CreateProcessW(
                self.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                workingDirectory.c_str(),
                &startup,
                &process))
        {
            logger_.Error("Failed to start myengine for the project: " + projectFile.u8string());
            return false;
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        discardSceneOnExit_ = discardScene;

        // This window shows "Opening project..." for a moment before it goes, so that the hand-over to the new
        // process (which waits for this one to exit) is not a blank screen
        const editor::LoadingId opening = loadingService_.Begin("Opening project...", editor::LoadingMode::Modal);
        loadingService_.SetSubtitle(opening, projectFile.stem().u8string());
        loadingService_.SetProgress(opening, -1.0f, "Starting the editor");
        restartPending_ = true;
        restartQuitTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
        return true;
    }

    void Application::SetProjectFile(const std::filesystem::path& projectFile)
    {
        projectFile_ = projectFile;
    }

    void Application::SetShowProjectBrowser(const bool show)
    {
        showProjectBrowser_ = show;
    }

    void Application::SetExitAfterFrames(const std::uint32_t frames)
    {
        exitAfterFrames_ = frames;
    }

    void Application::SetDeferredSceneLoad(const bool deferred)
    {
        deferredSceneLoad_ = deferred;
    }

    std::string Application::CaptureSceneSnapshot() const
    {
        return scene::SerializeWorldToString(world_);
    }

    bool Application::RestoreSceneSnapshot(const std::string_view snapshotJson)
    {
        if (!scene::LoadWorldFromString(world_, snapshotJson, &logger_))
        {
            return false;
        }

        RebindWindowControlledEntities();
        return true;
    }

    Application::WindowRuntime* Application::FindWindow(const WindowId id)
    {
        for (auto& runtime : windows_)
        {
            if (runtime.window->Id() == id)
            {
                return &runtime;
            }
        }
        return nullptr;
    }

    const Application::WindowRuntime* Application::FindWindow(const WindowId id) const
    {
        for (const auto& runtime : windows_)
        {
            if (runtime.window->Id() == id)
            {
                return &runtime;
            }
        }
        return nullptr;
    }

    bool Application::AllWindowsClosed() const
    {
        if (windows_.empty())
        {
            return true;
        }

        for (const auto& runtime : windows_)
        {
            if (!runtime.closed)
            {
                return false;
            }
        }
        return true;
    }

    void Application::SetInputOwnerWindow(const WindowId id)
    {
        if (inputOwnerWindowId_ != id)
        {
            inputOwnerWindowId_ = id;
            input_.ResetMouseTracking();
        }
    }

    void Application::SetCursorVisible(const bool visible)
    {
        if (visible)
        {
            if (!cursorHidden_)
            {
                return;
            }

            while (ShowCursor(TRUE) < 0)
            {
            }
            cursorHidden_ = false;
            return;
        }

        if (cursorHidden_)
        {
            return;
        }

        while (ShowCursor(FALSE) >= 0)
        {
        }
        cursorHidden_ = true;
    }

    void Application::AcquireMouseCapture(const Window& window, const std::uint32_t button)
    {
        if (mouseCaptureButtons_ == 0)
        {
            SetCapture(window.Handle());
        }
        mouseCaptureButtons_ |= button;
    }

    void Application::ReleaseMouseCapture(const std::uint32_t button)
    {
        if ((mouseCaptureButtons_ & button) == 0)
        {
            return;
        }

        mouseCaptureButtons_ &= ~button;
        if (mouseCaptureButtons_ == 0)
        {
            ReleaseCapture();
        }
    }

    void Application::WarpCursorToWindowCenter(const Window& window)
    {
        const int centerX = static_cast<int>(window.Width() / 2u);
        const int centerY = static_cast<int>(window.Height() / 2u);

        POINT center{
            static_cast<LONG>(centerX),
            static_cast<LONG>(centerY)};
        ClientToScreen(window.Handle(), &center);
        SetCursorPos(center.x, center.y);
        input_.SetMousePositionReference(centerX, centerY);
    }

    void Application::ConfigureInputBindings()
    {
        input_.BindAction("camera_forward", 'W');
        input_.BindAction("camera_forward", VK_UP);
        input_.BindAction("camera_backward", 'S');
        input_.BindAction("camera_backward", VK_DOWN);
        input_.BindAction("camera_left", 'A');
        input_.BindAction("camera_left", VK_LEFT);
        input_.BindAction("camera_right", 'D');
        input_.BindAction("camera_right", VK_RIGHT);
        input_.BindAction("camera_up", VK_SPACE);
        input_.BindAction("camera_up", 'E');
        input_.BindAction("camera_down", VK_SHIFT);
        input_.BindAction("camera_down", VK_LSHIFT);
        input_.BindAction("camera_down", 'Q');

        input_.BindAction("player_forward", 'W');
        input_.BindAction("player_forward", VK_UP);
        input_.BindAction("player_backward", 'S');
        input_.BindAction("player_backward", VK_DOWN);
        input_.BindAction("player_left", 'A');
        input_.BindAction("player_left", VK_LEFT);
        input_.BindAction("player_right", 'D');
        input_.BindAction("player_right", VK_RIGHT);
        input_.BindAction("player_jump", VK_SPACE);
    }

    void Application::BindRuntimeEventListeners()
    {
        if (runtimeEventsBound_)
        {
            return;
        }

        runtimeEventsBound_ = true;

        core::ServiceLocator::GetEventBus().Subscribe<physics::CollisionEvent>(
            [this](const physics::CollisionEvent& event)
            {
                std::ostringstream message;
                message.setf(std::ios::fixed);
                message.precision(2);
                message << "Collision " << event.entityA << " vs " << event.entityB
                        << " | impulse=" << event.impulse
                        << " | point=(" << event.point.x << ", " << event.point.y << ", " << event.point.z << ")";

                core::ServiceLocator::GetPhysicsWorldState().PushRecentEvent(message.str());
                logger_.Info(message.str());
            });

        core::ServiceLocator::GetEventBus().Subscribe<physics::TriggerEvent>(
            [this](const physics::TriggerEvent& event)
            {
                std::ostringstream message;
                message.setf(std::ios::fixed);
                message.precision(2);
                message << "Trigger " << event.triggerEntity << " -> " << event.otherEntity
                        << " | point=(" << event.point.x << ", " << event.point.y << ", " << event.point.z << ")";

                core::ServiceLocator::GetPhysicsWorldState().PushRecentEvent(message.str());
                logger_.Info(message.str());
            });
    }

    void Application::ResetDemoScene()
    {
        world_.ClearEntities();
        core::ServiceLocator::GetPhysicsWorldState().recentEvents.clear();
        BuildDemoScene();
        core::ServiceLocator::GetEventBus().Publish(scene::SceneLoadedEvent{&world_});
    }

    void Application::SpawnDemoBox(const WindowId windowId)
    {
        const auto* runtime = FindWindow(windowId);
        if (runtime == nullptr)
        {
            return;
        }

        static constexpr char kCubeMesh[] = "assets/models/crate.obj";
        static constexpr char kWarmMaterial[] = "assets/materials/warm.material.json";

        const std::size_t bodyIndex = world_.GetEntities().size() + 1u;
        const float laneOffset = static_cast<float>(bodyIndex % 5u) * 0.7f - 1.4f;

        const ecs::EntityId entity = world_.CreateEntity();
        world_.Emplace<ecs::components::TagComponent>(entity).name = "SpawnedBox_" + std::to_string(entity);
        auto& transform = world_.Emplace<ecs::components::TransformComponent>(entity);
        transform.position = {laneOffset, 4.8f + static_cast<float>(bodyIndex % 3u) * 0.55f, 3.8f};
        transform.scale = {0.85f, 0.85f, 0.85f};
        auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(entity);
        renderer.meshPath = kCubeMesh;
        renderer.materialPath = kWarmMaterial;
        renderer.visible = true;
        world_.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = runtime->window->Id();

        auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(entity);
        rigidbody.mass = 1.0f;
        rigidbody.useGravity = true;

        auto& collider = world_.Emplace<ecs::components::ColliderComponent>(entity);
        collider.type = ecs::components::ColliderType::Box;
        collider.halfExtents = {0.5f, 0.5f, 0.5f};
        collider.friction = 0.55f;
        collider.bounciness = 0.12f;
    }

    void Application::SpawnDemoSphere(const WindowId windowId)
    {
        const auto* runtime = FindWindow(windowId);
        if (runtime == nullptr)
        {
            return;
        }

        static constexpr char kSphereMesh[] = "assets/models/sphere.obj";
        static constexpr char kCoolMaterial[] = "assets/materials/cool.material.json";

        const std::size_t bodyIndex = world_.GetEntities().size() + 1u;
        const float laneOffset = static_cast<float>(bodyIndex % 5u) * 0.65f - 1.3f;

        const ecs::EntityId entity = world_.CreateEntity();
        world_.Emplace<ecs::components::TagComponent>(entity).name = "SpawnedSphere_" + std::to_string(entity);
        auto& transform = world_.Emplace<ecs::components::TransformComponent>(entity);
        transform.position = {laneOffset, 5.2f + static_cast<float>(bodyIndex % 2u) * 0.6f, 2.6f};
        transform.scale = {0.78f, 0.78f, 0.78f};
        auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(entity);
        renderer.meshPath = kSphereMesh;
        renderer.materialPath = kCoolMaterial;
        renderer.visible = true;
        world_.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = runtime->window->Id();

        auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(entity);
        rigidbody.mass = 0.65f;
        rigidbody.useGravity = true;

        auto& collider = world_.Emplace<ecs::components::ColliderComponent>(entity);
        collider.type = ecs::components::ColliderType::Sphere;
        collider.radius = 0.5f;
        collider.friction = 0.28f;
        collider.bounciness = 0.18f;
    }

    void Application::SpawnDemoBurst(const WindowId windowId)
    {
        for (int index = 0; index < 3; ++index)
        {
            SpawnDemoBox(windowId);
            SpawnDemoSphere(windowId);
        }
    }

    void Application::TogglePhysicsPause()
    {
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        physicsState.physicsPaused = !physicsState.physicsPaused;
    }

    void Application::TogglePhysicsDebugDraw()
    {
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        physicsState.debugDrawEnabled = !physicsState.debugDrawEnabled;
    }

    void Application::AdjustGravity(const float delta)
    {
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        physicsState.gravityStrength = std::clamp(physicsState.gravityStrength + delta, 0.0f, 30.0f);
    }

    void Application::PublishFrameStatistics(
        const float deltaTime,
        const double worldUpdateMs,
        const double scriptsMs,
        const double stateUpdateMs,
        const double hotReloadMs,
        const double uiUpdateMs,
        const double renderMs,
        const double frameMs)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        fpsAccumulatorTime_ += deltaTime;
        fpsAccumulatorFrames_ += 1;
        if (fpsAccumulatorTime_ >= 1.0f)
        {
            averagedFps_ = fpsAccumulatorFrames_ / std::max(fpsAccumulatorTime_, 0.0001f);
            fpsAccumulatorTime_ = 0.0f;
            fpsAccumulatorFrames_ = 0;
        }

        const float averageFps = averagedFps_ > 0.0f
            ? averagedFps_
            : (frameMs > 0.0001 ? static_cast<float>(1000.0 / frameMs) : 0.0f);

        for (auto& runtime : windows_)
        {
            if (runtime.window == nullptr)
            {
                continue;
            }

            auto& windowState = editorState.GetOrCreateWindowState(runtime.window->Id());
            windowState.timings.deltaTime = deltaTime;
            windowState.timings.averageFps = averageFps;
            windowState.timings.frameMs = frameMs;
            windowState.timings.worldUpdateMs = worldUpdateMs;
            windowState.timings.scriptsMs = scriptsMs;
            windowState.timings.stateUpdateMs = stateUpdateMs;
            windowState.timings.hotReloadMs = hotReloadMs;
            windowState.timings.uiUpdateMs = uiUpdateMs;
            windowState.timings.renderMs = renderMs;
        }
    }

    void Application::BuildDemoScene()
    {
        static constexpr char kCubeMesh[] = "assets/models/crate.obj";
        static constexpr char kSphereMesh[] = "assets/models/sphere.obj";
        static constexpr char kDefaultMaterial[] = "assets/materials/default.material.json";
        static constexpr char kWarmMaterial[] = "assets/materials/warm.material.json";
        static constexpr char kCoolMaterial[] = "assets/materials/cool.material.json";

        if (resourceManager_ != nullptr)
        {
            resourceManager_->Load<resource::MeshAsset>(kCubeMesh);
            resourceManager_->Load<resource::MeshAsset>(kSphereMesh);
            resourceManager_->Load<resource::MaterialAsset>(kDefaultMaterial);
            resourceManager_->Load<resource::MaterialAsset>(kWarmMaterial);
            resourceManager_->Load<resource::MaterialAsset>(kCoolMaterial);
        }

        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        physicsState.physicsPaused = false;
        physicsState.debugDrawEnabled = false;
        physicsState.gravityStrength = 9.81f;
        physicsState.recentEvents.clear();

        for (std::size_t i = 0; i < windows_.size(); ++i)
        {
            auto& runtime = windows_[i];
            if (runtime.closed || runtime.window == nullptr)
            {
                continue;
            }

            const WindowId windowId = runtime.window->Id();

            const ecs::EntityId cameraEntity = world_.CreateEntity();
            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(cameraEntity);
                tag.name = "Camera_" + std::to_string(windowId);

                auto& camera = world_.Emplace<ecs::components::CameraComponent>(cameraEntity);
                camera.position = {0.0f, 3.0f, -10.5f};
                camera.rotationDeg = {8.0f, 0.0f, 0.0f};
                camera.fovYDeg = 60.0f;
                camera.orthographicHalfHeight = 1.0f;
                camera.nearPlane = 0.01f;
                camera.farPlane = 200.0f;
                camera.isPrimary = true;

                auto& controller = world_.Emplace<ecs::components::CameraControllerComponent>(cameraEntity);
                controller.moveSpeed = std::max(0.1f, config_.input.moveSpeed);
                controller.rotateSpeedDeg = std::max(10.0f, config_.input.rotateSpeedDeg * 0.7f);
                controller.zoomSpeed = std::max(0.1f, config_.input.scaleSpeed);
                controller.mouseSensitivityDeg = std::clamp(config_.input.rotateSpeedDeg * 0.0015f, 0.03f, 0.7f);

                world_.Emplace<ecs::components::WindowBindingComponent>(cameraEntity).windowId = windowId;
            }

            const ecs::EntityId controlledEntity = world_.CreateEntity();
            runtime.controlledEntity = controlledEntity;

            {
                auto& tag = world_.Emplace<ecs::components::TagComponent>(controlledEntity);
                tag.name = "Controlled_" + std::to_string(windowId);

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(controlledEntity);
                transform.position = {-2.2f, 0.2f, 2.0f};
                transform.scale = {0.95f, 0.95f, 0.95f};

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(controlledEntity);
                renderer.meshPath = kCubeMesh;
                renderer.materialPath = kDefaultMaterial;

                auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(controlledEntity);
                rigidbody.mass = 1.0f;
                rigidbody.useGravity = true;

                auto& collider = world_.Emplace<ecs::components::ColliderComponent>(controlledEntity);
                collider.type = ecs::components::ColliderType::Box;
                collider.halfExtents = {0.5f, 0.5f, 0.5f};
                collider.friction = 0.75f;
                collider.bounciness = 0.08f;

                auto& controller = world_.Emplace<ecs::components::PlayerControllerComponent>(controlledEntity);
                controller.windowId = windowId;
                controller.moveSpeed = 5.3f;
                controller.jumpSpeed = 6.7f;
                controller.airControl = 0.42f;

                world_.Emplace<ecs::components::WindowBindingComponent>(controlledEntity).windowId = windowId;
            }

            const auto createBoxBody =
                [&](const std::string& tagName,
                    const ecs::components::Vec3& position,
                    const ecs::components::Vec3& scale,
                    const char* materialPath,
                    const float mass,
                    const bool isTrigger = false)
            {
                const ecs::EntityId entity = world_.CreateEntity();
                world_.Emplace<ecs::components::TagComponent>(entity).name = tagName;

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(entity);
                transform.position = position;
                transform.scale = scale;

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(entity);
                renderer.meshPath = kCubeMesh;
                renderer.materialPath = materialPath;

                auto& collider = world_.Emplace<ecs::components::ColliderComponent>(entity);
                collider.type = ecs::components::ColliderType::Box;
                collider.halfExtents = {0.5f, 0.5f, 0.5f};
                collider.isTrigger = isTrigger;
                collider.friction = mass > 0.0f ? 0.58f : 0.85f;
                collider.bounciness = mass > 0.0f ? 0.12f : 0.02f;

                if (mass > 0.0f)
                {
                    auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(entity);
                    rigidbody.mass = mass;
                    rigidbody.useGravity = true;
                }

                world_.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = windowId;
                return entity;
            };

            const auto createSphereBody =
                [&](const std::string& tagName,
                    const ecs::components::Vec3& position,
                    const ecs::components::Vec3& scale,
                    const char* materialPath,
                    const float mass)
            {
                const ecs::EntityId entity = world_.CreateEntity();
                world_.Emplace<ecs::components::TagComponent>(entity).name = tagName;

                auto& transform = world_.Emplace<ecs::components::TransformComponent>(entity);
                transform.position = position;
                transform.scale = scale;

                auto& renderer = world_.Emplace<ecs::components::MeshRendererComponent>(entity);
                renderer.meshPath = kSphereMesh;
                renderer.materialPath = materialPath;

                //auto& motion = world_.Emplace<ecs::components::MotionComponent>(africanHeadEntity);
                //motion.angularVelocityDeg.y = 10.0f;
                //motion.angularVelocityDeg.z = 10.0f;

                auto& rigidbody = world_.Emplace<ecs::components::RigidbodyComponent>(entity);
                rigidbody.mass = mass;
                rigidbody.useGravity = true;

                auto& collider = world_.Emplace<ecs::components::ColliderComponent>(entity);
                collider.type = ecs::components::ColliderType::Sphere;
                collider.radius = 0.5f;
                collider.friction = 0.22f;
                collider.bounciness = 0.18f;

                world_.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = windowId;
                return entity;
            };

            createBoxBody("Floor_" + std::to_string(windowId), {0.0f, -1.55f, 3.0f}, {8.5f, 1.0f, 8.5f}, kCoolMaterial, 0.0f);
            createBoxBody("Platform_" + std::to_string(windowId), {2.6f, 0.0f, 3.8f}, {2.6f, 0.55f, 2.0f}, kCoolMaterial, 0.0f);
            createBoxBody("WallLeft_" + std::to_string(windowId), {-5.0f, 0.1f, 3.0f}, {0.5f, 2.2f, 6.6f}, kCoolMaterial, 0.0f);
            createBoxBody("WallRight_" + std::to_string(windowId), {5.0f, 0.1f, 3.0f}, {0.5f, 2.2f, 6.6f}, kCoolMaterial, 0.0f);
            createBoxBody("TriggerGate_" + std::to_string(windowId), {0.0f, 0.15f, 5.8f}, {2.2f, 1.6f, 0.55f}, kWarmMaterial, 0.0f, true);

            createBoxBody("BoxStackA_" + std::to_string(windowId), {-0.2f, -0.45f, 3.4f}, {0.9f, 0.9f, 0.9f}, kWarmMaterial, 1.0f);
            createBoxBody("BoxStackB_" + std::to_string(windowId), {-0.2f, 0.55f, 3.4f}, {0.9f, 0.9f, 0.9f}, kWarmMaterial, 1.0f);
            createBoxBody("BoxStackC_" + std::to_string(windowId), {-0.2f, 1.55f, 3.4f}, {0.9f, 0.9f, 0.9f}, kWarmMaterial, 1.0f);

            createSphereBody("SphereA_" + std::to_string(windowId), {1.8f, 2.6f, 2.6f}, {0.85f, 0.85f, 0.85f}, kDefaultMaterial, 0.7f);
            createSphereBody("SphereB_" + std::to_string(windowId), {2.6f, 4.1f, 2.8f}, {0.72f, 0.72f, 0.72f}, kDefaultMaterial, 0.55f);

            logger_.Info("Demo scene created for window id=" + std::to_string(windowId));
        }
    }

    void Application::RebindWindowControlledEntities()
    {
        for (auto& runtime : windows_)
        {
            runtime.controlledEntity = ecs::kInvalidEntity;
        }

        for (auto& runtime : windows_)
        {
            if (runtime.closed || runtime.window == nullptr)
            {
                continue;
            }

            const WindowId windowId = runtime.window->Id();
            const std::string expectedTag = "Controlled_" + std::to_string(windowId);

            world_.ForEach<ecs::components::TagComponent, ecs::components::TransformComponent, ecs::components::WindowBindingComponent>(
                [&](const ecs::EntityId entity,
                    ecs::components::TagComponent& tag,
                    ecs::components::TransformComponent&,
                    ecs::components::WindowBindingComponent& binding)
                {
                    if (runtime.controlledEntity != ecs::kInvalidEntity || binding.windowId != windowId)
                    {
                        return;
                    }

                    if (tag.name == expectedTag)
                    {
                        runtime.controlledEntity = entity;
                    }
                });
        }
    }

    void Application::RenderFrame()
    {
        stateMachine_.Render(*this);

        for (auto& runtime : windows_)
        {
            if (runtime.closed || !runtime.surface.IsValid())
            {
                continue;
            }

            if (!renderAdapter_->BeginFrame(runtime.surface, runtime.clearColor))
            {
                continue;
            }

            // Asset thumbnails are drawn into their own targets before the scene and the UI use the back buffer
            if (thumbnailService_ != nullptr)
            {
                RunThumbnailStep(logger_, "Render", [&] { thumbnailService_->Render(runtime.surface); });
            }

            render::IntRect renderRegion{};
            std::uint32_t renderWidth = runtime.window->Width();
            std::uint32_t renderHeight = runtime.window->Height();
            const bool hasEditorRenderRegion =
                runtime.window != nullptr &&
                TryBuildEditorRenderRegion(*runtime.window, renderRegion, renderWidth, renderHeight);
            renderAdapter_->SetRenderRegion(runtime.surface, hasEditorRenderRegion ? &renderRegion : nullptr);

            ecs::RenderFrameContext context{
                *renderAdapter_,
                runtime.surface,
                runtime.clearColor,
                runtime.window->Id(),
                renderWidth,
                renderHeight,
                &logger_,
                resourceManager_.get(),
            };

            world_.RenderSystems(context);
            renderAdapter_->SetRenderRegion(runtime.surface, nullptr);
            uiManager_.RenderWindow(runtime.window->Id());
            renderAdapter_->EndFrame(runtime.surface);
        }
    }
}
