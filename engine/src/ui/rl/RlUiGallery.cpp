// The gallery of the UI kit: every screen of the mock-ups built from the widgets, with fake data. It is also the example of how a screen
// is made from the kit: a function that gets a Frame& and calls the widgets.

#include <myengine/ui/rl/RlUiGallery.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <myengine/ui/rl/RlUiHud.h>
#include <myengine/ui/rl/RlUiPaint.h>
#include <myengine/ui/rl/RlUiWidgets.h>

namespace myengine::rlui
{
    using namespace paint;

    namespace
    {
        const char* const kScreenNames[] = {"main", "play", "private", "loading", "pause", "results", "controls", "rebind", "camera", "video", "hud-kickoff", "hud-goal", "hud-scoreboard", "hud-replay"};

        // ---- the fake data of the mock-ups
        std::vector<HudPlayer> FakePlayers()
        {
            std::vector<HudPlayer> players(4);
            players[0] = {"Artem", 0, false, 420, 2, 0, 1, 3, 0, 0};
            players[1] = {"Bot Comet", 0, true, 260, 0, 1, 2, 1, 1, -1};
            players[2] = {"Bot Blaze", 1, true, 380, 1, 1, 0, 2, 2, -1};
            players[3] = {"Bot Nova", 1, true, 300, 1, 0, 1, 2, 0, -1};
            return players;
        }

        struct GalleryData
        {
            // the controls
            int tab = 0;
            // interactive values (the gallery is alive: the focus, the tabs, the sliders work on this)
            int difficulty = 1;
            int mode = 1;
            int playTab = 0;
            bool askQuit = false;
            int teamSize = 1;
            bool fillBots = true;
            std::string address = "192.168.1.42:27015";
            float fov = 110.0f;
            float distance = 270.0f;
            float height = 100.0f;
            float angle = -3.0f;
            float stiffness = 0.45f;
            float swivel = 4.7f;
            float transition = 1.2f;
            int ballCam = 0;
            bool shake = false;
            int windowMode = 1;
            int resolution = 0;
            bool vsync = true;
            int frameRate = 2;
            int shadows = 2;
            float uiScale = 1.0f;
            bool invertPitch = false;
            float deadzone = 0.2f;
            // the HUD
            Hud hud;
            GalleryScreen hudScreen = GalleryScreen::Count;
            int hudStep = 0;
            float hudTime = 0.0f;
            float rebindClock = 0.0f;
        };
        GalleryData g;

        // ---- the decorations that stand for the 3D scene of the game
        void FakeCar(Frame& f, const Vec2 origin, const float scale, const bool lowRoof = false)
        {
            const auto p = [&](const float x, const float y) { return Vec2{origin.x + x * scale, origin.y + y * scale}; };
            const Color body = style::kBlue;
            const Color dark = style::kBlueDark;
            const Color light = style::kBlueHi;
            // the shadow
            for (int i = 3; i >= 1; --i)
            {
                const float k = static_cast<float>(i) / 3.0f;
                const Vec2 c = p(330.0f, 252.0f);
                const float rx = (300.0f + 20.0f * k) * scale;
                const float ry = (22.0f + 8.0f * k) * scale;
                Vec2 points[24];
                for (int a = 0; a < 24; ++a)
                {
                    const float t = static_cast<float>(a) / 24.0f * 6.2831853f;
                    points[a] = {c.x + std::cos(t) * rx, c.y + std::sin(t) * ry};
                }
                Polygon(f, Span<Vec2>(points), Color::Hex(0x000000, 0.22f));
            }
            // the spoiler, the body, the cabin
            const Vec2 spoiler[] = {p(10, 118), p(78, 106), p(78, 126), p(4, 132)};
            Polygon(f, Span<Vec2>(spoiler), dark);
            const float roof = lowRoof ? 24.0f : 10.0f;
            const Vec2 cabin[] = {p(240, 104), p(335, roof), p(520, roof + 6), p(612, 104)};
            Polygon(f, Span<Vec2>(cabin), body);
            const Vec2 window[] = {p(285, 98), p(342, roof + 24), p(455, roof + 28), p(505, 98)};
            Polygon(f, Span<Vec2>(window), Color::Hex(0x4a6a9a));
            const Vec2 hull[] = {p(24, 168), p(80, 118), p(250, 100), p(610, 102), p(646, 128), p(652, 178), p(560, 204), p(100, 204)};
            Polygon(f, Span<Vec2>(hull), body);
            const Vec2 sill[] = {p(70, 178), p(646, 176), p(560, 208), p(100, 208)};
            Polygon(f, Span<Vec2>(sill), dark);
            const Vec2 stripe[] = {p(92, 150), p(625, 146), p(622, 160), p(97, 166)};
            Polygon(f, Span<Vec2>(stripe), light);
            FillRect(f, {p(6, 138).x, p(6, 138).y, 24.0f * scale, 14.0f * scale}, Color::Hex(0xff4d5e));
            FillRect(f, {p(628, 100).x, p(628, 100).y, 38.0f * scale, 12.0f * scale}, Color::Hex(0xfff0b8));
            // the wheels
            for (const float cx : {160.0f, 572.0f})
            {
                Circle(f, p(cx, 208), 62.0f * scale, Color::Hex(0x0b0b12));
                Circle(f, p(cx, 208), 28.0f * scale, Color::Hex(0x5b6580));
            }
        }

