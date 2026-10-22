// RlGameFlow.cpp
// The screens of the game (docs/game/rl-ui.md). Every screen is a function of the Frame of the kit that is called each frame; the state of the flow
// (which screen, the choices of the player) lives in Impl, the match and the 3D scene in RlGameSystem.

#include <myengine/game/RlGameFlow.h>

#include "RlLobbyScreens.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>

#include <rl/RlControls.h>
#include <rl/RlTraining.h>
#include <rl/RlTuning.h>

#include <myengine/core/AppPaths.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/editor/EditorState.h>
#include <myengine/game/RlCars.h>
#include <myengine/game/RlTrainingStore.h>
#include <myengine/ecs/systems/RlGameSystem.h>
#include <myengine/ui/UiManager.h>
#include <myengine/ui/rl/RlUiHost.h>
#include <myengine/ui/rl/RlUiHud.h>
#include <myengine/ui/rl/RlUiPaint.h>
#include <myengine/ui/rl/RlUiWidgets.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::game
{
    using namespace rlui;
    using namespace rlui::paint;
    namespace sys = ecs::systems;

    namespace
    {
        enum class Screen
        {
            Boot,
            Main,
            Play,
            Loading,
            Lobby,
            Match,
            Pause,
            Results,
            Settings,
            Room,
            Training,
            Garage,
        };

        const char* const kScreenNames[] = {"boot", "main", "play", "loading", "lobby", "match", "pause", "results", "settings", "room", "training", "garage"};

        const char* const kTips[] = {
            "Tip: hold {Space} to look at the ball, release for the car cam.",
            "Tip: tap {Right mouse} to jump, tap it again in the air to dodge.",
            "Tip: drive over the glowing pads to refill the boost.",
            "Tip: {Left mouse} boosts. A full pad gives you 100.",
            "Tip: a car at supersonic speed demolishes the enemy it hits.",
            "Tip: use the walls: hold the wheels on them and the camera follows.",
        };

        // ---- the controls screen: what a row edits
        struct Slot
        {
            bool axis = false;
            int action = 0;
            int sign = 0;  // axis: -1 or +1 (the half of the axis)
        };

        struct ControlRowDef
        {
            const char* label;
            const char* title;       // the explanation panel
            const char* help;
            const char* leftName;    // the name of the first slot in the rebind window ("STEER LEFT"), the second one is rightName
            const char* rightName;
            Slot slots[2];
            int slotCount;
        };

        constexpr int Axis(const rl::RlAxisAction a) { return static_cast<int>(a); }
        constexpr int Button(const rl::RlButtonAction a) { return static_cast<int>(a); }

        const ControlRowDef kControlRows[] = {
            {"Throttle", "THROTTLE", "Accelerate. The stronger you press, the faster you go.", "THROTTLE", "", {{true, Axis(rl::RlAxisAction::Throttle), 1}, {}}, 1},
            {"Reverse / brake", "REVERSE", "Brake, and reverse when you are stopped.", "REVERSE", "", {{true, Axis(rl::RlAxisAction::Throttle), -1}, {}}, 1},
            {"Steer left / right", "STEER", "Turn on the ground; yaw in the air.", "STEER LEFT", "STEER RIGHT",
             {{true, Axis(rl::RlAxisAction::Steer), -1}, {true, Axis(rl::RlAxisAction::Steer), 1}}, 2},
            {"Pitch nose down / up", "PITCH", "In the air the throttle keys pitch the car; the flip direction when you dodge.", "PITCH NOSE DOWN", "PITCH NOSE UP",
             {{true, Axis(rl::RlAxisAction::Pitch), -1}, {true, Axis(rl::RlAxisAction::Pitch), 1}}, 2},
            {"Air roll left / right", "AIR ROLL", "Roll the car in the air.", "AIR ROLL LEFT", "AIR ROLL RIGHT",
             {{true, Axis(rl::RlAxisAction::Roll), -1}, {true, Axis(rl::RlAxisAction::Roll), 1}}, 2},
            {"Jump", "JUMP", "Tap to jump, tap again in the air to dodge.", "JUMP", "", {{false, Button(rl::RlButtonAction::Jump), 0}, {}}, 1},
            {"Boost", "BOOST", "Hold to burn boost and accelerate beyond the top speed.", "BOOST", "", {{false, Button(rl::RlButtonAction::Boost), 0}, {}}, 1},
            {"Powerslide", "POWERSLIDE", "The handbrake on the ground; the air roll in the air.", "POWERSLIDE", "", {{false, Button(rl::RlButtonAction::Powerslide), 0}, {}}, 1},
            {"Ball cam", "BALL CAM", "Look at the ball, or at the car.", "BALL CAM", "", {{false, Button(rl::RlButtonAction::BallCam), 0}, {}}, 1},
            {"Scoreboard", "SCOREBOARD", "Hold to see the table of the match.", "SCOREBOARD", "", {{false, Button(rl::RlButtonAction::Scoreboard), 0}, {}}, 1},
            {"Pause", "PAUSE", "Open the pause menu.", "PAUSE", "", {{false, Button(rl::RlButtonAction::Menu), 0}, {}}, 1},
        };
        constexpr int kControlRowCount = static_cast<int>(sizeof(kControlRows) / sizeof(kControlRows[0]));

        std::string Bare(const std::string& token) { return !token.empty() && token.front() == '-' ? token.substr(1) : token; }
        bool IsPadToken(const std::string& token) { return Bare(token).rfind("Pad", 0) == 0; }
        bool IsStick(const std::string& token)
        {
            const std::string bare = Bare(token);
            return bare == "PadLeftX" || bare == "PadLeftY" || bare == "PadRightX" || bare == "PadRightY";
        }
        int SignOf(const std::string& token) { return !token.empty() && token.front() == '-' ? -1 : 1; }
        std::string WithSign(const std::string& bare, const int sign) { return (sign < 0 ? "-" : "") + bare; }

        std::string Format(const char* format, ...)
        {
            char buffer[256];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return buffer;
        }

        std::string Upper(std::string text)
        {
            for (char& c : text)
            {
                if (c >= 'a' && c <= 'z')
                {
                    c = static_cast<char>(c - 'a' + 'A');
                }
            }
            return text;
        }

        const char* const kBotLevels[] = {"ROOKIE", "PRO", "ALL-STAR"};
        const char* const kModeNames[] = {"PRACTICE", "1V1", "2V2", "3V3", "4V4"};
    }

    struct RlGameFlow::Impl
    {
        RlGameFlowServices svc;
        RlGameSettings settings;
        std::filesystem::path settingsFile;
        Host host;
        Hud hud;
        bool installed = false;

        Screen screen = Screen::Boot;
        bool menuLighting = false;
        Screen settingsReturn = Screen::Main;
        float screenTime = 0.0f;
        int frames = 0;
        Id lastFocus = kNoId;

        // the main menu
        bool askQuit = false;
        std::string errorTitle;
        std::string errorText;

        // play
        int playTab = 0;
        std::string portText;
        bool settingsLoaded = false;

        // loading
        sys::RlGameMatchSetup loadingSetup;
        float loadingTime = 0.0f;
        int loadingFrames = 0;
        bool loadingStarted = false;
        int tipIndex = 0;

        // match
        bool askLeave = false;
        bool askRestart = false;
        bool lobbyAskLeave = false;

        // settings
        int tab = 0;
        bool askResetControls = false;
        bool askResetTab = false;
        // the rebind window
        struct Rebind
        {
            bool active = false;
            int row = 0;
            int plate = 0;       // 0 keyboard / mouse, 1 gamepad
            int step = 0;        // which slot of the row is being asked
            float timer = 0.0f;  // seconds left
            bool clearedHalf = false;
        } rebind;
        // the video settings that wait for the answer of the player
        RlVideoSettings videoBackup;
        float videoConfirm = 0.0f;
        bool videoPending = false;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> resolutions;
        bool resolutionsRead = false;
        bool settingsDirty = false;

        // the lobby of a private match: LAN / Steam, the list, the room (RlLobbyScreens)
        RlLobbyScreens lobby;
        int autoStartHumans = 0;  // a host starts the match when so many players are in it (the lobby that was started had them)

        explicit Impl(const RlGameFlowServices& services)
            : svc(services)
            , lobby([this](const std::string& text) { Log(text); })
        {
        }

        sys::RlGameSystem& System() { return *svc.system; }

        // ---- the files and the settings applied to the game

        void LoadSettings()
        {
            settingsFile = svc.settingsFile.empty() ? RlGameSettings::DefaultPath() : svc.settingsFile;
            std::string error;
            if (!settings.Load(settingsFile, &error))
            {
                settings = RlGameSettings{};
            }
            settingsLoaded = true;
            portText = std::to_string(settings.play.hostPort);
        }

        void ApplyToGame()
        {
            sys::RlGameSystem& system = System();
            rl::RlCameraTuning base = system.CameraSettings();
            system.SetCameraSettings(settings.camera.ToTuning(base), settings.camera.ballCamHold);
            system.SetInvertPitch(settings.invertPitch);
            system.SetVolumes(settings.audio.master, settings.audio.effects, settings.audio.music);
            system.SetPlayerName(settings.PlayerName());
            CheckCar();
            host.SetUiScale(static_cast<float>(settings.video.uiScalePercent) / 100.0f);
        }

        void Save()
        {
            std::string error;
            const bool saved = settings.Save(settingsFile, &error);
            Log(saved ? "settings saved to " + settingsFile.generic_u8string() : "settings not saved: " + error);
        }

        // ---- the movement between the screens

        void Log(const std::string& text)
        {
            if (svc.log)
            {
                svc.log("Game UI: " + text);
            }
        }

        void GoTo(Frame& f, const Screen next)
        {
            Log(std::string("screen ") + kScreenNames[static_cast<int>(next)]);
            screen = next;
            screenTime = 0.0f;
            askQuit = askLeave = askRestart = lobbyAskLeave = false;
            askResetControls = askResetTab = false;
            resolutionsRead = false;
            if (next != Screen::Lobby && next != Screen::Loading)
            {
                autoStartHumans = 0;
            }
            if (next == Screen::Main || next == Screen::Play)
            {
                lobby.ReleaseMatch();  // the match of the lobby is over or was left
            }
            if (next == Screen::Main)
            {
                lobby.CloseIdle();
            }
            f.ResetFocus();
            // the stage behind the menus is dimmer than the field of the match
            const bool menuLight = next != Screen::Match && next != Screen::Pause;
            if (menuLight != menuLighting && svc.setLighting)
            {
                menuLighting = menuLight;
                svc.setLighting(menuLight);
            }
            // the pointer and the keyboard belong to the menus except in a match
            if (svc.ui != nullptr)
            {
                svc.ui->SetGameUiCapturesInput(next != Screen::Match);
            }
        }

        void Sound(const char* name) { System().PlayUiSound(name); }

        // The common furniture of a screen over the 3D scene: the dark gradient on the left that the text stands on
        void SideShade(Frame& f, const float width = 1000.0f)
        {
            const Vec2 window = f.metrics().designWindow;
            f.SetSpace(Space::Screen);
            FillRectGradient(f, {0.0f, 0.0f, width * window.x / 1920.0f + 400.0f, window.y}, Color::Hex(0x050b1c, 0.92f), Color::Hex(0x050b1c, 0.0f));
            f.SetSpace(Space::Menu);
        }

        // The whole window dimmed and the stage drawn by the kit: for screens with a lot of text on them
        void Backdrop(Frame& f, const float alpha = 0.9f)
        {
            f.SetSpace(Space::Screen);
            Dim(f, Color::Hex(0x050b1c, alpha));
            f.SetSpace(Space::Menu);
        }

        void Hints(Frame& f, std::initializer_list<Hint> hints)
        {
            std::vector<Hint> list(hints);
            HintBar(f, Span<Hint>(list));
        }

        // ---- the main loop of the screens

        double drawMsSum = 0.0;
        int drawCount = 0;
        float drawLogTimer = 0.0f;

        void Draw(Frame& f)
        {
            const auto begin = std::chrono::steady_clock::now();
            DrawScreen(f);
            drawMsSum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            ++drawCount;
            drawLogTimer += f.DeltaTime();
            if (drawLogTimer >= 5.0f && drawCount > 0)
            {
                Log(Format("screen %s: %.3f ms per frame of the screens (%d frames)", kScreenNames[static_cast<int>(screen)], drawMsSum / drawCount, drawCount));
                drawLogTimer = 0.0f;
                drawMsSum = 0.0;
                drawCount = 0;
            }
        }

        float cursorIdle = 0.0f;

        void DrawScreen(Frame& f)
        {
            screenTime += f.DeltaTime();
            // the pointer hides after two seconds without moving in the menus (a match hides it by itself)
            cursorIdle = (f.input().MouseMoved() || f.input().MousePressed() || f.input().Wheel() != 0.0f) ? 0.0f : cursorIdle + f.DeltaTime();
            if (cursorIdle > 2.0f && screen != Screen::Match)
            {
                core::ServiceLocator::GetEditorRuntimeState().gameHidesCursor = true;
            }
            ++frames;
            lobby.Update(f.DeltaTime());
            if (screen == Screen::Main || screen == Screen::Play)
            {
                if (lobby.TakeOpenRoom())  // an invitation of a friend, or +connect_lobby
                {
                    playTab = 1;
                    GoTo(f, Screen::Room);
                }
                else if (screen == Screen::Main && errorText.empty() && lobby.TakeError(errorTitle, errorText))
                {
                    Log("lobby error: " + errorText);
                }
            }
            sys::RlGameSystem& system = System();
            const Vec2 window = f.metrics().window;
            system.SetViewAspect(window.y > 0.0f ? window.x / window.y : 16.0f / 9.0f);

            // the sounds of the menus: the focus moved / a choice was made / back
            if (screen != Screen::Match && screen != Screen::Boot && screen != Screen::Loading)
            {
                if (f.input().Pressed(Nav::Confirm) || f.input().MousePressed())
                {
                    Sound("ui_click");
                }
                else if (f.input().Pressed(Nav::Back))
                {
                    Sound("ui_back");
                }
                else if (f.Focus() != lastFocus && lastFocus != kNoId && screenTime > 0.25f)
                {
                    Sound("ui_hover");
                }
            }
            lastFocus = f.Focus();

            const sys::RlGameStatus& status = system.Status();
            // the network can end a match, or fail, on any screen
            if (status.stage == sys::RlGameStage::Failed && screen != Screen::Main && screen != Screen::Boot)
            {
                errorTitle = "CONNECTION FAILED";
                errorText = status.error.empty() ? "The connection was lost." : status.error;
                GoTo(f, Screen::Main);
            }

            // a network match that the command line started (--game --rl-host / --rl-join) arrives without the Play screen
            if (screen == Screen::Main && errorText.empty() && !askQuit)
            {
                if (status.stage == sys::RlGameStage::Lobby || status.stage == sys::RlGameStage::Connecting)
                {
                    lobbyEnteredAt = frames;
                    GoTo(f, Screen::Lobby);
                }
                else if (status.stage == sys::RlGameStage::Playing)
                {
                    hud.Reset();
                    GoTo(f, Screen::Match);
                }
            }

            switch (screen)
            {
            case Screen::Boot: DrawBoot(f); break;
            case Screen::Main: DrawMain(f); break;
            case Screen::Play: DrawPlay(f); break;
            case Screen::Loading: DrawLoading(f); break;
            case Screen::Lobby: DrawLobby(f); break;
            case Screen::Room: DrawRoom(f); break;
            case Screen::Match: DrawMatch(f); break;
            case Screen::Pause: DrawPause(f); break;
            case Screen::Results: DrawResults(f); break;
            case Screen::Settings: DrawSettings(f); break;
            case Screen::Training: DrawTraining(f); break;
            case Screen::Garage: DrawGarage(f); break;
            }
        }

        // ---- boot

        void DrawBoot(Frame& f)
        {
            f.SetSpace(Space::Screen);
            Dim(f, Color::Hex(0x000000, 1.0f));
            const Vec2 window = f.metrics().designWindow;
            const float alpha = std::min(screenTime / 0.35f, 1.0f);
            Text(f, FontStyle::BlackItalic, 64.0f, {window.x * 0.5f, window.y * 0.5f - 44.0f}, style::kText.Faded(alpha), "MYENGINE", Align::Center, 0.06f);
            FillRect(f, {window.x * 0.5f - 110.0f, window.y * 0.5f + 40.0f, 220.0f * alpha, 4.0f}, style::kAccent.Faded(alpha));
            f.SetSpace(Space::Menu);
            const bool ready = !svc.sceneReady || svc.sceneReady();
            if (ready)
            {
                PreloadCars();
            }
            if (ready && screenTime > 0.9f && frames > 4)
            {
                GoTo(f, Screen::Main);
            }
        }

        // ---- main menu

        void DrawMain(Frame& f)
        {
            const sys::RlGameStatus& status = System().Status();
            SideShade(f);
            ScreenTitle(f, "MYENGINE", "ROCKET\nARENA", {80.0f, 76.0f}, true);
            const float versionWidth = Chip(f, {1850.0f, 60.0f}, "V0.1 \xC2\xB7 G1");
            Chip(f, {1850.0f - versionWidth - 12.0f, 60.0f}, "OFFLINE", Icon::WifiOff);

            const Id play = HashId("main/play"), training = HashId("main/training"), settingsId = HashId("main/settings"), exit = HashId("main/exit");
            const Id garageId = HashId("main/garage");
            f.SetDefaultFocus(mainDefaultFocus);
            const bool dialogOpen = askQuit || !errorText.empty();
            Column column({76.0f, 370.0f, style::kMenuButtonWidth, style::kMenuButtonHeight}, style::kMenuButtonStep);
            if (MenuButton(f, play, column.Next(), "PLAY", Icon::Play))
            {
                playTab = 0;
                GoTo(f, Screen::Play);
            }
            if (MenuButton(f, garageId, column.Next(), "GARAGE", Icon::Car))
            {
                mainDefaultFocus = garageId;
                OpenGarage(f, Screen::Main);
                return;
            }
            if (MenuButton(f, training, column.Next(), "TRAINING", Icon::Target))
            {
                OpenTraining(f);
            }
            if (MenuButton(f, settingsId, column.Next(), "SETTINGS", Icon::Settings))
            {
                settingsReturn = Screen::Main;
                tab = 0;
                GoTo(f, Screen::Settings);
            }
            if (MenuButton(f, exit, column.Next(), "EXIT", Icon::Exit))
            {
                askQuit = true;
            }
            if (f.input().Pressed(Nav::Back) && !dialogOpen)
            {
                f.SetFocus(exit);
            }
            Hints(f, {{Glyph::UpDown, Glyph::PadA, "Select"}, {Glyph::Enter, Glyph::None, "Confirm"}, {Glyph::Esc, Glyph::PadB, "Back"}});
            (void)status;
            // the car of the player, lower right under the podium
            if (const game::RlCarDef* car = game::Cars().FindOrDefault(settings.car))
            {
                const Rect plate{1380.0f, 876.0f, 430.0f, 84.0f};
                Parallelogram(f, plate, style::kNavySolid.Faded(0.9f));
                Text(f, FontStyle::SemiBold, 16.0f, {plate.x + 36.0f, plate.y + 12.0f}, style::kTextDim, "YOUR CAR", Align::Left, 0.1f);
                const std::string name = Upper(car->name);
                Text(f, FontStyle::BlackItalic, 34.0f, {plate.x + 36.0f, plate.y + 32.0f}, style::kText, name);
                Text(f, FontStyle::Bold, 18.0f, {plate.x + 50.0f + TextWidth(f, FontStyle::BlackItalic, 34.0f, name), plate.y + 44.0f}, style::kTextDim, "\xC2\xB7 " + Upper(car->className), Align::Left, 0.08f);
            }
            if (carMissingToast)
            {
                carMissingToast = false;
                f.ShowToast("Your car is no longer available \xE2\x80\x94 Pulse selected", style::kMvp, 4.0f);
            }
            if (askQuit)
            {
                const DialogResult result = ConfirmDialog(f, HashId("main/quit"), {"QUIT TO DESKTOP?", "Your settings are saved.", "Quit", "Cancel"});
                if (result == DialogResult::Confirm)
                {
                    Save();
                    askQuit = false;
                    if (svc.quit)
                    {
                        svc.quit();
                    }
                }
                else if (result == DialogResult::Cancel)
                {
                    askQuit = false;
                }
            }
            else if (!errorText.empty())
            {
                const DialogResult result = ConfirmDialog(f, HashId("main/error"), {errorTitle, errorText, "Main menu", "Close"});
                if (result != DialogResult::None)
                {
                    errorText.clear();
                    errorTitle.clear();
                }
            }
        }

        // ---- play

        void BeginLoading(Frame& f, const sys::RlGameMatchSetup& setup)
        {
            loadingSetup = setup;
            loadingSetup.playerName = settings.PlayerName();
            trainingSession = setup.kind == sys::RlGameMatchSetup::Kind::Training;
            loadingTime = 0.0f;
            loadingFrames = 0;
            loadingStarted = false;
            tipIndex = (tipIndex + 1) % static_cast<int>(sizeof(kTips) / sizeof(kTips[0]));
            hud.Reset();
            GoTo(f, Screen::Loading);
        }

        void DrawPlay(Frame& f)
        {
            Backdrop(f, 0.95f);
            const bool privateTab = playTab == 1;
            ScreenTitle(f, "PLAY", privateTab ? "PRIVATE MATCH" : "CHOOSE A MODE");
            const std::string_view labels[] = {"OFFLINE \xC2\xB7 BOTS", "PRIVATE MATCH"};
            const int newTab = Tabs(f, HashId("play/tabs"), {75.0f, 200.0f, 640.0f, 54.0f}, Span<std::string_view>(labels), playTab);
            if (newTab != playTab)
            {
                playTab = newTab;
                f.SetFocus(kNoId);
            }
            if (!privateTab)
            {
                const ModeCardDesc cards[4] = {{"1v1", "DUEL", "You + 1 bot", 1}, {"2v2", "DOUBLES", "You + 3 bots", 2}, {"3v3", "STANDARD", "You + 5 bots", 3}, {"4v4", "CHAOS", "You + 7 bots", 4}};
                f.SetDefaultFocus(HashId("play/card" + std::to_string(settings.play.mode - 1)));
                for (int i = 0; i < 4; ++i)
                {
                    if (ModeCard(f, HashId("play/card" + std::to_string(i)), {62.0f + 284.0f * static_cast<float>(i), 300.0f, 320.0f, 330.0f}, cards[i], settings.play.mode == i + 1))
                    {
                        settings.play.mode = i + 1;
                    }
                }
                const std::string_view levels[] = {"Rookie", "Pro", "All-star"};
                settings.play.botLevel =
                    OptionRow(f, HashId("play/difficulty"), {80.0f, 700.0f, 1100.0f, 64.0f}, "Bot difficulty", Span<std::string_view>(levels), settings.play.botLevel);
                if (const game::RlCarDef* car = game::Cars().FindOrDefault(settings.car))
                {
                    const RowFrame row = Row(f, HashId("play/car"), {80.0f, 774.0f, 1100.0f, 64.0f}, "Car");
                    const Color ink = row.state.focused ? style::kFocusText : style::kText;
                    const std::string name = Upper(car->name);
                    Text(f, FontStyle::BlackItalic, 34.0f, {row.content.x, row.content.y + 12.0f}, ink, name);
                    Text(f, FontStyle::SemiBold, 22.0f, {row.content.x + TextWidth(f, FontStyle::BlackItalic, 34.0f, name) + 18.0f, row.content.y + 20.0f},
                         row.state.focused ? Color::Hex(0x4a5d85) : style::kTextDim, car->className);
                    Text(f, FontStyle::SemiBold, 22.0f, {80.0f + 1100.0f - 54.0f, row.content.y + 20.0f}, ink, "Change", Align::Right);
                    IconGlyph(f, Icon::ChevronRight, 22.0f, {80.0f + 1100.0f - 34.0f, row.content.y + 32.0f}, ink);
                    if (row.state.activated)
                    {
                        OpenGarage(f, Screen::Play);
                        return;
                    }
                }
                if (ActionButton(f, HashId("play/start"), {1370.0f, 877.0f, 440.0f, 76.0f}, "START MATCH", Icon::Play, true, true))
                {
                    sys::RlGameMatchSetup setup;
                    setup.mode = settings.play.mode;
                    setup.botLevel = settings.play.botLevel;
                    Save();
                    BeginLoading(f, setup);
                }
                Hints(f, {{Glyph::LeftRight, Glyph::PadA, "Select"}, {Glyph::QE, Glyph::PadLBRB, "Tabs"}, {Glyph::Esc, Glyph::PadB, "Back"}});
            }
            else
            {
                if (lobby.DrawPrivate(f, settings, settings.PlayerName()) == RlLobbyScreens::Action::OpenRoom)
                {
                    Save();
                    GoTo(f, Screen::Room);
                    return;
                }
                Hints(f, {{Glyph::AllArrows, Glyph::PadA, "Select"}, {Glyph::QE, Glyph::PadLBRB, "Tabs"}, {Glyph::Esc, Glyph::PadB, "Back"}});
            }
            if (f.input().Pressed(Nav::Back) && !(privateTab && lobby.ErrorShown()))
            {
                Save();
                GoTo(f, Screen::Main);
            }
        }

        // The room of the lobby service (RlLobbyScreens): the members, the readiness, the invitation, the start. When the host starts, the match of G5 begins
        // on the transport of the service: hosts wait in the lobby of the match until the players of this lobby are in, then start it by themselves.
        void DrawRoom(Frame& f)
        {
            const RlLobbyScreens::Action action = lobby.DrawRoom(f, settings.PlayerName());
            if (action == RlLobbyScreens::Action::None)
            {
                // the car of the player under the invitation: the garage opens over the room, Back returns (G / Y do the same)
                const game::RlCarDef* car = game::Cars().FindOrDefault(settings.car);
                const bool free = !lobby.ErrorShown();
                Panel(f, {1210.0f, 650.0f, 630.0f, 210.0f}, false, true);
                Text(f, FontStyle::SemiBold, 16.0f, {1290.0f, 670.0f}, style::kTextFaint, "YOUR CAR", Align::Left, 0.1f);
                if (car != nullptr)
                {
                    Image(f, CarImage(*car, 0), {1290.0f, 700.0f, 200.0f, 96.0f});
                    Text(f, FontStyle::BlackItalic, 34.0f, {1510.0f, 704.0f}, style::kText, Upper(car->name));
                    Text(f, FontStyle::SemiBold, 18.0f, {1510.0f, 750.0f}, style::kTextDim, Upper(car->className), Align::Left, 0.1f);
                }
                if (ActionButton(f, HashId("room/car"), {1290.0f, 800.0f, 400.0f, 48.0f}, "CHANGE CAR", Icon::Car, free) || (free && (f.input().KeyPressed('G') || f.input().Pressed(Nav::ResetRow))))
                {
                    OpenGarage(f, Screen::Room);
                    return;
                }
            }
            if (action == RlLobbyScreens::Action::LeaveRoom)
            {
                GoTo(f, Screen::Play);
            }
            else if (action == RlLobbyScreens::Action::StartMatch)
            {
                sys::RlGameMatchSetup setup;
                int humans = 0;
                if (lobby.TakeStartedMatch(setup, humans))
                {
                    setup.playerName = settings.PlayerName();
                    Save();
                    hud.Reset();
                    System().StartMatch(setup);
                    lobbyEnteredAt = frames;
                    GoTo(f, Screen::Lobby);
                    autoStartHumans = setup.kind == sys::RlGameMatchSetup::Kind::Host ? humans : 0;
                }
            }
        }

        // ---- loading

        void DrawLoading(Frame& f)
        {
            MenuBackdropOver3D(f);
            const std::string config = LoadingConfigText();
            Text(f, FontStyle::SemiBold, 24.0f, {80.0f, 703.0f}, style::kTextDim, config);
            if (const game::RlCarDef* car = game::Cars().FindOrDefault(settings.car))
            {
                Text(f, FontStyle::SemiBold, 24.0f, {80.0f, 667.0f}, style::kTextDim, "Your car: " + Upper(car->name));
            }
            Text(f, FontStyle::BlackItalic, 84.0f, {80.0f, 730.0f}, style::kText, "STANDARD ARENA");
            // the tip: {Key} is a key cap
            std::string tip = kTips[tipIndex];
            float x = 80.0f;
            while (!tip.empty())
            {
                const size_t open = tip.find('{');
                const std::string before = tip.substr(0, open);
                if (!before.empty())
                {
                    Text(f, FontStyle::SemiBold, 24.0f, {x, 846.0f}, style::kTextDim, before);
                    x += TextWidth(f, FontStyle::SemiBold, 24.0f, before);
                }
                if (open == std::string::npos)
                {
                    break;
                }
                const size_t close = tip.find('}', open);
                const std::string key = tip.substr(open + 1, close - open - 1);
                x += KeyCap(f, {x, 840.0f}, key) + 4.0f;
                tip = close == std::string::npos ? std::string() : tip.substr(close + 1);
            }
            loadingTime += f.DeltaTime();
            ++loadingFrames;
            const sys::RlGameStatus& status = System().Status();
            // the match is built after the screen has been drawn for a few frames (the building takes a moment)
            if (!loadingStarted && loadingFrames >= 3 && loadingTime >= 0.25f)
            {
                loadingStarted = true;
                System().StartMatch(loadingSetup);
            }
            const float real = svc.loadingProgress ? svc.loadingProgress() : -1.0f;
            float fraction = std::min(loadingTime / 1.1f, 0.95f);
            if (real >= 0.0f)
            {
                fraction = std::min(fraction, 0.4f + 0.55f * real);
            }
            const bool started = loadingStarted && status.stage == sys::RlGameStage::Playing;
            if (started)
            {
                fraction = 1.0f;
            }
            ProgressBar(f, {80.0f, 960.0f, 1760.0f, 10.0f}, fraction);
            Text(f, FontStyle::SemiBold, 22.0f, {1840.0f, 996.0f}, style::kBlueHi, Format("%s \xC2\xB7 %d%%", fraction < 0.7f ? "Building the arena" : "Starting the match",
                                                                                          static_cast<int>(std::lround(fraction * 100.0f))), Align::Right);
            if (started && loadingTime >= 0.8f)
            {
                System().MatchViewBegins();  // the flight over the stadium starts now
                hud.Reset();
                GoTo(f, Screen::Match);
            }
            else if (loadingStarted && status.stage == sys::RlGameStage::Failed)
            {
                errorTitle = "CANNOT START";
                errorText = status.error;
                GoTo(f, Screen::Main);
            }
        }

        std::string LoadingConfigText() const
        {
            if (loadingSetup.kind == sys::RlGameMatchSetup::Kind::Training)
            {
                std::string pack = "FREE PLAY";
                for (const rl::RlTrainingPack& candidate : trainingPacks)
                {
                    if (candidate.id == loadingSetup.trainingPack)
                    {
                        pack = Upper(candidate.name);
                    }
                }
                return "TRAINING \xC2\xB7 " + pack;
            }
            std::string text = loadingSetup.kind == sys::RlGameMatchSetup::Kind::Offline ? "OFFLINE" : "ONLINE";
            text += " \xC2\xB7 ";
            text += loadingSetup.mode == 0 ? "TRAINING" : kModeNames[std::clamp(loadingSetup.mode, 0, 4)];
            if (loadingSetup.mode != 0)
            {
                text += " \xC2\xB7 ";
                text += kBotLevels[std::clamp(loadingSetup.botLevel, 0, 2)];
                text += " BOTS";
            }
            return text;
        }

        // The loading screen over the 3D scene: the same dark stage as the menus, so the text can be read
        void MenuBackdropOver3D(Frame& f)
        {
            f.SetSpace(Space::Screen);
            Dim(f, Color::Hex(0x050b1c, 0.96f));
            f.SetSpace(Space::Menu);
        }

        // ---- training: free play and the packs of exercises (docs/game/rl-training.md)

        std::vector<rl::RlTrainingPack> trainingPacks;
        rl::RlTrainingResults trainingResults;
        int trainingSelected = 0;       // 0 free play, 1.. the packs
        int trainingShown = 0;          // what the panel on the right describes: the focused item
        bool trainingSession = false;   // the match that runs (or ran) is a training: Leave goes back to the Training screen

        // The screen reads the packs and the results of the player (the match that has just ended may have changed them)
        void OpenTraining(Frame& f)
        {
            std::vector<std::string> errors;
            trainingPacks = LoadTrainingPacks(&errors);
            for (const std::string& error : errors)
            {
                Log("training: " + error);
            }
            trainingResults.LoadFromFile(TrainingResultsPath().generic_u8string());
            trainingSelected = std::clamp(trainingSelected, 0, static_cast<int>(trainingPacks.size()));
            trainingShown = trainingSelected;
            GoTo(f, Screen::Training);
        }

        void StartTraining(Frame& f, const int item)
        {
            sys::RlGameMatchSetup setup;
            setup.kind = sys::RlGameMatchSetup::Kind::Training;
            setup.mode = 0;
            setup.botLevel = 2;
            setup.trainingPack = item == 0 ? std::string() : trainingPacks[static_cast<size_t>(item - 1)].id;
            trainingSelected = item;
            BeginLoading(f, setup);
        }

        void DrawTraining(Frame& f)
        {
            Backdrop(f, 0.95f);
            ScreenTitle(f, "TRAINING", "CHOOSE A DRILL");
            const int count = static_cast<int>(trainingPacks.size()) + 1;
            f.SetDefaultFocus(HashId("training/item" + std::to_string(trainingSelected)));
            Column column({76.0f, 300.0f, style::kMenuButtonWidth, style::kMenuButtonHeight}, style::kMenuButtonStep);
            for (int i = 0; i < count; ++i)
            {
                const Id id = HashId("training/item" + std::to_string(i));
                const std::string label = i == 0 ? std::string("FREE PLAY") : Upper(trainingPacks[static_cast<size_t>(i - 1)].name);
                if (MenuButton(f, id, column.Next(), label, i == 0 ? Icon::Play : Icon::Target))
                {
                    StartTraining(f, i);
                    return;
                }
                if (f.Focus() == id)
                {
                    trainingShown = i;
                }
            }

            // the panel is tall: a small slant, so that the text does not meet the edge
            const Rect panel{700.0f, 290.0f, 1140.0f, 640.0f};
            const float slant = 40.0f;
            Parallelogram(f, panel, style::kNavySolid.Faded(0.94f), slant);
            ParallelogramOutline(f, panel, style::kLine, 1.0f, slant);
            Parallelogram(f, {panel.x, panel.y, 6.0f, panel.h}, style::kAccent, slant);
            const float left = panel.x + slant + 40.0f;
            if (trainingShown <= 0 || trainingShown > static_cast<int>(trainingPacks.size()))
            {
                Text(f, FontStyle::BlackItalic, 52.0f, {left, panel.y + 30.0f}, style::kText, "FREE PLAY");
                Text(f, FontStyle::SemiBold, 22.0f, {left, panel.y + 110.0f}, style::kTextDim, "One car and the ball: no clock, no opponents, no score.");
                Text(f, FontStyle::SemiBold, 22.0f, {left, panel.y + 142.0f}, style::kTextDim, "The counters show the speed of your hits and how high you touch the ball.");
                const char* const keys[][2] = {{"R", "Ball back in front of the car"},
                                               {"T", "Ball in the air in front of the car: practise the aerials"},
                                               {"C", "Car back to the start"},
                                               {"B", "Endless boost on / off"},
                                               {"Esc", "Pause"}};
                float y = panel.y + 230.0f;
                for (const auto& key : keys)
                {
                    const float width = KeyCap(f, {left, y}, key[0]);
                    Text(f, FontStyle::SemiBold, 22.0f, {left + std::max(width, 70.0f) + 20.0f, y + 4.0f}, style::kText, key[1]);
                    y += 56.0f;
                }
                Hints(f, {{Glyph::UpDown, Glyph::PadA, "Select"}, {Glyph::Enter, Glyph::None, "Confirm"}, {Glyph::Esc, Glyph::PadB, "Back"}});
            }
            else
            {
                const rl::RlTrainingPack& pack = trainingPacks[static_cast<size_t>(trainingShown - 1)];
                Text(f, FontStyle::BlackItalic, 52.0f, {left, panel.y + 30.0f}, style::kText, Upper(pack.name));
                const int done = trainingResults.PackSuccesses(pack);
                const std::string progress = std::to_string(done) + " / " + std::to_string(pack.exercises.size()) + " DONE";
                Text(f, FontStyle::ExtraBoldItalic, 34.0f, {panel.x + panel.w - slant - 40.0f, panel.y + 40.0f}, done == static_cast<int>(pack.exercises.size()) ? style::kGood : style::kTextDim, progress, Align::Right);
                PushClip(f, {left, panel.y + 104.0f, panel.w - slant - 100.0f, 34.0f});
                Text(f, FontStyle::SemiBold, 20.0f, {left, panel.y + 108.0f}, style::kTextDim, pack.description);
                PopClip(f);
                const float captionY = panel.y + 160.0f;
                ColumnCaption(f, {left + 44.0f, captionY}, "EXERCISE");
                ColumnCaption(f, {left + 560.0f, captionY}, "DONE / TRIES", Align::Center);
                ColumnCaption(f, {left + 760.0f, captionY}, "BEST TIME", Align::Center);
                ColumnCaption(f, {left + 940.0f, captionY}, "BEST HIT", Align::Center);
                float y = captionY + 34.0f;
                for (const rl::RlTrainingExercise& exercise : pack.exercises)
                {
                    const rl::RlTrainingExerciseResult r = trainingResults.Get(pack.id, exercise.id);
                    const bool did = r.successes > 0;
                    const Rect row{left - 14.0f, y, panel.w - 2.0f * slant - 90.0f, 44.0f};
                    FillRect(f, row, style::kNavySolid.Faded(0.7f));
                    Circle(f, {left + 12.0f, y + 22.0f}, 9.0f, did ? style::kGood : style::kNavy3);
                    TextIn(f, FontStyle::SemiBold, 22.0f, {left + 44.0f, y, 500.0f, 44.0f}, did ? style::kText : style::kTextDim, exercise.name);
                    TextIn(f, FontStyle::SemiBold, 22.0f, {left + 480.0f, y, 160.0f, 44.0f}, style::kTextDim, std::to_string(r.successes) + " / " + std::to_string(r.attempts), Align::Center);
                    TextIn(f, FontStyle::SemiBold, 22.0f, {left + 680.0f, y, 160.0f, 44.0f}, did ? style::kText : style::kTextDim, did ? Format("%.1f s", static_cast<double>(r.bestTime)) : std::string("-"), Align::Center);
                    TextIn(f, FontStyle::SemiBold, 22.0f, {left + 860.0f, y, 160.0f, 44.0f}, did ? style::kText : style::kTextDim,
                           did && r.bestHitSpeed > 0.0f ? Format("%d km/h", static_cast<int>(std::lround(r.bestHitSpeed * 3.6f))) : std::string("-"), Align::Center);
                    y += 48.0f;
                }
                Hints(f, {{Glyph::UpDown, Glyph::PadA, "Select"}, {Glyph::Enter, Glyph::None, "Start"}, {Glyph::Esc, Glyph::PadB, "Back"}});
            }
            if (f.input().Pressed(Nav::Back))
            {
                GoTo(f, Screen::Main);
            }
        }

        // ---- the garage (docs/game/rl-cars.md): the podium of the menu shows the car in focus, the bars compare it with the one that is driven

        Screen garageReturn = Screen::Main;     // where Back goes: the main menu, Play, the room, the lobby
        std::string garageFocusCar;             // the car in focus (a key); an equip is a separate act
        int garageTeam = 0;                     // the paint of the preview: 0 blue, 1 orange
        bool garageDragging = false;
        std::string garageClickCar;             // the double click that equips with the mouse
        double garageClickTime = -10.0;
        bool carsPreloaded = false;
        bool carMissingToast = false;           // the car of the file is not in the data any more: Pulse, said once on the main menu
        Id mainDefaultFocus = HashId("main/play");
        std::map<std::string, std::uint64_t> carImages;

        // The profile of a car on a card (the texture id), 0 while it loads
        std::uint64_t CarImage(const game::RlCarDef& car, const int team)
        {
            const std::string& path = car.preview[team];
            if (path.empty() || !svc.loadImage)
            {
                return 0;
            }
            std::uint64_t& id = carImages[path];
            if (id == 0)
            {
                id = svc.loadImage(path);
            }
            return id;
        }

        // The meshes, the paint and the previews of every car are loaded behind the boot screen, so that the carousel does not stutter
        void PreloadCars()
        {
            if (carsPreloaded || !svc.preload)
            {
                return;
            }
            carsPreloaded = true;
            for (const game::RlCarDef& car : game::Cars().Cars())
            {
                svc.preload(car.bodyMesh);
                if (!car.lightsMesh.empty())
                {
                    svc.preload(car.lightsMesh);
                }
                for (int team = 0; team < 2; ++team)
                {
                    svc.preload(car.material[team]);
                    std::string texture = car.material[team];   // assets/materials/rl2/<name>.material.json -> assets/textures/rl2/<name>.tga
                    const std::size_t at = texture.find("/materials/");
                    const std::size_t end = texture.rfind(".material.json");
                    if (at != std::string::npos && end != std::string::npos)
                    {
                        svc.preload(texture.substr(0, at) + "/textures/" + texture.substr(at + 11, end - at - 11) + ".tga");
                    }
                    CarImage(car, team);
                }
            }
        }

        // The car of the settings must be one of the data: a car that was removed is Pulse again (and the main menu says so)
        void CheckCar()
        {
            const game::RlCarCatalog& catalog = game::Cars();
            if (!catalog.Empty() && catalog.Find(settings.car) == nullptr)
            {
                settings.car = catalog.Default() != nullptr ? catalog.Default()->id : std::string("pulse");
                carMissingToast = true;
            }
            System().SetSelectedCar(settings.car);
        }

        void OpenGarage(Frame& f, const Screen from)
        {
            garageReturn = from;
            garageFocusCar = settings.car;
            garageDragging = false;
            garageClickCar.clear();
            GoTo(f, Screen::Garage);
        }

        void LeaveGarage(Frame& f)
        {
            System().SetGarageView({});
            garageDragging = false;
            GoTo(f, garageReturn);
        }

        std::string ClassLine(const game::RlCarDef& car) const { return Upper(car.className); }

        float GarageChip(Frame& f, const float x, const float y, const std::string& text, const Color fill, const Color ink, const bool frame, const Icon icon = Icon::None)
        {
            const float textWidth = TextWidth(f, FontStyle::Bold, 18.0f, text, 0.1f);
            const float iconWidth = icon != Icon::None ? 26.0f : 0.0f;
            const Rect box{x, y, textWidth + iconWidth + 26.0f, 32.0f};
            FillRect(f, box, fill);
            if (frame)
            {
                OutlineRect(f, box, style::kLine, 1.0f);
            }
            if (icon != Icon::None)
            {
                IconGlyph(f, icon, 16.0f, {box.x + 22.0f, box.y + 16.0f}, ink);
            }
            Text(f, FontStyle::Bold, 18.0f, {box.x + 13.0f + iconWidth, box.y + 6.0f}, ink, text, Align::Left, 0.1f);
            return box.w + 8.0f;
        }

        void DrawGarage(Frame& f)
        {
            sys::RlGameSystem& system = System();
            const game::RlCarCatalog& catalog = game::Cars();
            SideShade(f, 900.0f);
            if (catalog.Empty())
            {
                Text(f, FontStyle::BlackItalic, 64.0f, {80.0f, 86.0f}, style::kText, "GARAGE");
                Text(f, FontStyle::SemiBold, 24.0f, {80.0f, 190.0f}, style::kTextDim, "There are no cars in assets/data/cars.");
                if (f.input().Pressed(Nav::Back) || f.input().Pressed(Nav::Confirm))
                {
                    LeaveGarage(f);
                }
                return;
            }
            const std::vector<game::RlCarDef>& list = catalog.Cars();
            const game::RlCarDef* focus = catalog.FindOrDefault(garageFocusCar);
            const game::RlCarDef* equipped = catalog.FindOrDefault(settings.car);
            if (focus == nullptr || equipped == nullptr)
            {
                LeaveGarage(f);
                return;
            }
            const int count = static_cast<int>(list.size());
            const char* from = garageReturn == Screen::Play ? "PLAY" : (garageReturn == Screen::Lobby || garageReturn == Screen::Room ? "LOBBY" : "MAIN MENU");

            // ---- the left column
            Text(f, FontStyle::SemiBold, 24.0f, {80.0f, 58.0f}, style::kTextDim, from);
            Text(f, FontStyle::BlackItalic, 64.0f, {80.0f, 84.0f}, style::kText, "GARAGE");
            Text(f, FontStyle::BlackItalic, 96.0f, {80.0f, 182.0f}, style::kText, Upper(focus->name));
            float chipX = 80.0f;
            const float chipY = 312.0f;
            chipX += GarageChip(f, chipX, chipY, ClassLine(*focus), style::kBlue, style::kText, false);
            chipX += GarageChip(f, chipX, chipY, "HITBOX \xE2\x89\x88 " + Upper(focus->hitboxClass), style::kNavy, style::kTextDim, true);
            if (focus->id == equipped->id)
            {
                GarageChip(f, chipX, chipY, "EQUIPPED", Color::Hex(0x0e3a24), style::kGood, true, Icon::Check);
            }
            Text(f, FontStyle::SemiBold, 20.0f, {80.0f, 358.0f}, style::kTextDim, focus->description);
            const bool compare = focus->id != equipped->id;
            for (int i = 0; i < static_cast<int>(game::RlCarStat::Count); ++i)
            {
                const game::RlCarStat stat = static_cast<game::RlCarStat>(i);
                const float y = 408.0f + 44.0f * static_cast<float>(i);
                Text(f, FontStyle::SemiBold, 26.0f, {80.0f, y - 2.0f}, style::kText, game::RlCarStatName(stat));
                StatBarDesc bar;
                bar.value = catalog.Segments(*focus, stat);
                bar.compare = compare ? catalog.Segments(*equipped, stat) : -1;
                bar.betterIsMore = game::RlCarStatBetterIsMore(stat);
                StatBar(f, {285.0f, y + 6.0f}, bar);
            }
            IconGlyph(f, Icon::Ruler, 18.0f, {90.0f, 646.0f}, style::kTextDim);
            std::string ruler = Format("Hitbox %.2f \xC3\x97 %.2f \xC3\x97 %.2f m", static_cast<double>(focus->Length()), static_cast<double>(focus->Width()), static_cast<double>(focus->HeightOfHitbox()));
            Text(f, FontStyle::SemiBold, 20.0f, {108.0f, 634.0f}, style::kTextDim, ruler);
            if (compare)
            {
                Text(f, FontStyle::SemiBold, 20.0f, {108.0f + TextWidth(f, FontStyle::SemiBold, 20.0f, ruler), 634.0f}, style::kAccent, " \xC2\xB7 compared with " + equipped->name);
            }

            // ---- the team of the preview
            Text(f, FontStyle::SemiBold, 22.0f, {1560.0f, 74.0f}, style::kTextDim, "Preview team", Align::Right);
            const std::string_view teams[] = {"BLUE", "ORANGE"};
            garageTeam = Tabs(f, HashId("garage/team"), {1580.0f, 62.0f, 260.0f, 54.0f}, Span<std::string_view>(teams), garageTeam);

            // ---- the carousel: the cards of 270 x 150 (176 in the focus) in a row, bottom aligned
            const int visible = std::min(count, 6);
            const int focusIndex = std::max(catalog.IndexOf(focus->id), 0);
            int& first = f.IntState(HashId("garage/first"));
            first = std::clamp(first, 0, std::max(count - visible, 0));
            if (focusIndex < first)
            {
                first = focusIndex;
            }
            if (focusIndex >= first + visible)
            {
                first = focusIndex - visible + 1;
            }
            f.SetDefaultFocus(HashId("garage/car/" + equipped->id));
            std::string pendingFocus;
            std::string toEquip;
            const double now = f.Time();
            for (int k = 0; k < visible; ++k)
            {
                const game::RlCarDef& car = list[static_cast<std::size_t>(first + k)];
                const bool focused = car.id == focus->id;
                const Rect rect{70.0f + 286.0f * static_cast<float>(k), focused ? 808.0f : 834.0f, 270.0f, focused ? 176.0f : 150.0f};
                CarCardDesc card;
                card.name = car.name;
                card.klass = car.className;
                card.image = CarImage(car, 0);
                card.equipped = car.id == equipped->id;
                const Id id = HashId("garage/car/" + car.id);
                std::string upperName = Upper(car.name);
                std::string upperClass = ClassLine(car);
                card.name = upperName;
                card.klass = upperClass;
                const bool chosen = CarCard(f, id, rect, card);
                if (f.Focus() == id)
                {
                    pendingFocus = car.id;
                }
                if (chosen && car.id != equipped->id)
                {
                    const bool byMouse = f.input().MousePressed();
                    if (!byMouse || (garageClickCar == car.id && now - garageClickTime < 0.45))
                    {
                        toEquip = car.id;
                    }
                    else
                    {
                        garageClickCar = car.id;
                        garageClickTime = now;
                    }
                }
            }
            if (count > visible)
            {
                if (first > 0)
                {
                    IconGlyph(f, Icon::ChevronLeft, 30.0f, {36.0f, 910.0f}, style::kText);
                }
                if (first + visible < count)
                {
                    IconGlyph(f, Icon::ChevronRight, 30.0f, {1884.0f, 910.0f}, style::kText);
                }
            }
            if (!pendingFocus.empty())
            {
                garageFocusCar = pendingFocus;
            }
            // the wheel of the mouse turns the carousel
            if (f.input().Wheel() != 0.0f)
            {
                const int step = f.input().Wheel() < 0.0f ? 1 : -1;
                const int next = std::clamp(focusIndex + step, 0, count - 1);
                f.SetFocus(HashId("garage/car/" + list[static_cast<std::size_t>(next)].id));
                garageFocusCar = list[static_cast<std::size_t>(next)].id;
            }
            if (!toEquip.empty())
            {
                settings.car = toEquip;
                system.SetSelectedCar(settings.car);
                Save();
                const game::RlCarDef* now_equipped = catalog.Find(toEquip);
                f.ShowToast((now_equipped != nullptr ? now_equipped->name : toEquip) + " equipped", style::kGood);
                Sound("ui_click");
            }

            // ---- the car on the podium: drag with the mouse or the right stick turns it by hand
            const Vec2 mouse = f.ToDesign(f.input().MousePosition());
            if (f.input().MousePressed())
            {
                garageDragging = mouse.x > 700.0f && mouse.y > 130.0f && mouse.y < 790.0f;
            }
            if (garageDragging && f.input().MouseDown())
            {
                const Vec2 before = f.ToDesign(f.input().PreviousRaw().mousePosition);
                const float dx = mouse.x - before.x;
                if (dx != 0.0f)
                {
                    system.RotateGarageCar(dx * 0.5f);
                }
            }
            else
            {
                garageDragging = false;
            }
            const float stick = f.input().Raw().padAxes[2];
            if (std::fabs(stick) > 0.25f)
            {
                system.RotateGarageCar(stick * 140.0f * f.DeltaTime());
            }
            sys::RlGarageView view;
            view.active = true;
            view.car = garageFocusCar;
            view.team = garageTeam;
            system.SetGarageView(view);

            Hints(f, {{Glyph::LeftRight, Glyph::PadA, compare ? "Equip" : "Equipped"}, {Glyph::QE, Glyph::PadLBRB, "Team colour"}, {Glyph::Esc, Glyph::PadB, "Back"}});
            float hintX = 1840.0f - TextWidth(f, FontStyle::SemiBold, 22.0f, "Rotate") - 14.0f;
            Text(f, FontStyle::SemiBold, 22.0f, {hintX, 1010.0f}, style::kTextDim, "Rotate");
            hintX -= 12.0f + TextWidth(f, FontStyle::Bold, 20.0f, "Drag") + 24.0f;
            KeyCap(f, {hintX, 1006.0f}, "Drag");

            if (f.input().Pressed(Nav::Back))
            {
                LeaveGarage(f);
            }
        }

        // ---- the lobby of a private match

        int lobbyEnteredAt = 0;
        std::string joinedAddress;

        void DrawLobby(Frame& f)
        {
            const sys::RlGameStatus& status = System().Status();
            Backdrop(f, 0.93f);
            const bool hosting = status.isHost;
            ScreenTitle(f, "PRIVATE MATCH", hosting ? "LOBBY" : "JOINING");
            Text(f, FontStyle::SemiBold, 24.0f, {80.0f, 238.0f}, style::kTextDim, status.statusText.empty() ? "Waiting ..." : status.statusText);

            // the two teams side by side
            const float panelHeight = 76.0f + 62.0f * static_cast<float>(std::max(status.setup.mode, 1)) + 14.0f;
            const Rect blue{80.0f, 300.0f, 800.0f, panelHeight};
            const Rect orange{1040.0f, 300.0f, 800.0f, panelHeight};
            for (int team = 0; team < 2; ++team)
            {
                const Rect r = team == 0 ? blue : orange;
                Panel(f, r, false, true);
                ParallelogramGradient(f, {r.x, r.y, r.w, 56.0f}, style::TeamColor(team), style::TeamDark(team));
                Text(f, FontStyle::BlackItalic, 34.0f, {r.x + 28.0f, r.y + 8.0f}, style::kText, team == 0 ? "BLUE" : "ORANGE");
                float y = r.y + 76.0f;
                for (const sys::RlLobbySlot& slot : status.lobby)
                {
                    if (slot.team != team)
                    {
                        continue;
                    }
                    const Rect row{r.x + 20.0f, y, r.w - 40.0f, 54.0f};
                    Parallelogram(f, row, slot.you ? style::kNavy3 : style::kNavy2);
                    IconGlyph(f, slot.bot ? Icon::Robot : Icon::Person, 24.0f, {row.x + 34.0f, row.y + 27.0f}, slot.name.empty() ? style::kTextFaint : style::kText);
                    TextIn(f, FontStyle::Bold, 24.0f, {row.x + 70.0f, row.y, 250.0f, row.h}, slot.name.empty() ? style::kTextFaint : style::kText,
                           slot.name.empty() ? "Open slot" : slot.name + (slot.you ? "  (you)" : ""));
                    if (const game::RlCarDef* slotCar = slot.name.empty() ? nullptr : game::Cars().FindOrDefault(slot.car.empty() ? std::string("pulse") : slot.car))
                    {
                        // the car of the slot: its profile, the name and the class
                        Image(f, CarImage(*slotCar, team), {row.x + 330.0f, row.y + 3.0f, 100.0f, 48.0f});
                        Text(f, FontStyle::BlackItalic, 26.0f, {row.x + 442.0f, row.y + 4.0f}, style::kText, Upper(slotCar->name));
                        Text(f, FontStyle::SemiBold, 16.0f, {row.x + 442.0f, row.y + 33.0f}, style::kTextDim, Upper(slotCar->className), Align::Left, 0.1f);
                    }
                    if (slot.host)
                    {
                        TextIn(f, FontStyle::Bold, 16.0f, {row.x + row.w - 190.0f, row.y, 100.0f, row.h}, style::kAccent, "HOST", Align::Right);
                    }
                    if (slot.pingMs >= 0)
                    {
                        TextIn(f, FontStyle::SemiBold, 20.0f, {row.x + row.w - 110.0f, row.y, 90.0f, row.h}, slot.pingMs < 60 ? style::kGood : (slot.pingMs < 120 ? style::kMvp : style::kBad),
                               Format("%d ms", slot.pingMs), Align::Right);
                    }
                    y += 62.0f;
                }
            }

            f.SetDefaultFocus(HashId("lobby/leave"));
            // Change car under the team of the player: the garage opens over the lobby, Back returns (G / Y do the same)
            if (status.stage == sys::RlGameStage::Lobby && !lobbyAskLeave)
            {
                if (ActionButton(f, HashId("lobby/car"), {80.0f, 300.0f + panelHeight + 26.0f, 400.0f, 72.0f}, "CHANGE CAR", Icon::Car) || f.input().KeyPressed('G') ||
                    f.input().Pressed(Nav::ResetRow))
                {
                    OpenGarage(f, Screen::Lobby);
                    return;
                }
            }
            int humans = 0;
            for (const sys::RlLobbySlot& slot : status.lobby)
            {
                humans += (!slot.bot && !slot.name.empty()) ? 1 : 0;
            }
            const int slots = status.setup.mode * 2;
            if (autoStartHumans > 0 && hosting && status.stage == sys::RlGameStage::Lobby && (humans >= autoStartHumans || screenTime > 12.0f))
            {
                Log(Format("the players of the lobby are in (%d of %d): starting the match", humans, autoStartHumans));
                autoStartHumans = 0;
                System().StartHostedMatch();
            }
            if (hosting && status.stage == sys::RlGameStage::Lobby)
            {
                const bool canStart = status.setup.fillWithBots || humans >= slots;
                f.SetDefaultFocus(HashId("lobby/start"));
                if (ActionButton(f, HashId("lobby/start"), {1200.0f, 900.0f, 360.0f, 76.0f}, "START MATCH", Icon::Play, canStart, true))
                {
                    System().StartHostedMatch();
                }
            }
            else if (!hosting && status.stage == sys::RlGameStage::Lobby)
            {
                Text(f, FontStyle::SemiBold, 24.0f, {1840.0f, 925.0f}, style::kTextDim, "Waiting for the host ...", Align::Right);
            }
            if (ActionButton(f, HashId("lobby/leave"), {hosting ? 1580.0f : 1460.0f, 900.0f, hosting ? 260.0f : 380.0f, 76.0f}, hosting ? "LEAVE" : "CANCEL", Icon::Exit) ||
                (f.input().Pressed(Nav::Back) && !lobbyAskLeave))
            {
                System().LeaveMatch();
                GoTo(f, Screen::Play);
            }
            Hints(f, {{Glyph::AllArrows, Glyph::PadA, "Select"}, {Glyph::G, Glyph::PadY, "Garage"}, {Glyph::Esc, Glyph::PadB, "Leave"}});

            if (status.stage == sys::RlGameStage::Playing)
            {
                if (!joinedAddress.empty())
                {
                    auto& recent = settings.play.recent;
                    recent.erase(std::remove(recent.begin(), recent.end(), joinedAddress), recent.end());
                    recent.insert(recent.begin(), joinedAddress);
                    settings.Clamp();
                    joinedAddress.clear();
                    Save();
                }
                hud.Reset();
                GoTo(f, Screen::Match);
            }
            else if (status.stage == sys::RlGameStage::Menu && frames > lobbyEnteredAt + 8)
            {
                GoTo(f, Screen::Play);  // the network left by itself
            }
        }

        // ---- the match

        void DrawMatch(Frame& f)
        {
            sys::RlGameSystem& system = System();
            const sys::RlGameStatus& status = system.Status();
            if (status.stage == sys::RlGameStage::Menu && screenTime > 0.4f)
            {
                GoTo(f, Screen::Main);
                return;
            }
            if (status.stage == sys::RlGameStage::Finished)
            {
                GoTo(f, Screen::Results);
                return;
            }
            if (system.TakePausePressed())
            {
                system.SetPaused(true);
                GoTo(f, Screen::Pause);
                return;
            }
            HudState state = status.hud;
            state.showFps = settings.video.showFps;
            state.fps = svc.fps ? svc.fps() : 0.0f;
            state.ballCam = status.ballCam;
            hud.Update(f.DeltaTime(), state);
            for (const HudEvent& event : system.TakeHudEvents())
            {
                hud.PushEvent(event);
            }
            f.SetSpace(Space::Screen);
            hud.Draw(f, status.scoreboardHeld);
            f.SetSpace(Space::Menu);
        }

        // ---- pause

        std::string MatchLine(const sys::RlGameStatus& status) const
        {
            if (status.setup.kind == sys::RlGameMatchSetup::Kind::Training)
            {
                return "TRAINING \xC2\xB7 " + Upper(status.hud.training.pack);
            }
            std::string text = status.online ? "ONLINE" : "OFFLINE";
            text += " \xC2\xB7 ";
            text += status.setup.mode == 0 ? "TRAINING" : kModeNames[std::clamp(status.setup.mode, 0, 4)];
            if (!status.online && status.setup.mode != 0)
            {
                text += " \xC2\xB7 ";
                text += kBotLevels[std::clamp(status.setup.botLevel, 0, 2)];
                text += " BOTS";
            }
            return text;
        }

        void DrawPause(Frame& f)
        {
            sys::RlGameSystem& system = System();
            const sys::RlGameStatus& status = system.Status();
            if (status.stage == sys::RlGameStage::Menu || status.stage == sys::RlGameStage::Finished)
            {
                GoTo(f, status.stage == sys::RlGameStage::Finished ? Screen::Results : Screen::Main);
                return;
            }
            f.SetSpace(Space::Screen);
            Dim(f, Color::Hex(0x030814, 0.45f));
            hud.Draw(f, false, true);
            f.SetSpace(Space::Menu);
            Text(f, FontStyle::BlackItalic, 64.0f, {80.0f, 236.0f}, style::kText, "PAUSED");
            const bool dialogOpen = askLeave || askRestart;
            f.SetDefaultFocus(HashId("pause/resume"));
            Column column({76.0f, 350.0f, style::kMenuButtonWidth, style::kMenuButtonHeight}, style::kMenuButtonStep);
            const bool resume = MenuButton(f, HashId("pause/resume"), column.Next(), "RESUME", Icon::Play);
            if (MenuButton(f, HashId("pause/settings"), column.Next(), "SETTINGS", Icon::Settings))
            {
                settingsReturn = Screen::Pause;
                tab = 0;
                GoTo(f, Screen::Settings);
                return;
            }
            const bool training = status.setup.kind == sys::RlGameMatchSetup::Kind::Training;
            if (MenuButton(f, HashId("pause/restart"), column.Next(), training ? "RESTART TRAINING" : "RESTART MATCH", Icon::Refresh, !status.online))
            {
                askRestart = true;
            }
            if (MenuButton(f, HashId("pause/leave"), column.Next(), training ? "LEAVE TRAINING" : "LEAVE MATCH", Icon::Exit))
            {
                askLeave = true;
            }
            Panel(f, {1258.0f, 280.0f, 565.0f, 220.0f}, false, true);
            Text(f, FontStyle::SemiBold, 22.0f, {1314.0f, 312.0f}, style::kTextDim, MatchLine(status));
            if (status.setup.mode != 0)
            {
                float x = 1314.0f;
                const std::string blue = std::to_string(status.hud.score[0]);
                const std::string orange = std::to_string(status.hud.score[1]);
                Text(f, FontStyle::BlackItalic, 52.0f, {x, 348.0f}, style::kBlueHi, blue);
                x += TextWidth(f, FontStyle::BlackItalic, 52.0f, blue) + 18.0f;
                Text(f, FontStyle::BlackItalic, 52.0f, {x, 348.0f}, style::kText, "\xE2\x80\x94");
                x += TextWidth(f, FontStyle::BlackItalic, 52.0f, "\xE2\x80\x94") + 18.0f;
                Text(f, FontStyle::BlackItalic, 52.0f, {x, 348.0f}, style::kOrangeHi, orange);
                x += TextWidth(f, FontStyle::BlackItalic, 52.0f, orange) + 22.0f;
                Text(f, FontStyle::BlackItalic, 34.0f, {x, 364.0f}, style::kTextDim, "\xC2\xB7 " + hud.ClockText());
            }
            else
            {
                Text(f, FontStyle::BlackItalic, 52.0f, {1314.0f, 348.0f}, style::kText, "TRAINING");
            }
            Text(f, FontStyle::SemiBold, 18.0f, {1314.0f, 420.0f}, style::kTextDim,
                 status.online ? "Online: the match goes on behind this menu." : "Offline: the match is frozen.");
            Hints(f, {{Glyph::UpDown, Glyph::PadA, "Select"}, {Glyph::Esc, Glyph::PadB, "Resume"}});

            if (askLeave)
            {
                const DialogResult result = ConfirmDialog(f, HashId("pause/leave-dialog"),
                                                          {training ? "LEAVE THE TRAINING?" : "LEAVE THE MATCH?",
                                                           training ? "Your results are saved." : (status.online ? "You will leave the match for good." : "The match is lost."), "Leave", "Stay"});
                if (result == DialogResult::Confirm)
                {
                    system.LeaveMatch();
                    if (training)
                    {
                        OpenTraining(f);
                    }
                    else
                    {
                        GoTo(f, Screen::Main);
                    }
                    return;
                }
                if (result == DialogResult::Cancel)
                {
                    askLeave = false;
                }
            }
            else if (askRestart)
            {
                const DialogResult result = ConfirmDialog(f, HashId("pause/restart-dialog"),
                                                          {training ? "RESTART THE TRAINING?" : "RESTART THE MATCH?",
                                                           training ? "The pack starts again from its first exercise." : "The score and the clock start over.", "Restart", "Cancel"});
                if (result == DialogResult::Confirm)
                {
                    sys::RlGameMatchSetup setup = status.setup;
                    BeginLoading(f, setup);
                    return;
                }
                if (result == DialogResult::Cancel)
                {
                    askRestart = false;
                }
            }
            if (!dialogOpen && (resume || f.input().Pressed(Nav::Back)))
            {
                system.SetPaused(false);
                GoTo(f, Screen::Match);
            }
        }

        // ---- results

        void DrawResults(Frame& f)
        {
            sys::RlGameSystem& system = System();
            const sys::RlGameStatus& status = system.Status();
            if (status.stage == sys::RlGameStage::Menu)
            {
                GoTo(f, Screen::Main);
                return;
            }
            const int winner = status.winner < 0 ? 0 : status.winner;
            f.SetSpace(Space::Screen);
            const Vec2 window = f.metrics().designWindow;
            FillRectGradientV(f, {0.0f, window.y * 0.46f, window.x, window.y * 0.54f}, Color::Hex(0x050b1c, 0.0f), Color::Hex(0x050b1c, 0.88f));
            f.SetSpace(Space::Menu);

            const Rect banner{520.0f, 60.0f, 880.0f, 120.0f};
            ParallelogramGradient3(f, banner, style::TeamColor(winner).Faded(0.1f), style::TeamColor(winner), style::TeamColor(winner).Faded(0.1f));
            Text(f, FontStyle::BlackItalic, 96.0f, {960.0f, 58.0f}, style::kText, winner == 0 ? "BLUE WINS" : "ORANGE WINS", Align::Center);
            const std::string blueScore = std::to_string(status.hud.score[0]);
            const std::string orangeScore = std::to_string(status.hud.score[1]);
            const float blueWidth = TextWidth(f, FontStyle::BlackItalic, 64.0f, blueScore);
            const float orangeWidth = TextWidth(f, FontStyle::BlackItalic, 64.0f, orangeScore);
            const float total = blueWidth + 70.0f + orangeWidth;
            float x = 960.0f - total * 0.5f;
            Text(f, FontStyle::BlackItalic, 64.0f, {x, 208.0f}, style::kBlueHi, blueScore);
            FillRect(f, {x + blueWidth + 18.0f, 244.0f, 34.0f, 6.0f}, style::kTextDim);
            Text(f, FontStyle::BlackItalic, 64.0f, {x + blueWidth + 70.0f, 208.0f}, style::kOrangeHi, orangeScore);
            if (status.overtimePlayed)
            {
                const Rect ot{960.0f + total * 0.5f + 20.0f, 238.0f, 48.0f, 32.0f};
                FillRect(f, ot, style::kMvp);
                TextIn(f, FontStyle::Bold, 16.0f, ot, Color::Hex(0x08142c), "OT", Align::Center);
            }
            if (status.mvp >= 0 && status.mvp < static_cast<int>(status.hud.players.size()))
            {
                const std::string name = Upper(status.hud.players[static_cast<size_t>(status.mvp)].name);
                const float width = TextWidth(f, FontStyle::ExtraBoldItalic, 26.0f, "MVP \xC2\xB7 " + name) + 100.0f;
                const Rect mvp{1470.0f, 330.0f, std::max(width, 258.0f), 48.0f};
                Parallelogram(f, mvp, style::kMvp);
                IconGlyph(f, Icon::Star, 24.0f, {mvp.x + 44.0f, mvp.y + 24.0f}, Color::Hex(0x08142c));
                TextIn(f, FontStyle::ExtraBoldItalic, 26.0f, {mvp.x + 66.0f, mvp.y, mvp.w - 66.0f, mvp.h}, Color::Hex(0x08142c), "MVP \xC2\xB7 " + name);
            }
            if (const game::RlCarDef* car = game::Cars().FindOrDefault(system.SelectedCar()))
            {
                // the podium shows the car of the winner: its name under the MVP chip
                if (status.winner >= 0 && !status.hud.players.empty() && status.hud.localPlayer >= 0 && status.hud.localPlayer < static_cast<int>(status.hud.players.size()) &&
                    status.hud.players[static_cast<std::size_t>(status.hud.localPlayer)].team == status.winner)
                {
                    Text(f, FontStyle::ExtraBoldItalic, 26.0f, {1470.0f, 392.0f}, style::kText, Upper(car->name) + " \xC2\xB7 " + Upper(car->className));
                }
            }
            HudState table = status.hud;
            table.phase = HudPhase::Finished;
            const int count = static_cast<int>(table.players.size());
            const float top = std::min(690.0f, 930.0f - 56.0f * static_cast<float>(count) - 40.0f);
            Hud::DrawScoreTable(f, {360.0f, top, 1200.0f, 0.0f}, table, status.online, false, false);

            f.SetDefaultFocus(HashId("results/again"));
            const bool canPlayAgain = !status.online || status.isHost;
            if (ActionButton(f, HashId("results/again"), {1208.0f, 982.0f, 314.0f, 66.0f}, canPlayAgain ? "PLAY AGAIN" : "WAITING FOR HOST", Icon::Refresh, canPlayAgain, true))
            {
                if (status.online)
                {
                    system.RestartMatch();
                    lobbyEnteredAt = frames;
                    GoTo(f, Screen::Lobby);
                }
                else
                {
                    BeginLoading(f, status.setup);
                }
                return;
            }
            if (ActionButton(f, HashId("results/menu"), {1532.0f, 982.0f, 310.0f, 66.0f}, "MAIN MENU", Icon::Home) || f.input().Pressed(Nav::Back))
            {
                system.LeaveMatch();
                GoTo(f, Screen::Main);
            }
        }

        // ---- settings

        void ApplyCamera()
        {
            sys::RlGameSystem& system = System();
            system.SetCameraSettings(settings.camera.ToTuning(system.CameraSettings()), settings.camera.ballCamHold);
        }

        void ApplyVideo(const bool windowChange)
        {
            if (windowChange && !videoPending)
            {
                videoPending = true;
                videoConfirm = 15.0f;
                videoBackup = pendingBefore;
            }
            if (svc.applyVideo)
            {
                svc.applyVideo(settings.video);
            }
        }

        RlVideoSettings pendingBefore;

        // ---- the bindings of the controls: reading and writing the tokens of RlControls

        std::vector<std::string> TokensOf(const Slot& slot)
        {
            rl::RlControls& c = System().Controls();
            return slot.axis ? c.AxisTokens(static_cast<rl::RlAxisAction>(slot.action)) : c.ButtonTokens(static_cast<rl::RlButtonAction>(slot.action));
        }

        // The token of a slot on a device ("" when none). A stick that binds a whole axis counts for both halves of it.
        std::string SlotToken(const Slot& slot, const bool pad)
        {
            for (const std::string& token : TokensOf(slot))
            {
                if (IsPadToken(token) != pad)
                {
                    continue;
                }
                if (!slot.axis || IsStick(token) || SignOf(token) == slot.sign)
                {
                    return token;
                }
            }
            return {};
        }

        void WriteTokens(const Slot& slot, const std::vector<std::string>& tokens)
        {
            rl::RlControls& c = System().Controls();
            if (slot.axis)
            {
                c.SetAxis(static_cast<rl::RlAxisAction>(slot.action), tokens);
            }
            else
            {
                c.SetButton(static_cast<rl::RlButtonAction>(slot.action), tokens);
            }
        }

        // Replaces what the slot has on the device by `token` (empty: removes it). `halves`: the row has two halves of one axis (steer, pitch, roll);
        // `whole`: the token is a stick that binds both of them.
        void SetSlotToken(const Slot& slot, const bool pad, const std::string& token, const bool halves, const bool whole)
        {
            std::vector<std::string> result;
            for (const std::string& t : TokensOf(slot))
            {
                if (IsPadToken(t) != pad)
                {
                    result.push_back(t);
                    continue;
                }
                if (!slot.axis || whole || (halves && IsStick(t)) || SignOf(t) == slot.sign)
                {
                    continue;  // replaced
                }
                result.push_back(t);
            }
            if (!token.empty())
            {
                result.push_back(token);
            }
            WriteTokens(slot, result);
        }

        std::string PlateText(const ControlRowDef& row, const bool pad)
        {
            std::vector<std::string> names;
            for (int i = 0; i < row.slotCount; ++i)
            {
                const std::string token = SlotToken(row.slots[i], pad);
                if (token.empty())
                {
                    continue;
                }
                const std::string name = TokenDisplayName(token);
                if (std::find(names.begin(), names.end(), name) == names.end())
                {
                    names.push_back(name);
                }
            }
            if (names.empty())
            {
                return "\xE2\x80\x94";
            }
            std::string text = names[0];
            for (size_t i = 1; i < names.size(); ++i)
            {
                text += " \xC2\xB7 " + names[i];
            }
            return text;
        }

        // The physical direction of the control a token stands for, for the slot (a key is +1; "-PadLT" on the negative half is the trigger itself)
        static int RawSign(const Slot& slot, const std::string& token) { return slot.axis ? SignOf(token) * slot.sign : 1; }

        // The new binding of a slot from the control the player pressed. Returns false when the control cannot be used (nothing changes).
        bool ApplyBinding(Frame& f, const int rowIndex, const bool pad, const int step, const std::string& captured)
        {
            const ControlRowDef& row = kControlRows[rowIndex];
            const Slot slot = row.slots[step];
            const bool halves = row.slotCount == 2;
            bool whole = false;
            std::string token;
            if (slot.axis)
            {
                if (pad && halves && IsStick(captured))
                {
                    whole = true;
                    token = captured;  // the stick moved to the positive side of the axis
                }
                else
                {
                    token = WithSign(Bare(captured), slot.sign * SignOf(captured));
                }
            }
            else
            {
                if (SignOf(captured) < 0)
                {
                    return false;
                }
                token = captured;
            }
            // the control is used by another binding on the same device: they swap
            const std::string oldToken = SlotToken(slot, pad);
            Log(std::string("rebind ") + row.label + (pad ? " (pad) " : " (keyboard) ") + oldToken + " -> " + token);
            std::string swapped;
            for (int r = 0; r < kControlRowCount && swapped.empty(); ++r)
            {
                for (int i = 0; i < kControlRows[r].slotCount; ++i)
                {
                    if (r == rowIndex && i == step)
                    {
                        continue;
                    }
                    const Slot other = kControlRows[r].slots[i];
                    const std::string otherToken = SlotToken(other, pad);
                    if (otherToken.empty() || Bare(otherToken) != Bare(token))
                    {
                        continue;
                    }
                    const bool otherHalves = kControlRows[r].slotCount == 2;
                    std::string moved;
                    if (!oldToken.empty() && !(IsStick(oldToken) && halves))
                    {
                        moved = other.axis ? WithSign(Bare(oldToken), other.sign * RawSign(slot, oldToken)) : Bare(oldToken);
                    }
                    SetSlotToken(other, pad, moved, otherHalves, IsStick(otherToken) && otherHalves);
                    swapped = std::string(kControlRows[r].leftName);
                    f.ShowToast(Format("%s: %s <-> %s", kControlRows[r].label, TokenDisplayName(token).c_str(), row.label));
                    break;
                }
            }
            SetSlotToken(slot, pad, token, halves, whole);
            return true;
        }

        void ResetSlot(const int rowIndex, const bool pad)
        {
            const ControlRowDef& row = kControlRows[rowIndex];
            const rl::RlControls defaults = rl::RlControls::Defaults();
            for (int i = 0; i < row.slotCount; ++i)
            {
                const Slot slot = row.slots[i];
                std::string wanted;
                const std::vector<std::string> tokens = slot.axis ? defaults.AxisTokens(static_cast<rl::RlAxisAction>(slot.action))
                                                                   : defaults.ButtonTokens(static_cast<rl::RlButtonAction>(slot.action));
                for (const std::string& t : tokens)
                {
                    if (IsPadToken(t) == pad && (!slot.axis || IsStick(t) || SignOf(t) == slot.sign))
                    {
                        wanted = t;
                        break;
                    }
                }
                SetSlotToken(slot, pad, wanted, row.slotCount == 2, IsStick(wanted) && row.slotCount == 2);
            }
        }

        void ClearSlot(const int rowIndex, const bool pad)
        {
            const ControlRowDef& row = kControlRows[rowIndex];
            for (int i = 0; i < row.slotCount; ++i)
            {
                SetSlotToken(row.slots[i], pad, std::string(), row.slotCount == 2, true);
            }
        }

        int lastControlsFocus = 0;

        void DrawControls(Frame& f)
        {
            sys::RlGameSystem& system = System();
            ColumnCaption(f, {106.0f, 292.0f}, "ACTION");
            ColumnCaption(f, {526.0f, 292.0f}, "KEYBOARD / MOUSE");
            ColumnCaption(f, {756.0f, 292.0f}, "GAMEPAD");
            f.SetDefaultFocus(HashId("controls/row5"));
            Column rows({80.0f, 322.0f, 1100.0f, style::kRowHeight}, 62.0f);
            int focusedRow = lastControlsFocus;
            const bool locked = rebind.active || askResetControls;
            for (int i = 0; i < kControlRowCount; ++i)
            {
                const ControlRowDef& row = kControlRows[i];
                const BindRowResult r = BindRow(f, HashId("controls/row" + std::to_string(i)), rows.Next(), row.label, PlateText(row, false), PlateText(row, true));
                if (r.focused)
                {
                    focusedRow = i;
                }
                if (locked)
                {
                    continue;
                }
                if (r.rebind)
                {
                    rebind = Rebind{};
                    rebind.active = true;
                    rebind.row = i;
                    rebind.plate = r.plate;
                    rebind.timer = 5.0f;
                    rebindStartFrame = frames;
                }
                else if (r.clear)
                {
                    ClearSlot(i, r.plate == 1);
                    system.SaveControls();
                }
                else if (r.resetRow)
                {
                    ResetSlot(i, r.plate == 1);
                    system.SaveControls();
                }
            }
            lastControlsFocus = focusedRow;
            const ControlRowDef& focused = kControlRows[focusedRow];
            Panel(f, {1245.0f, 290.0f, 640.0f, 360.0f}, false, true);
            Text(f, FontStyle::ExtraBoldItalic, 26.0f, {1315.0f, 322.0f}, style::kText, focused.title);
            Text(f, FontStyle::SemiBold, 20.0f, {1315.0f, 360.0f}, style::kTextDim, focused.help);
            const bool invert = ToggleRow(f, HashId("controls/invert"), {1315.0f, 430.0f, 500.0f, 58.0f}, "Invert pitch", settings.invertPitch);
            if (invert != settings.invertPitch)
            {
                settings.invertPitch = invert;
                system.SetInvertPitch(invert);
            }
            const float dead = SliderRow(f, HashId("controls/deadzone"), {1315.0f, 494.0f, 500.0f, 58.0f}, "Pad deadzone", system.Controls().padDeadzone, 0.05f, 0.5f, 0.05f,
                                         Format("%.2f", system.Controls().padDeadzone));
            if (std::fabs(dead - system.Controls().padDeadzone) > 1e-4f)
            {
                system.Controls().padDeadzone = dead;
                system.SaveControls();
            }
            if (ActionButton(f, HashId("controls/reset"), {1275.0f, 880.0f, 580.0f, 72.0f}, "RESET TO DEFAULTS", Icon::Refresh, !locked))
            {
                askResetControls = true;
            }
            Hints(f, {{Glyph::Enter, Glyph::PadA, "Rebind"}, {Glyph::Del, Glyph::PadX, "Clear"}, {Glyph::R, Glyph::PadY, "Reset row"}, {Glyph::Esc, Glyph::PadB, "Back"}});

            if (askResetControls)
            {
                const DialogResult result = ConfirmDialog(f, HashId("controls/reset-dialog"), {"RESET THE CONTROLS?", "All the bindings return to the defaults.", "Reset", "Cancel"});
                if (result == DialogResult::Confirm)
                {
                    system.ResetControls();
                    askResetControls = false;
                }
                else if (result == DialogResult::Cancel)
                {
                    askResetControls = false;
                }
            }
            if (rebind.active)
            {
                UpdateRebind(f);
            }
        }

        int rebindStartFrame = 0;

        void UpdateRebind(Frame& f)
        {
            sys::RlGameSystem& system = System();
            const ControlRowDef& row = kControlRows[rebind.row];
            const bool pad = rebind.plate == 1;
            const RawInput& now = f.input().Raw();
            const RawInput& before = f.input().PreviousRaw();
            rebind.timer -= f.DeltaTime();
            const bool escape = now.keys[VK_ESCAPE] && !before.keys[VK_ESCAPE];
            const bool del = now.keys[VK_DELETE] && !before.keys[VK_DELETE];
            const bool armed = frames > rebindStartFrame + 1;
            bool finished = false;
            if (escape || rebind.timer <= 0.0f)
            {
                rebind.active = false;  // cancelled
                return;
            }
            if (armed)
            {
                if (del)
                {
                    SetSlotToken(row.slots[rebind.step], pad, std::string(), row.slotCount == 2, true);
                    finished = true;
                }
                else
                {
                    const std::string captured = CaptureToken(before, now, pad);
                    if (!captured.empty() && ApplyBinding(f, rebind.row, pad, rebind.step, captured))
                    {
                        // a stick takes the whole axis; a button of a row of two halves goes on to the second half
                        finished = true;
                        const bool wholeStick = pad && row.slotCount == 2 && IsStick(captured);
                        if (row.slotCount == 2 && rebind.step == 0 && !wholeStick)
                        {
                            finished = false;
                            rebind.step = 1;
                            rebind.timer = 5.0f;
                            rebindStartFrame = frames;
                        }
                    }
                }
            }
            if (finished)
            {
                rebind.active = false;
                system.SaveControls();
                return;
            }
            const char* name = rebind.step == 0 ? row.leftName : row.rightName;
            const std::string header = pad ? "REBIND \xC2\xB7 GAMEPAD" : "REBIND \xC2\xB7 KEYBOARD / MOUSE";
            std::string prompt = pad ? "Press a gamepad button or move a stick / trigger..." : "Press a key or a mouse button...";
            if (pad && row.slotCount == 2 && rebind.step == 0)
            {
                prompt = std::string("Move a stick towards ") + (row.slots[0].action == Axis(rl::RlAxisAction::Pitch) ? "NOSE UP" : (row.slots[0].action == Axis(rl::RlAxisAction::Roll) ? "RIGHT" : "RIGHT")) +
                         ", or press a button for " + row.leftName;
            }
            RebindDialog(f, {header, name, prompt, "Esc \xE2\x80\x94 cancel \xC2\xB7 Del \xE2\x80\x94 clear", "", std::max(rebind.timer / 5.0f, 0.0f)});
        }

        void DrawCamera(Frame& f)
        {
            f.SetDefaultFocus(HashId("camera/fov"));
            Column rows({80.0f, 290.0f, 840.0f, style::kRowHeight}, 62.0f);
            RlCameraSettings& c = settings.camera;
            const auto slider = [&](const char* id, const char* label, float& value, const float lo, const float hi, const float step, const char* format)
            {
                const float next = SliderRow(f, HashId(id), rows.Next(), label, value, lo, hi, step, Format(format, static_cast<double>(value)));
                if (std::fabs(next - value) > 1e-5f)
                {
                    value = next;
                    ApplyCamera();
                }
            };
            slider("camera/fov", "Field of view", c.fov, 60.0f, 110.0f, 1.0f, "%.0f");
            slider("camera/distance", "Distance", c.distanceCm, 100.0f, 400.0f, 10.0f, "%.0f");
            slider("camera/height", "Height", c.heightCm, 40.0f, 200.0f, 10.0f, "%.0f");
            slider("camera/angle", "Angle", c.angle, -15.0f, 0.0f, 0.5f, "%.1f");
            slider("camera/stiffness", "Stiffness", c.stiffness, 0.0f, 1.0f, 0.05f, "%.2f");
            slider("camera/swivel", "Swivel speed", c.swivelSpeed, 1.0f, 10.0f, 0.1f, "%.1f");
            slider("camera/transition", "Transition speed", c.transitionSpeed, 1.0f, 2.0f, 0.1f, "%.1f");
            {
                // "Wall tilt": how much the camera leans with the wall and the ceiling (0: the horizon is always level)
                const float percent = SliderRow(f, HashId("camera/walltilt"), rows.Next(), "Wall tilt", c.wallTilt * 100.0f, 0.0f, 100.0f, 5.0f,
                                                Format("%d%%", static_cast<int>(std::lround(c.wallTilt * 100.0f))));
                if (std::fabs(percent / 100.0f - c.wallTilt) > 1e-4f)
                {
                    c.wallTilt = percent / 100.0f;
                    ApplyCamera();
                }
            }
            const std::string_view ball[] = {"Toggle", "Hold"};
            const int mode = OptionRow(f, HashId("camera/ball"), rows.Next(), "Ball cam", Span<std::string_view>(ball), c.ballCamHold ? 1 : 0);
            if ((mode == 1) != c.ballCamHold)
            {
                c.ballCamHold = mode == 1;
                ApplyCamera();
            }
            ToggleRow(f, HashId("camera/shake"), rows.Next(), "Camera shake", false, false);
            Chip(f, {1850.0f, 60.0f}, "LIVE PREVIEW", Icon::Eye);
            if (ActionButton(f, HashId("camera/reset"), {80.0f, 940.0f, 420.0f, 60.0f}, "RESET TO DEFAULTS", Icon::Refresh))
            {
                askResetTab = true;
            }
            Hints(f, {{Glyph::UpDown, Glyph::PadA, "Select"}, {Glyph::LeftRight, Glyph::None, "Change"}, {Glyph::Esc, Glyph::PadB, "Back"}});
            if (askResetTab)
            {
                const DialogResult result = ConfirmDialog(f, HashId("camera/reset-dialog"), {"RESET THE CAMERA?", "The camera returns to the defaults.", "Reset", "Cancel"});
                if (result == DialogResult::Confirm)
                {
                    const bool keepHold = false;
                    c = RlCameraSettings::FromTuning(rl::RlCameraTuning{});
                    c.ballCamHold = keepHold;
                    ApplyCamera();
                    askResetTab = false;
                }
                else if (result == DialogResult::Cancel)
                {
                    askResetTab = false;
                }
            }
        }

        void DrawVideo(Frame& f)
        {
            // asking the display for its modes takes a while: once for each visit of the tab
            if (!resolutionsRead)
            {
                resolutions = svc.resolutions ? svc.resolutions() : std::vector<std::pair<std::uint32_t, std::uint32_t>>{};
                resolutionsRead = true;
            }
            RlVideoSettings& v = settings.video;
            const RlVideoSettings before = v;
            f.SetDefaultFocus(HashId("video/mode"));
            Column rows({80.0f, 290.0f, 1100.0f, 64.0f}, 68.0f);
            const bool locked = videoPending || askResetTab;
            const std::string_view modes[] = {"Windowed", "Borderless", "Fullscreen"};
            const int mode = OptionRow(f, HashId("video/mode"), rows.Next(), "Window mode", Span<std::string_view>(modes), v.windowMode, !locked);
            // the resolution: the sizes the display offers (a windowed window may be any of them; Borderless uses the desktop)
            int index = -1;
            for (size_t i = 0; i < resolutions.size(); ++i)
            {
                if (resolutions[i].first == static_cast<std::uint32_t>(v.width) && resolutions[i].second == static_cast<std::uint32_t>(v.height))
                {
                    index = static_cast<int>(i);
                }
            }
            // Borderless takes the resolution of the desktop: show the window as it is
            int shownWidth = v.width;
            int shownHeight = v.height;
            if (v.windowMode == 1 && svc.windowSize)
            {
                const auto size = svc.windowSize();
                shownWidth = static_cast<int>(size.first);
                shownHeight = static_cast<int>(size.second);
            }
            std::string resolutionText = Format("%d \xC3\x97 %d", shownWidth, shownHeight);
            std::string ratio;
            {
                const int w = shownWidth, h = shownHeight;
                const double aspect = static_cast<double>(w) / static_cast<double>(std::max(h, 1));
                ratio = std::fabs(aspect - 16.0 / 9.0) < 0.02 ? "16:9" : (std::fabs(aspect - 16.0 / 10.0) < 0.02 ? "16:10" : (std::fabs(aspect - 21.0 / 9.0) < 0.05 ? "21:9" : (std::fabs(aspect - 4.0 / 3.0) < 0.02 ? "4:3" : "")));
            }
            const int step = StepperRow(f, HashId("video/resolution"), rows.Next(), "Resolution", resolutionText, ratio, v.windowMode != 1 && !locked && !resolutions.empty());
            const std::string_view ratesText[] = {"Unlimited", "60", "120", "144", "240"};
            int rateIndex = 0;
            for (int i = 0; i < 5; ++i)
            {
                rateIndex = kFpsLimits[i] == v.fpsLimit ? i : rateIndex;
            }
            const bool vsync = ToggleRow(f, HashId("video/vsync"), rows.Next(), "VSync", v.vsync, !locked, "Off", "On");
            const int rate = OptionRow(f, HashId("video/fps"), rows.Next(), "Frame rate limit", Span<std::string_view>(ratesText), rateIndex, !locked);
            const std::string_view shadows[] = {"Off", "Low", "High"};
            const int shadow = OptionRow(f, HashId("video/shadows"), rows.Next(), "Shadow quality", Span<std::string_view>(shadows), v.shadows, !locked);
            const std::string_view msaa[] = {"Off", "4x"};
            const int msaaIndex = OptionRow(f, HashId("video/msaa"), rows.Next(), "Anti-aliasing", Span<std::string_view>(msaa), v.msaa >= 4 ? 1 : 0, !locked);
            const bool bloom = ToggleRow(f, HashId("video/bloom"), rows.Next(), "Bloom", v.bloom, !locked);
            const float scale = SliderRow(f, HashId("video/uiscale"), rows.Next(), "UI scale", static_cast<float>(v.uiScalePercent), 75.0f, 125.0f, 5.0f, Format("%d%%", v.uiScalePercent), !locked);
            const bool showFps = ToggleRow(f, HashId("video/fps-overlay"), rows.Next(), "Show FPS", v.showFps, !locked);
            Panel(f, {1258.0f, 290.0f, 600.0f, 220.0f}, false, true);
            Text(f, FontStyle::BlackItalic, 26.0f, {1320.0f, 322.0f}, style::kText, "APPLY");
            Text(f, FontStyle::SemiBold, 20.0f, {1320.0f, 366.0f}, style::kTextDim, "Window mode and resolution ask to keep the change: it");
            Text(f, FontStyle::SemiBold, 20.0f, {1320.0f, 392.0f}, style::kTextDim, "reverts in 15 s if you do not confirm.");
            if (ActionButton(f, HashId("video/reset"), {1275.0f, 880.0f, 580.0f, 72.0f}, "RESET TO DEFAULTS", Icon::Refresh, !locked))
            {
                askResetTab = true;
            }
            Hints(f, {{Glyph::UpDown, Glyph::PadA, "Select"}, {Glyph::LeftRight, Glyph::None, "Change"}, {Glyph::Esc, Glyph::PadB, "Back"}});

            if (!locked)
            {
                bool windowChange = false;
                RlVideoSettings next = v;
                if (mode != v.windowMode)
                {
                    next.windowMode = mode;
                    windowChange = true;
                }
                if (step != 0 && !resolutions.empty())
                {
                    const int from = index < 0 ? static_cast<int>(resolutions.size()) - 1 : index;
                    const int to = std::clamp(from + step, 0, static_cast<int>(resolutions.size()) - 1);
                    next.width = static_cast<int>(resolutions[static_cast<size_t>(to)].first);
                    next.height = static_cast<int>(resolutions[static_cast<size_t>(to)].second);
                    windowChange = windowChange || to != index;
                }
                next.vsync = vsync;
                next.fpsLimit = kFpsLimits[std::clamp(rate, 0, 4)];
                next.shadows = shadow;
                next.msaa = msaaIndex == 1 ? 4 : 1;
                next.bloom = bloom;
                next.showFps = showFps;
                const int newScale = static_cast<int>(std::lround(scale));
                if (newScale != v.uiScalePercent)
                {
                    next.uiScalePercent = newScale;
                    host.SetUiScale(static_cast<float>(newScale) / 100.0f);
                }
                const bool changed = next.windowMode != before.windowMode || next.width != before.width || next.height != before.height || next.vsync != before.vsync ||
                                     next.fpsLimit != before.fpsLimit || next.shadows != before.shadows || next.msaa != before.msaa || next.bloom != before.bloom ||
                                     next.showFps != before.showFps || next.uiScalePercent != before.uiScalePercent;
                if (changed)
                {
                    pendingBefore = before;
                    v = next;
                    ApplyVideo(windowChange && (next.windowMode != before.windowMode || next.width != before.width || next.height != before.height));
                }
            }

            if (videoPending)
            {
                videoConfirm -= f.DeltaTime();
                const DialogResult result = ConfirmDialog(f, HashId("video/keep"), {"KEEP THESE SETTINGS?", Format("Reverting in %d s", std::max(static_cast<int>(std::ceil(videoConfirm)), 0)), "Keep", "Revert"});
                if (result == DialogResult::Confirm)
                {
                    videoPending = false;
                }
                else if (result == DialogResult::Cancel || videoConfirm <= 0.0f)
                {
                    videoPending = false;
                    v = videoBackup;
                    host.SetUiScale(static_cast<float>(v.uiScalePercent) / 100.0f);
                    if (svc.applyVideo)
                    {
                        svc.applyVideo(v);
                    }
                }
            }
            else if (askResetTab)
            {
                const DialogResult result = ConfirmDialog(f, HashId("video/reset-dialog"), {"RESET THE VIDEO SETTINGS?", "The window and the quality return to the defaults.", "Reset", "Cancel"});
                if (result == DialogResult::Confirm)
                {
                    pendingBefore = v;
                    v = RlVideoSettings{};
                    host.SetUiScale(1.0f);
                    ApplyVideo(true);
                    askResetTab = false;
                }
                else if (result == DialogResult::Cancel)
                {
                    askResetTab = false;
                }
            }
        }

        void DrawAudio(Frame& f)
        {
            sys::RlGameSystem& system = System();
            RlAudioSettings& a = settings.audio;
            f.SetDefaultFocus(HashId("audio/master"));
            Column rows({80.0f, 290.0f, 1000.0f, style::kRowHeight}, 62.0f);
            const auto slider = [&](const char* id, const char* label, float& value)
            {
                const float next = SliderRow(f, HashId(id), rows.Next(), label, value * 100.0f, 0.0f, 100.0f, 5.0f, Format("%d%%", static_cast<int>(std::lround(value * 100.0f)))) / 100.0f;
                if (std::fabs(next - value) > 1e-4f)
                {
                    value = next;
                    system.SetVolumes(a.master, a.effects, a.music);
                }
            };
            slider("audio/master", "Master", a.master);
            slider("audio/effects", "Effects", a.effects);
            slider("audio/music", "Music", a.music);
            if (ActionButton(f, HashId("audio/reset"), {1275.0f, 880.0f, 580.0f, 72.0f}, "RESET TO DEFAULTS", Icon::Refresh))
            {
                a = RlAudioSettings{};
                system.SetVolumes(a.master, a.effects, a.music);
            }
            Hints(f, {{Glyph::UpDown, Glyph::PadA, "Select"}, {Glyph::LeftRight, Glyph::None, "Change"}, {Glyph::Esc, Glyph::PadB, "Back"}});
        }

        void DrawSettings(Frame& f)
        {
            const bool cameraTab = tab == 1;
            // the camera is seen on the live field behind the screen (a pause already has the match behind it)
            System().SetCameraPreview(cameraTab && settingsReturn == Screen::Main);
            if (cameraTab && settingsReturn == Screen::Main)
            {
                f.SetSpace(Space::Screen);
                FillRectGradient(f, {0.0f, 0.0f, 1180.0f, f.metrics().designWindow.y}, Color::Hex(0x050b1c, 0.94f), Color::Hex(0x050b1c, 0.0f));
                f.SetSpace(Space::Menu);
            }
            else
            {
                Backdrop(f, 0.94f);
            }
            static const char* const kTitles[] = {"CONTROLS", "CAMERA", "VIDEO", "AUDIO"};
            ScreenTitle(f, "SETTINGS", kTitles[std::clamp(tab, 0, 3)]);
            const std::string_view labels[] = {"CONTROLS", "CAMERA", "VIDEO", "AUDIO"};
            const bool dialogOpen = rebind.active || askResetControls || askResetTab || videoPending;
            if (!dialogOpen)
            {
                const int next = Tabs(f, HashId("settings/tabs"), {75.0f, 200.0f, 700.0f, 54.0f}, Span<std::string_view>(labels), tab);
                if (next != tab)
                {
                    tab = next;
                    f.SetFocus(kNoId);
                }
            }
            else
            {
                Tabs(f, HashId("settings/tabs"), {75.0f, 200.0f, 700.0f, 54.0f}, Span<std::string_view>(labels), tab);
            }
            switch (tab)
            {
            case 0: DrawControls(f); break;
            case 1: DrawCamera(f); break;
            case 2: DrawVideo(f); break;
            default: DrawAudio(f); break;
            }
            if (!dialogOpen && f.input().Pressed(Nav::Back) && !rebind.active)
            {
                LeaveSettings(f);
            }
        }

        void LeaveSettings(Frame& f)
        {
            System().SetCameraPreview(false);
            Save();
            System().SaveControls();
            GoTo(f, settingsReturn);
        }
    };

    // ---- the public side

    RlGameFlow::RlGameFlow(const RlGameFlowServices& services) : impl_(std::make_unique<Impl>(services)) {}

    RlGameFlow::~RlGameFlow()
    {
        if (impl_ != nullptr && impl_->installed)
        {
            impl_->Save();
            impl_->host.Uninstall();
        }
    }

    void RlGameFlow::Install()
    {
        Impl& d = *impl_;
        d.LoadSettings();
        d.settings.Clamp();
        if (d.svc.setLighting)
        {
            d.menuLighting = true;
            d.svc.setLighting(true);
        }
        HostConfig config;
        config.uiScale = static_cast<float>(d.settings.video.uiScalePercent) / 100.0f;
        d.host.Install(*d.svc.ui, config);
        d.host.SetScreen([impl = impl_.get()](Frame& frame) { impl->Draw(frame); });
        d.ApplyToGame();
        d.lobby.Install(GetCommandLineA());
        if (d.svc.applyVideo)
        {
            d.svc.applyVideo(d.settings.video);
        }
        d.installed = true;
    }

    void RlGameFlow::SetRawInputHook(std::function<void(RawInput&)> hook)
    {
        impl_->host.SetRawInputHook(std::move(hook));
    }

    void RlGameFlow::SaveSettings()
    {
        impl_->Save();
    }

    const RlGameSettings& RlGameFlow::Settings() const
    {
        return impl_->settings;
    }

    const char* RlGameFlow::ScreenName() const
    {
        return kScreenNames[static_cast<int>(impl_->screen)];
    }
}