        void FakeField(Frame& f)
        {
            const Vec2 window = f.metrics().designWindow;
            const float top = window.y - 125.0f;
            const Vec2 pitch[] = {{window.x * 0.06f, top}, {window.x * 0.94f, top}, {window.x, window.y}, {0.0f, window.y}};
            Polygon(f, Span<Vec2>(pitch), Color::Hex(0x1f6b3a));
            // the stripes of the mowing
            for (int i = 0; i < 12; i += 2)
            {
                const float x0 = window.x * (static_cast<float>(i) / 12.0f);
                const float x1 = window.x * (static_cast<float>(i + 1) / 12.0f);
                const float t0 = window.x * 0.06f + (x0 - window.x * 0.06f) * 0.0f;
                (void)t0;
                const Vec2 stripe[] = {{window.x * 0.06f + (window.x * 0.88f) * (static_cast<float>(i) / 12.0f), top},
                                       {window.x * 0.06f + (window.x * 0.88f) * (static_cast<float>(i + 1) / 12.0f), top},
                                       {x1, window.y}, {x0, window.y}};
                Polygon(f, Span<Vec2>(stripe), Color::Hex(0x000000, 0.10f));
            }
            Line(f, {window.x * 0.5f, top}, {window.x * 0.5f, window.y}, Color::Hex(0xffffff, 0.45f), 2.0f);
            Line(f, {window.x * 0.17f, top + 14.0f}, {window.x * 0.01f, window.y}, Color::Hex(0xffffff, 0.35f), 2.0f);
            Line(f, {window.x * 0.83f, top + 14.0f}, {window.x * 0.99f, window.y}, Color::Hex(0xffffff, 0.35f), 2.0f);
        }

        // The game seen through the HUD: dark stadium and the pitch (the 3D scene of the game fills this place)
        void FakeGameView(Frame& f, const bool car, const Vec2 carOrigin, const float carScale)
        {
            f.SetSpace(Space::Screen);
            MenuBackdrop(f);
            FakeField(f);
            f.SetSpace(Space::Menu);
            if (car)
            {
                FakeCar(f, carOrigin, carScale);
            }
        }

        // The hints
        void Hints(Frame& f, const Glyph k0, const Glyph p0, const char* t0, const Glyph k1, const Glyph p1, const char* t1, const Glyph k2, const Glyph p2, const char* t2,
                   const Glyph k3 = Glyph::None, const Glyph p3 = Glyph::None, const char* t3 = "")
        {
            Hint hints[4] = {{k0, p0, t0}, {k1, p1, t1}, {k2, p2, t2}, {k3, p3, t3}};
            HintBar(f, Span<Hint>(hints, k3 == Glyph::None && p3 == Glyph::None ? 3u : 4u));
        }

        // ---------------------------------------------------------------- the screens

        void MainMenu(Frame& f)
        {
            MenuBackdrop(f);
            FakeCar(f, {870.0f, 600.0f}, 1.0f);
            ScreenTitle(f, "MYENGINE", "ROCKET\nARENA", {80.0f, 76.0f}, true);
            const float versionWidth = Chip(f, {1850.0f, 60.0f}, "V0.1 \xC2\xB7 G1");
            Chip(f, {1850.0f - versionWidth - 12.0f, 60.0f}, "OFFLINE", Icon::WifiOff);

            const Id play = HashId("main/play");
            const Id training = HashId("main/training");
            const Id settings = HashId("main/settings");
            const Id exit = HashId("main/exit");
            f.SetDefaultFocus(play);
            Column column({76.0f, 370.0f, style::kMenuButtonWidth, style::kMenuButtonHeight}, style::kMenuButtonStep);
            MenuButton(f, play, column.Next(), "PLAY", Icon::Play);
            MenuButton(f, training, column.Next(), "TRAINING", Icon::Target);
            MenuButton(f, settings, column.Next(), "SETTINGS", Icon::Settings);
            if (MenuButton(f, exit, column.Next(), "EXIT", Icon::Exit))
            {
                g.askQuit = true;
            }
            if (f.input().Pressed(Nav::Back) && !g.askQuit)
            {
                f.SetFocus(exit);
            }
            Hints(f, Glyph::UpDown, Glyph::PadA, "Select", Glyph::Enter, Glyph::None, "Confirm", Glyph::Esc, Glyph::PadB, "Back");
            if (g.askQuit)
            {
                const DialogResult result = ConfirmDialog(f, HashId("main/quit"), {"QUIT TO DESKTOP?", "Your settings are saved.", "Quit", "Cancel"});
                if (result != DialogResult::None)
                {
                    g.askQuit = false;
                }
            }
        }

        void Tabs2(Frame& f, const char* a, const char* b, int& tab)
        {
            const std::string_view labels[] = {a, b};
            tab = Tabs(f, HashId("tabs"), {75.0f, 200.0f, 640.0f, 54.0f}, Span<std::string_view>(labels), tab);
        }

        void PlayScreen(Frame& f, const bool privateTab)
        {
            MenuBackdrop(f);
            ScreenTitle(f, "PLAY", privateTab ? "PRIVATE MATCH" : "CHOOSE A MODE");
            int tab = privateTab ? 1 : g.playTab;
            Tabs2(f, "OFFLINE \xC2\xB7 BOTS", "PRIVATE MATCH", tab);
            g.playTab = tab;
            if (!privateTab && tab == 1)
            {
                // the tab was switched in the gallery: show the other page next frame
                f.SetFocus(kNoId);
            }
            if (!privateTab)
            {
                const ModeCardDesc cards[4] = {{"1v1", "DUEL", "You + 1 bot", 1}, {"2v2", "DOUBLES", "You + 3 bots", 2}, {"3v3", "STANDARD", "You + 5 bots", 3}, {"4v4", "CHAOS", "You + 7 bots", 4}};
                f.SetDefaultFocus(HashId("play/card1"));
                for (int i = 0; i < 4; ++i)
                {
                    const std::string id = "play/card" + std::to_string(i);
                    if (ModeCard(f, HashId(id), {62.0f + 284.0f * static_cast<float>(i), 300.0f, 320.0f, 330.0f}, cards[i], g.mode == i))
                    {
                        g.mode = i;
                    }
                }
                const std::string_view levels[] = {"Rookie", "Pro", "All-star"};
                g.difficulty = OptionRow(f, HashId("play/difficulty"), {80.0f, 700.0f, 1100.0f, 64.0f}, "Bot difficulty", Span<std::string_view>(levels), g.difficulty);
                ActionButton(f, HashId("play/start"), {1370.0f, 877.0f, 440.0f, 76.0f}, "START MATCH", Icon::Play);
                Hints(f, Glyph::LeftRight, Glyph::PadA, "Select", Glyph::QE, Glyph::PadLBRB, "Tabs", Glyph::Esc, Glyph::PadB, "Back");
            }
            else
            {
                Panel(f, {45.0f, 300.0f, 810.0f, 520.0f}, true);
                IconGlyph(f, Icon::Server, 40.0f, {160.0f, 366.0f}, style::kText);
                Text(f, FontStyle::BlackItalic, 52.0f, {196.0f, 332.0f}, style::kText, "HOST");
                Text(f, FontStyle::SemiBold, 24.0f, {142.0f, 398.0f}, style::kTextDim, "Friends join you. You play on your machine.");
                const std::string_view sizes[] = {"1v1", "2v2", "3v3", "4v4"};
                f.SetDefaultFocus(HashId("host/size"));
                g.teamSize = OptionRow(f, HashId("host/size"), {142.0f, 452.0f, 618.0f, 58.0f}, "Team size", Span<std::string_view>(sizes), g.teamSize);
                g.fillBots = ToggleRow(f, HashId("host/fill"), {142.0f, 516.0f, 618.0f, 58.0f}, "Fill with bots", g.fillBots, true, "Off", "On");
                ValueRow(f, HashId("host/port"), {142.0f, 580.0f, 618.0f, 58.0f}, "Port", "27015");
                ActionButton(f, HashId("host/go"), {142.0f, 668.0f, 440.0f, 76.0f}, "HOST MATCH", Icon::Play);

                Panel(f, {782.0f, 300.0f, 810.0f, 520.0f}, false);
                IconGlyph(f, Icon::Join, 40.0f, {904.0f, 366.0f}, style::kText);
                Text(f, FontStyle::BlackItalic, 52.0f, {940.0f, 332.0f}, style::kText, "JOIN BY IP");
                Text(f, FontStyle::SemiBold, 24.0f, {886.0f, 398.0f}, style::kTextDim, "Ask the host for the address.");
                TextRow(f, HashId("join/address"), {886.0f, 452.0f, 618.0f, 58.0f}, "Address", g.address, 21, "0123456789.:");
                ValueRow(f, HashId("join/recent"), {886.0f, 516.0f, 618.0f, 58.0f}, "Recent", "192.168.1.42 \xC2\xB7 yesterday", true, false);
                ActionButton(f, HashId("join/go"), {886.0f, 600.0f, 360.0f, 66.0f}, "JOIN", Icon::Join);
                IconGlyph(f, Icon::Info, 18.0f, {896.0f, 706.0f}, style::kTextDim);
                Text(f, FontStyle::SemiBold, 20.0f, {910.0f, 694.0f}, style::kTextDim, "Steam invites come later (G6).");
                Hints(f, Glyph::AllArrows, Glyph::PadA, "Select", Glyph::QE, Glyph::PadLBRB, "Tabs", Glyph::Esc, Glyph::PadB, "Back");
            }
        }

        void LoadingScreen(Frame& f)
        {
            MenuBackdrop(f);
            Text(f, FontStyle::SemiBold, 24.0f, {80.0f, 703.0f}, style::kTextDim, "OFFLINE \xC2\xB7 2V2 \xC2\xB7 PRO BOTS");
            Text(f, FontStyle::BlackItalic, 84.0f, {80.0f, 730.0f}, style::kText, "STANDARD ARENA");
            Text(f, FontStyle::SemiBold, 24.0f, {80.0f, 846.0f}, style::kTextDim, "Tip: hold");
            const float x = 80.0f + TextWidth(f, FontStyle::SemiBold, 24.0f, "Tip: hold") + 10.0f;
            const float w = KeyCap(f, {x, 840.0f}, "Space");
            Text(f, FontStyle::SemiBold, 24.0f, {x + w + 12.0f, 846.0f}, style::kTextDim, "to look at the ball, release for car cam.");
            ProgressBar(f, {80.0f, 960.0f, 1760.0f, 10.0f}, 0.64f);
            Text(f, FontStyle::SemiBold, 22.0f, {1840.0f, 996.0f}, style::kBlueHi, "Building the arena \xC2\xB7 64%", Align::Right);
        }

        HudState GameState(const HudPhase phase, const float clock, const int blue, const int orange, const float boost)
        {
            HudState s;
            s.phase = phase;
            s.clock = clock;
            s.score[0] = blue;
            s.score[1] = orange;
            s.boost = boost;
            s.players = FakePlayers();
            s.mode = 2;
            s.localPlayer = 0;
            return s;
        }

        void PauseScreen(Frame& f)
        {
            FakeGameView(f, false, {}, 1.0f);
            f.SetSpace(Space::Screen);
            Dim(f, Color::Hex(0x030814, 0.35f));
            f.SetSpace(Space::Menu);
            g.hud.Update(f.DeltaTime(), GameState(HudPhase::Playing, 67.0f, 2, 2, 74.0f));
            g.hud.Draw(f, false, true);
            Text(f, FontStyle::BlackItalic, 64.0f, {80.0f, 236.0f}, style::kText, "PAUSED");
            f.SetDefaultFocus(HashId("pause/resume"));
            Column column({76.0f, 350.0f, style::kMenuButtonWidth, style::kMenuButtonHeight}, style::kMenuButtonStep);
            MenuButton(f, HashId("pause/resume"), column.Next(), "RESUME", Icon::Play);
            MenuButton(f, HashId("pause/settings"), column.Next(), "SETTINGS", Icon::Settings);
            MenuButton(f, HashId("pause/restart"), column.Next(), "RESTART MATCH", Icon::Refresh);
            MenuButton(f, HashId("pause/leave"), column.Next(), "LEAVE MATCH", Icon::Exit);
            Panel(f, {1258.0f, 280.0f, 565.0f, 220.0f}, false, true);
            Text(f, FontStyle::SemiBold, 22.0f, {1314.0f, 312.0f}, style::kTextDim, "OFFLINE \xC2\xB7 2V2 \xC2\xB7 PRO BOTS");
            Text(f, FontStyle::BlackItalic, 52.0f, {1314.0f, 348.0f}, style::kBlueHi, "2");
            Text(f, FontStyle::BlackItalic, 52.0f, {1352.0f, 348.0f}, style::kText, "\xE2\x80\x94");
            Text(f, FontStyle::BlackItalic, 52.0f, {1402.0f, 348.0f}, style::kOrangeHi, "2");
            Text(f, FontStyle::BlackItalic, 34.0f, {1440.0f, 364.0f}, style::kTextDim, "\xC2\xB7 1:07");
            Text(f, FontStyle::SemiBold, 18.0f, {1314.0f, 420.0f}, style::kTextDim, "Offline: the match is frozen. Online: the match goes on.");
            Hints(f, Glyph::UpDown, Glyph::PadA, "Select", Glyph::Esc, Glyph::PadB, "Resume", Glyph::None, Glyph::None, "");
        }

        void ResultsScreen(Frame& f)
        {
            MenuBackdrop(f);
            const Rect banner{520.0f, 60.0f, 880.0f, 120.0f};
            ParallelogramGradient3(f, banner, style::kBlue.Faded(0.1f), style::kBlue, style::kBlue.Faded(0.1f));
            Text(f, FontStyle::BlackItalic, 96.0f, {960.0f, 58.0f}, style::kText, "BLUE WINS", Align::Center);
            Text(f, FontStyle::BlackItalic, 64.0f, {838.0f, 208.0f}, style::kBlueHi, "3");
            FillRect(f, {896.0f, 244.0f, 52.0f, 6.0f}, style::kTextDim);
            Text(f, FontStyle::BlackItalic, 64.0f, {966.0f, 208.0f}, style::kOrangeHi, "2");
            FillRect(f, {1032.0f, 238.0f, 48.0f, 32.0f}, style::kMvp);
            TextIn(f, FontStyle::Bold, 16.0f, {1032.0f, 238.0f, 48.0f, 32.0f}, Color::Hex(0x08142c), "OT", Align::Center);
            FakeCar(f, {640.0f, 280.0f}, 0.9f);
            const Rect mvp{1255.0f, 400.0f, 258.0f, 48.0f};
            Parallelogram(f, mvp, style::kMvp);
            IconGlyph(f, Icon::Star, 24.0f, {mvp.x + 44.0f, mvp.y + 24.0f}, Color::Hex(0x08142c));
            TextIn(f, FontStyle::ExtraBoldItalic, 26.0f, {mvp.x + 66.0f, mvp.y, 200.0f, mvp.h}, Color::Hex(0x08142c), "MVP \xC2\xB7 ARTEM");
            HudState s = GameState(HudPhase::Finished, 0.0f, 3, 2, 0.0f);
            s.players[0].score = 620; s.players[0].goals = 2; s.players[0].assists = 1; s.players[0].saves = 1; s.players[0].shots = 4; s.players[0].demos = 0;
            s.players[1].score = 300; s.players[1].goals = 1; s.players[1].assists = 0; s.players[1].saves = 2; s.players[1].shots = 2; s.players[1].demos = 1;
            Hud::DrawScoreTable(f, {360.0f, 690.0f, 1200.0f, 0.0f}, s, false, false, false);
            f.SetDefaultFocus(HashId("results/again"));
            ActionButton(f, HashId("results/again"), {1208.0f, 982.0f, 314.0f, 66.0f}, "PLAY AGAIN", Icon::Refresh);
            ActionButton(f, HashId("results/menu"), {1532.0f, 982.0f, 310.0f, 66.0f}, "MAIN MENU", Icon::Home);
        }

        struct ControlRow
        {
            const char* action;
            const char* keyboard;
            const char* pad;
        };
        const ControlRow kControlRows[] = {{"Throttle", "W", "RT"}, {"Reverse / brake", "S", "LT"}, {"Steer left / right", "A \xC2\xB7 D", "Left stick"}, {"Jump", "Right mouse", "A"},
                                           {"Boost", "Left mouse", "B"}, {"Powerslide", "Shift", "X"}, {"Air roll left / right", "Q \xC2\xB7 E", "LB \xC2\xB7 RB"},
                                           {"Ball cam", "Space", "Y"}, {"Scoreboard", "Tab", "Back"}, {"Pause", "Esc", "Start"}};

        void SettingsFrame(Frame& f, const char* title, const int tab)
        {
            MenuBackdrop(f);
            ScreenTitle(f, "SETTINGS", title);
            const std::string_view labels[] = {"CONTROLS", "CAMERA", "VIDEO", "AUDIO"};
            Tabs(f, HashId("settings/tabs"), {75.0f, 200.0f, 700.0f, 54.0f}, Span<std::string_view>(labels), tab);
        }

        void ControlsScreen(Frame& f, const bool rebind)
        {
            SettingsFrame(f, "CONTROLS", 0);
            ColumnCaption(f, {106.0f, 292.0f}, "ACTION");
            ColumnCaption(f, {526.0f, 292.0f}, "KEYBOARD / MOUSE");
            ColumnCaption(f, {756.0f, 292.0f}, "GAMEPAD");
            f.SetDefaultFocus(HashId("controls/Jump"));
            Column rows({80.0f, 322.0f, 1100.0f, style::kRowHeight}, 62.0f);
            for (const ControlRow& row : kControlRows)
            {
                BindRow(f, HashId(std::string("controls/") + row.action), rows.Next(), row.action, row.keyboard, row.pad);
            }
            Panel(f, {1245.0f, 290.0f, 640.0f, 360.0f}, false, true);
            Text(f, FontStyle::ExtraBoldItalic, 26.0f, {1315.0f, 322.0f}, style::kText, "JUMP");
            Text(f, FontStyle::SemiBold, 20.0f, {1315.0f, 360.0f}, style::kTextDim, "Tap to jump, tap again in the air to dodge.");
            ValueRow(f, HashId("controls/air"), {1315.0f, 400.0f, 500.0f, 58.0f}, "In the air", "Throttle = pitch", true, false);
            g.invertPitch = ToggleRow(f, HashId("controls/invert"), {1315.0f, 462.0f, 500.0f, 58.0f}, "Invert pitch", g.invertPitch);
            ValueRow(f, HashId("controls/deadzone"), {1315.0f, 524.0f, 500.0f, 58.0f}, "Pad deadzone", "0.20");
            ActionButton(f, HashId("controls/reset"), {1275.0f, 880.0f, 580.0f, 72.0f}, "RESET TO DEFAULTS", Icon::Refresh);
            Hints(f, Glyph::Enter, Glyph::PadA, "Rebind", Glyph::Del, Glyph::PadX, "Clear", Glyph::R, Glyph::PadY, "Reset row", Glyph::Esc, Glyph::PadB, "Back");
            if (rebind)
            {
                g.rebindClock += f.DeltaTime();
                const float remaining = 1.0f - std::fmod(g.rebindClock, 5.0f) / 5.0f;
                RebindDialog(f, {"REBIND \xC2\xB7 KEYBOARD / MOUSE", "JUMP", "Press a key or a mouse button...", "Esc \xE2\x80\x94 cancel \xC2\xB7 Del \xE2\x80\x94 clear \xC2\xB7 the key is already used by ", "Boost", remaining * 0.6f + 0.4f});
            }
        }

        void CameraScreen(Frame& f)
        {
            SettingsFrame(f, "CAMERA", 1);
            f.SetDefaultFocus(HashId("camera/fov"));
            Column rows({80.0f, 290.0f, 1000.0f, style::kRowHeight}, 62.0f);
            const auto text = [](const char* format, const float value)
            {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), format, static_cast<double>(value));
                return std::string(buffer);
            };
            g.fov = SliderRow(f, HashId("camera/fov"), rows.Next(), "Field of view", g.fov, 60.0f, 110.0f, 1.0f, text("%.0f", g.fov));
            g.distance = SliderRow(f, HashId("camera/distance"), rows.Next(), "Distance", g.distance, 100.0f, 400.0f, 10.0f, text("%.0f", g.distance));
            g.height = SliderRow(f, HashId("camera/height"), rows.Next(), "Height", g.height, 40.0f, 200.0f, 10.0f, text("%.0f", g.height));
            g.angle = SliderRow(f, HashId("camera/angle"), rows.Next(), "Angle", g.angle, -15.0f, 0.0f, 0.5f, text("%.1f", g.angle));
            g.stiffness = SliderRow(f, HashId("camera/stiffness"), rows.Next(), "Stiffness", g.stiffness, 0.0f, 1.0f, 0.05f, text("%.2f", g.stiffness));
            g.swivel = SliderRow(f, HashId("camera/swivel"), rows.Next(), "Swivel speed", g.swivel, 1.0f, 10.0f, 0.1f, text("%.1f", g.swivel));
            g.transition = SliderRow(f, HashId("camera/transition"), rows.Next(), "Transition speed", g.transition, 1.0f, 2.0f, 0.1f, text("%.1f", g.transition));
            const std::string_view ball[] = {"Toggle", "Hold"};
            g.ballCam = OptionRow(f, HashId("camera/ball"), rows.Next(), "Ball cam", Span<std::string_view>(ball), g.ballCam);
            g.shake = ToggleRow(f, HashId("camera/shake"), rows.Next(), "Camera shake", g.shake);
            // the live preview
            const Rect preview{1180.0f, 290.0f, 700.0f, 400.0f};
            Parallelogram(f, preview, Color::Hex(0x0b1d46));
            ParallelogramOutline(f, preview, style::kLine, 1.0f);
            PushClip(f, preview);
            const Vec2 pitch[] = {{1195.0f, 658.0f}, {1805.0f, 658.0f}, {1815.0f, 690.0f}, {1180.0f, 690.0f}};
            Polygon(f, Span<Vec2>(pitch), Color::Hex(0x1f8a43));
            FakeCar(f, {1395.0f, 535.0f}, 0.42f);
            Circle(f, {1637.0f, 428.0f}, 27.0f, Color::Hex(0xe8eefc));
            PopClip(f);
            IconGlyph(f, Icon::Eye, 16.0f, {1262.0f, 322.0f}, style::kText);
            Text(f, FontStyle::Bold, 16.0f, {1280.0f, 313.0f}, style::kText, "LIVE PREVIEW", Align::Left, 0.1f);
            ActionButton(f, HashId("camera/reset"), {1275.0f, 880.0f, 580.0f, 72.0f}, "RESET TO DEFAULTS", Icon::Refresh);
            Hints(f, Glyph::UpDown, Glyph::PadA, "Select", Glyph::LeftRight, Glyph::None, "Change", Glyph::Esc, Glyph::PadB, "Back");
        }

        void VideoScreen(Frame& f)
        {
            SettingsFrame(f, "VIDEO", 2);
            f.SetDefaultFocus(HashId("video/mode"));
            Column rows({80.0f, 290.0f, 1100.0f, 64.0f}, 68.0f);
            const std::string_view modes[] = {"Windowed", "Borderless", "Fullscreen"};
            g.windowMode = OptionRow(f, HashId("video/mode"), rows.Next(), "Window mode", Span<std::string_view>(modes), g.windowMode);
            static const char* const resolutions[] = {"2560 \xC3\x97 1440", "1920 \xC3\x97 1080", "1600 \xC3\x97 900", "1280 \xC3\x97 720"};
            const int step = StepperRow(f, HashId("video/resolution"), rows.Next(), "Resolution", resolutions[g.resolution], "16:9");
            g.resolution = std::clamp(g.resolution - step, 0, 3);
            g.vsync = ToggleRow(f, HashId("video/vsync"), rows.Next(), "VSync", g.vsync, true, "Off", "On");
            const std::string_view rates[] = {"60", "120", "144", "240", "Unlimited"};
            g.frameRate = OptionRow(f, HashId("video/fps"), rows.Next(), "Frame rate limit", Span<std::string_view>(rates), g.frameRate);
            const std::string_view shadows[] = {"Off", "Low", "High"};
            g.shadows = OptionRow(f, HashId("video/shadows"), rows.Next(), "Shadow quality", Span<std::string_view>(shadows), g.shadows);
            char scale[16];
            std::snprintf(scale, sizeof(scale), "%d%%", static_cast<int>(std::lround(g.uiScale * 100.0f)));
            g.uiScale = SliderRow(f, HashId("video/uiscale"), rows.Next(), "UI scale", g.uiScale, 0.75f, 1.25f, 0.05f, scale);
            Panel(f, {1258.0f, 290.0f, 600.0f, 220.0f}, false, true);
            Text(f, FontStyle::BlackItalic, 26.0f, {1320.0f, 322.0f}, style::kText, "APPLY");
            Text(f, FontStyle::SemiBold, 20.0f, {1320.0f, 366.0f}, style::kTextDim, "Window mode and resolution ask to keep the change: it");
            Text(f, FontStyle::SemiBold, 20.0f, {1320.0f, 392.0f}, style::kTextDim, "reverts in 15 s if you do not confirm.");
            Hints(f, Glyph::UpDown, Glyph::PadA, "Select", Glyph::LeftRight, Glyph::None, "Change", Glyph::Esc, Glyph::PadB, "Back");
        }

        // The HUD screens: a fake game view and the HUD fed with the states of the mock-ups; every cycle starts over when the screen is entered
        void HudScreen(Frame& f, const GalleryScreen screen)
        {
            if (g.hudScreen != screen)
            {
                g.hudScreen = screen;
                g.hud.Reset();
                g.hudStep = 0;
                g.hudTime = 0.0f;
            }
            g.hudTime += f.DeltaTime();
            const bool goal = screen == GalleryScreen::HudGoal;
            const bool kickoff = screen == GalleryScreen::HudKickoff;
            const bool replay = screen == GalleryScreen::HudReplay;
            FakeGameView(f, kickoff || goal || replay, kickoff ? Vec2{690.0f, 790.0f} : (replay ? Vec2{880.0f, 700.0f} : Vec2{340.0f, 820.0f}), kickoff ? 0.84f : 0.8f);
            f.SetSpace(Space::Screen);

            HudState state;
            if (kickoff)
            {
                state = GameState(HudPhase::Countdown, 272.0f, 1, 2, 33.0f);
                state.phaseTimer = 1.55f;
                state.pingMs = 42;
                state.ballCamKey = "Space";
            }
            else if (goal)
            {
                state = GameState(g.hudStep == 0 ? HudPhase::Playing : HudPhase::GoalScored, 67.0f, g.hudStep == 0 ? 1 : 2, 2, 74.0f);
                state.phaseTimer = 60.0f;
                state.lastGoalTeam = 0;
                state.lastGoalScorer = 0;
                state.lastGoalSpeedKmh = 98.0f;
                state.ballCam = g.hudStep != 0 || true;
            }
            else if (replay)
            {
                state = GameState(HudPhase::GoalScored, 67.0f, 2, 2, 74.0f);
                state.replay.active = true;
                state.replay.author = "Artem";
                state.replay.team = 0;
                state.replay.goalSpeedKmh = 98.0f;
                state.replay.speed = 0.5f;
                state.replay.progress = 0.62f;
            }
            else
            {
                state = GameState(HudPhase::Playing, 67.0f, 2, 2, 74.0f);
                state.pingMs = 0;
            }
            g.hud.Update(f.DeltaTime(), state);
            if (goal && g.hudStep == 0)
            {
                g.hudStep = 1;
                state.phase = HudPhase::GoalScored;
                state.score[0] = 2;
                g.hud.Update(0.0f, state);
                g.hud.PushEvent({HudEventType::Save, 1, -1});
                g.hud.PushEvent({HudEventType::Demo, 2, 1});
                g.hud.PushEvent({HudEventType::Goal, 0, -1});
            }
            g.hud.Draw(f, screen == GalleryScreen::HudScoreboard);
            f.SetSpace(Space::Menu);
        }
    }

    const char* GalleryScreenName(const GalleryScreen screen)
    {
        const int index = static_cast<int>(screen);
        return index >= 0 && index < static_cast<int>(GalleryScreen::Count) ? kScreenNames[index] : "?";
    }

    bool ParseGalleryScreen(const std::string_view name, GalleryScreen& out)
    {
        for (int i = 0; i < static_cast<int>(GalleryScreen::Count); ++i)
        {
            if (name == kScreenNames[i])
            {
                out = static_cast<GalleryScreen>(i);
                return true;
            }
        }
        return false;
    }

    void DrawGalleryScreen(Frame& f, const GalleryScreen screen)
    {
        switch (screen)
        {
        case GalleryScreen::Main: MainMenu(f); break;
        case GalleryScreen::Play: PlayScreen(f, false); break;
        case GalleryScreen::Private: PlayScreen(f, true); break;
        case GalleryScreen::Loading: LoadingScreen(f); break;
        case GalleryScreen::Pause: PauseScreen(f); break;
        case GalleryScreen::Results: ResultsScreen(f); break;
        case GalleryScreen::Controls: ControlsScreen(f, false); break;
        case GalleryScreen::Rebind: ControlsScreen(f, true); break;
        case GalleryScreen::Camera: CameraScreen(f); break;
        case GalleryScreen::Video: VideoScreen(f); break;
        case GalleryScreen::HudKickoff:
        case GalleryScreen::HudGoal:
        case GalleryScreen::HudScoreboard:
        case GalleryScreen::HudReplay: HudScreen(f, screen); break;
        case GalleryScreen::Count: break;
        }
    }
}
