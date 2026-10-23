// The UI kit of the Rocket League game (G1-kit) without a window or a GPU: the mapping of the input (repeat, text editing, the device used last,
// the control that was pressed for a rebind), the focus model (the default, the move by geometry, hover, confirm, scopes of the dialogs), the
// widgets (rows, sliders, text, bindings, tabs, dialogs, lists), the logic of the HUD (countdown, banners, feed, clock), the layout at several
// window sizes (720p ... 4K, 21:9) and a draw of every screen of the gallery at each of them. ImGui runs headless: a context with the baked fonts,
// the draw lists are read after the frame.

#include <imgui/imgui.h>

#include <myengine/ui/rl/RlUiFonts.h>
#include <myengine/ui/rl/RlUiFrame.h>
#include <myengine/ui/rl/RlUiGallery.h>
#include <myengine/ui/rl/RlUiHud.h>
#include <myengine/ui/rl/RlUiInput.h>
#include <myengine/ui/rl/RlUiPaint.h>
#include <myengine/ui/rl/RlUiWidgets.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    using namespace myengine::rlui;
    namespace fs = std::filesystem;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Near(const float a, const float b, const float tolerance = 1.5f) { return std::fabs(a - b) <= tolerance; }

    // ---------------------------------------------------------------- the harness

    constexpr int kEnter = 0x0D;
    constexpr int kEsc = 0x1B;
    constexpr int kLeft = 0x25;
    constexpr int kUp = 0x26;
    constexpr int kRight = 0x27;
    constexpr int kDown = 0x28;
    constexpr int kShift = 0x10;
    constexpr int kBackspace = 0x08;
    constexpr int kDelete = 0x2E;

    RawInput Keys(std::initializer_list<int> keys)
    {
        RawInput raw;
        for (const int key : keys)
        {
            raw.keys[key] = true;
        }
        return raw;
    }

    // A context of ImGui with the baked fonts of the kit and the frame that draws on its foreground list
    class Harness
    {
    public:
        Harness(const float width, const float height, const float uiScale = 1.0f) : width_(width), height_(height), uiScale_(uiScale)
        {
            context_ = ImGui::CreateContext();
            ImGui::SetCurrentContext(context_);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            const float scale = std::min(height / 1080.0f, width / 1920.0f) * uiScale;
            std::string error;
            const fs::path fontDirectory = fs::u8path(MYENGINE_SOURCE_DIR) / "assets" / "fonts";
            bakedOk_ = fonts_.Bake(*io.Fonts, fontDirectory, scale <= 0.75f ? 0.67f : (scale <= 1.1f ? 1.0f : (scale <= 1.7f ? 1.5f : 2.0f)), &error);
            Check(bakedOk_, "the fonts bake: " + error);
            unsigned char* pixels = nullptr;
            int atlasWidth = 0;
            int atlasHeight = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
            Check(pixels != nullptr && atlasWidth > 0, "the atlas is built");
            atlasPixels_ = static_cast<long long>(atlasWidth) * atlasHeight;
        }
        ~Harness()
        {
            ImGui::SetCurrentContext(context_);
            ImGui::DestroyContext(context_);
        }
        Harness(const Harness&) = delete;
        Harness& operator=(const Harness&) = delete;

        // One frame: Begin, the screen, End. The raw input is the devices of this frame.
        void Run(const std::function<void(Frame&)>& screen, const RawInput& raw, const float dt = 1.0f / 60.0f)
        {
            ImGui::SetCurrentContext(context_);
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(width_, height_);
            io.DeltaTime = dt;
            ImGui::NewFrame();
            FrameDesc desc;
            desc.width = width_;
            desc.height = height_;
            desc.uiScale = uiScale_;
            desc.deltaTime = dt;
            desc.time = time_;
            time_ += static_cast<double>(dt);
            const auto started = std::chrono::steady_clock::now();
            frame_.Begin(desc, raw, ImGui::GetForegroundDrawList(), &fonts_);
            screen(frame_);
            frame_.End();
            lastMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            ImDrawList* list = ImGui::GetForegroundDrawList();
            vertices_ = list->VtxBuffer.Size;
            minX_ = FLT_MAX;
            maxX_ = -FLT_MAX;
            minY_ = FLT_MAX;
            maxY_ = -FLT_MAX;
            for (int i = 0; i < list->VtxBuffer.Size; ++i)
            {
                minX_ = std::min(minX_, list->VtxBuffer[i].pos.x);
                maxX_ = std::max(maxX_, list->VtxBuffer[i].pos.x);
                minY_ = std::min(minY_, list->VtxBuffer[i].pos.y);
                maxY_ = std::max(maxY_, list->VtxBuffer[i].pos.y);
            }
            ImGui::Render();
        }

        Frame& frame() { return frame_; }
        double lastMilliseconds() const { return lastMs_; }
        int vertices() const { return vertices_; }
        float minX() const { return minX_; }
        float maxX() const { return maxX_; }
        float minY() const { return minY_; }
        float maxY() const { return maxY_; }
        long long atlasPixels() const { return atlasPixels_; }
        float width() const { return width_; }
        float height() const { return height_; }

    private:
        ImGuiContext* context_ = nullptr;
        Fonts fonts_;
        Frame frame_;
        float width_;
        float height_;
        float uiScale_;
        double time_ = 0.0;
        bool bakedOk_ = false;
        int vertices_ = 0;
        float minX_ = 0.0f;
        float maxX_ = 0.0f;
        float minY_ = 0.0f;
        float maxY_ = 0.0f;
        long long atlasPixels_ = 0;
        double lastMs_ = 0.0;
    };

    // ---------------------------------------------------------------- the input

    void TestInput()
    {
        InputMapper input;
        const float dt = 1.0f / 60.0f;
        input.Update(RawInput{}, dt);
        Check(!input.Pressed(Nav::Down), "nothing is pressed at the start");

        // a held direction: pressed at once, then after 0.35 s repeated 12 times a second
        int presses = 0;
        const RawInput down = Keys({kDown});
        for (int frame = 0; frame < 60; ++frame)
        {
            input.Update(down, dt);
            presses += input.Pressed(Nav::Down) ? 1 : 0;
        }
        Check(presses >= 7 && presses <= 9, "a key held for a second repeats about 8 times (" + std::to_string(presses) + ")");
        input.Update(RawInput{}, dt);
        input.Update(down, dt);
        Check(input.Pressed(Nav::Down), "a new press fires at once");
        input.Update(down, dt);
        Check(!input.Pressed(Nav::Down), "and not again in the next frame");

        // WASD navigate in the menu, but are letters in a text field
        InputMapper typing;
        typing.Update(RawInput{}, dt, true);
        typing.Update(Keys({'W'}), dt, true);
        Check(!typing.Pressed(Nav::Up), "W is a letter while a field is edited");
        InputMapper menu;
        menu.Update(RawInput{}, dt);
        menu.Update(Keys({'W'}), dt);
        Check(menu.Pressed(Nav::Up), "W moves up in the menu");

        // the pad: the D-pad, the stick past the half, the buttons; the device used last follows
        InputMapper pad;
        pad.Update(RawInput{}, dt);
        RawInput padInput;
        padInput.padConnected = true;
        padInput.padButtons = 1u << kPadA;
        pad.Update(padInput, dt);
        Check(pad.Pressed(Nav::Confirm) && pad.LastDevice() == Device::Gamepad, "A confirms and the pad is the device in use");
        RawInput stick;
        stick.padConnected = true;
        stick.padAxes[1] = 0.8f;
        pad.Update(stick, dt);
        Check(pad.Pressed(Nav::Up), "the stick up is Up");
        pad.Update(Keys({kEsc}), dt);
        Check(pad.Pressed(Nav::Back) && pad.LastDevice() == Device::KeyboardMouse, "Esc is Back and the keyboard is the device again");

        // the names and the capture of a rebind
        Check(TokenDisplayName("MouseRight") == "Right mouse" && TokenDisplayName("-S") == "S" && TokenDisplayName("PadRT") == "RT", "the names of the bindings");
        RawInput before;
        RawInput after = Keys({'J'});
        Check(CaptureToken(before, after, false) == "J", "a letter is captured");
        after = Keys({0x20});
        Check(CaptureToken(before, after, false) == "Space", "Space is captured");
        after = RawInput{};
        after.mouse[1] = true;
        Check(CaptureToken(before, after, false) == "MouseRight", "a mouse button is captured");
        Check(CaptureToken(Keys({'J'}), Keys({'J'}), false).empty(), "a key that was already down is not new");
        Check(CaptureToken(before, Keys({kEsc}), false).empty() && CaptureToken(before, Keys({kDelete}), false).empty(), "Esc and Del are the dialog's own");
        RawInput padNow;
        padNow.padConnected = true;
        padNow.padAxes[5] = 0.9f;
        Check(CaptureToken(RawInput{}, padNow, true) == "PadRT", "a trigger is captured on the pad side");
        RawInput stickNow;
        stickNow.padConnected = true;
        stickNow.padAxes[1] = -0.9f;
        Check(CaptureToken(RawInput{}, stickNow, true) == "-PadLeftY", "a stick direction carries its sign");
        Check(CaptureToken(before, Keys({'J'}), true).empty(), "the pad side ignores the keyboard");
    }

    // ---------------------------------------------------------------- the focus

    const Id kA = HashId("test/a");
    const Id kB = HashId("test/b");
    const Id kC = HashId("test/c");

    void ThreeButtons(Frame& f, bool* activated = nullptr)
    {
        f.SetDefaultFocus(kA);
        const bool a = MenuButton(f, kA, {76.0f, 370.0f, 520.0f, 72.0f}, "PLAY", Icon::Play);
        const bool b = MenuButton(f, kB, {76.0f, 452.0f, 520.0f, 72.0f}, "SETTINGS", Icon::Settings);
        const bool c = MenuButton(f, kC, {76.0f, 534.0f, 520.0f, 72.0f}, "EXIT", Icon::Exit);
        if (activated != nullptr)
        {
            activated[0] = a;
            activated[1] = b;
            activated[2] = c;
        }
    }

    void TestFocus()
    {
        Harness h(1920.0f, 1080.0f);
        bool activated[3] = {};
        const auto screen = [&](Frame& f) { ThreeButtons(f, activated); };
        h.Run(screen, RawInput{});
        Check(h.frame().Focus() == kA, "the first frame gives the focus to the default item");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kDown}));
        Check(h.frame().Focus() == kB, "Down moves the focus to the item below");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kDown}));
        Check(h.frame().Focus() == kC, "and again");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kDown}));
        Check(h.frame().Focus() == kC, "the focus does not wrap");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kUp}));
        Check(h.frame().Focus() == kB, "Up moves back");

        // confirm: once per press
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kEnter}));
        Check(activated[1] && !activated[0] && !activated[2], "Enter confirms the focused item");
        h.Run(screen, Keys({kEnter}));
        Check(!activated[1], "a held Enter confirms once");

        // the pointer: moving over an item focuses it, a click confirms it
        RawInput mouse;
        mouse.mousePosition = {10.0f, 10.0f};
        h.Run(screen, mouse);
        mouse.mousePosition = {300.0f, 570.0f};
        h.Run(screen, mouse);
        Check(h.frame().Focus() == kC, "the pointer over an item moves the focus to it");
        mouse.mouse[0] = true;
        h.Run(screen, mouse);
        Check(activated[2], "a click confirms the item under the pointer");
        mouse.mouse[0] = false;
        mouse.mousePosition = {300.0f, 400.0f};
        h.Run(screen, mouse);
        Check(h.frame().Focus() == kA, "and the focus follows the pointer to another one");

        // a disabled item takes no focus
        Harness d(1920.0f, 1080.0f);
        const auto withDisabled = [&](Frame& f)
        {
            f.SetDefaultFocus(kA);
            MenuButton(f, kA, {76.0f, 370.0f, 520.0f, 72.0f}, "A");
            MenuButton(f, kB, {76.0f, 452.0f, 520.0f, 72.0f}, "B", Icon::None, false);
            MenuButton(f, kC, {76.0f, 534.0f, 520.0f, 72.0f}, "C");
        };
        d.Run(withDisabled, RawInput{});
        d.Run(withDisabled, RawInput{});
        d.Run(withDisabled, Keys({kDown}));
        Check(d.frame().Focus() == kC, "a disabled item is skipped");

        // a screen that goes away: the focus lands on the default again
        Harness r(1920.0f, 1080.0f);
        r.Run(screen, RawInput{});
        r.Run(screen, Keys({kDown}));
        r.frame().ResetFocus();
        r.Run(screen, RawInput{});
        Check(r.frame().Focus() == kA, "ResetFocus brings the default focus back");
    }

    // ---------------------------------------------------------------- the widgets

    void TestRows()
    {
        // an option row: the arrows change the choice, the ends hold
        Harness h(1920.0f, 1080.0f);
        int choice = 1;
        const std::string_view options[] = {"Rookie", "Pro", "All-star"};
        const auto screen = [&](Frame& f)
        {
            f.SetDefaultFocus(HashId("row"));
            choice = OptionRow(f, HashId("row"), {80.0f, 700.0f, 1100.0f, 64.0f}, "Bot difficulty", Span<std::string_view>(options), choice);
        };
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kRight}));
        Check(choice == 2, "Right chooses the next option");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kRight}));
        Check(choice == 2, "the last option holds");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kLeft}));
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kLeft}));
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kLeft}));
        Check(choice == 0, "Left walks back to the first");
        // a click on the second segment
        RawInput click;
        click.mousePosition = {10.0f, 10.0f};
        h.Run(screen, click);
        // the segments start at the content column (x 80 + 346); "Rookie" is about 100 px wide: the second one is right of x 560
        click.mousePosition = {80.0f + 346.0f + 140.0f, 732.0f};
        h.Run(screen, click);
        click.mouse[0] = true;
        h.Run(screen, click);
        Check(choice == 1, "a click on a segment chooses it (" + std::to_string(choice) + ")");

        // a toggle
        Harness t(1920.0f, 1080.0f);
        bool value = false;
        const auto toggle = [&](Frame& f) { f.SetDefaultFocus(HashId("t")); value = ToggleRow(f, HashId("t"), {80.0f, 300.0f, 1000.0f, 58.0f}, "VSync", value); };
        t.Run(toggle, RawInput{});
        t.Run(toggle, Keys({kRight}));
        Check(value, "Right turns a toggle on");
        t.Run(toggle, RawInput{});
        t.Run(toggle, Keys({kLeft}));
        Check(!value, "Left turns it off");

        // a slider: step, Shift x5, the ends
        Harness s(1920.0f, 1080.0f);
        float slider = 100.0f;
        const auto sliderScreen = [&](Frame& f)
        {
            f.SetDefaultFocus(HashId("s"));
            slider = SliderRow(f, HashId("s"), {80.0f, 290.0f, 1000.0f, 58.0f}, "Field of view", slider, 60.0f, 110.0f, 1.0f, std::to_string(static_cast<int>(slider)));
        };
        s.Run(sliderScreen, RawInput{});
        s.Run(sliderScreen, Keys({kRight}));
        Check(Near(slider, 101.0f, 1e-4f), "Right steps a slider by one step");
        s.Run(sliderScreen, RawInput{});
        s.Run(sliderScreen, Keys({kRight, kShift}));
        Check(Near(slider, 106.0f, 1e-4f), "Shift makes the step five times bigger");
        s.Run(sliderScreen, RawInput{});
        s.Run(sliderScreen, Keys({kRight, kShift}));
        Check(Near(slider, 110.0f, 1e-4f), "the end of the range holds");
        s.Run(sliderScreen, RawInput{});
        s.Run(sliderScreen, Keys({kLeft}));
        Check(Near(slider, 109.0f, 1e-4f), "Left steps back");
        // dragging with the pointer: the middle of the track is the middle of the range
        RawInput drag;
        drag.mousePosition = {10.0f, 10.0f};
        s.Run(sliderScreen, drag);
        const float trackLeft = 80.0f + 395.0f;
        drag.mousePosition = {trackLeft + 215.0f, 318.0f};
        s.Run(sliderScreen, drag);
        drag.mouse[0] = true;
        s.Run(sliderScreen, drag);
        Check(Near(slider, 85.0f, 1.01f), "the pointer sets a slider (" + std::to_string(slider) + ")");

        // a stepper (the resolution)
        Harness e(1920.0f, 1080.0f);
        int stepped = 0;
        const auto stepper = [&](Frame& f)
        {
            f.SetDefaultFocus(HashId("e"));
            stepped = StepperRow(f, HashId("e"), {80.0f, 358.0f, 1100.0f, 64.0f}, "Resolution", "2560 x 1440", "16:9");
        };
        e.Run(stepper, RawInput{});
        e.Run(stepper, Keys({kLeft}));
        Check(stepped == -1, "Left steps a stepper back");
        e.Run(stepper, RawInput{});
        e.Run(stepper, Keys({kRight}));
        Check(stepped == 1, "Right steps it forward");

        // a text row: the allowed characters, the length, Backspace
        Harness x(1920.0f, 1080.0f);
        std::string address;
        bool changed = false;
        const auto field = [&](Frame& f)
        {
            f.SetDefaultFocus(HashId("x"));
            changed = TextRow(f, HashId("x"), {886.0f, 452.0f, 618.0f, 58.0f}, "Address", address, 8, "0123456789.:");
        };
        RawInput typed;
        x.Run(field, typed);
        typed.typed = "12a.5";
        x.Run(field, typed);
        Check(changed && address == "12.5", "only the allowed characters are typed (" + address + ")");
        typed.typed = "1234567890";
        x.Run(field, typed);
        Check(address == "12.51234", "the length is limited (" + address + ")");
        typed.typed.clear();
        x.Run(field, typed);
        RawInput erase = Keys({kBackspace});
        x.Run(field, erase);
        Check(address == "12.5123", "Backspace erases one character (" + address + ")");

        // the letters of the keys do not move the focus out of a field being typed in
        Harness y(1920.0f, 1080.0f);
        std::string name = "";
        const auto two = [&](Frame& f)
        {
            f.SetDefaultFocus(HashId("name"));
            TextRow(f, HashId("name"), {80.0f, 300.0f, 618.0f, 58.0f}, "Name", name, 8);
            ValueRow(f, HashId("below"), {80.0f, 362.0f, 618.0f, 58.0f}, "Port", "27015");
        };
        y.Run(two, RawInput{});
        y.Run(two, RawInput{});
        y.Run(two, Keys({'S'}));
        Check(y.frame().Focus() == HashId("name"), "S typed in a field does not move down");
        y.Run(two, RawInput{});
        y.Run(two, Keys({kDown}));
        Check(y.frame().Focus() == HashId("below"), "the arrow does");
    }

    void TestBindingsAndTabs()
    {
        Harness h(1920.0f, 1080.0f);
        BindRowResult last;
        const auto screen = [&](Frame& f)
        {
            f.SetDefaultFocus(HashId("jump"));
            last = BindRow(f, HashId("jump"), {80.0f, 530.0f, 1100.0f, 58.0f}, "Jump", "Right mouse", "A");
        };
        h.Run(screen, RawInput{});
        h.Run(screen, RawInput{});   // the focus is given at the end of the first frame
        Check(last.focused && last.plate == 0, "the keyboard plate is chosen first");
        h.Run(screen, Keys({kRight}));
        Check(last.plate == 1, "Right chooses the pad plate");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kEnter}));
        Check(last.rebind && last.plate == 1, "Enter asks to rebind the chosen plate");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kDelete}));
        Check(last.clear, "Del clears");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({'R'}));
        Check(last.resetRow, "R resets the row");

        // tabs: Q and E
        Harness t(1920.0f, 1080.0f);
        int tab = 1;
        const std::string_view labels[] = {"CONTROLS", "CAMERA", "VIDEO", "AUDIO"};
        const auto tabs = [&](Frame& f) { tab = Tabs(f, HashId("tabs"), {75.0f, 200.0f, 700.0f, 54.0f}, Span<std::string_view>(labels), tab); };
        t.Run(tabs, RawInput{});
        t.Run(tabs, Keys({'E'}));
        Check(tab == 2, "E is the next tab");
        t.Run(tabs, RawInput{});
        t.Run(tabs, Keys({'Q'}));
        t.Run(tabs, RawInput{});
        t.Run(tabs, Keys({'Q'}));
        t.Run(tabs, RawInput{});
        t.Run(tabs, Keys({'Q'}));
        Check(tab == 0, "Q walks back and stops at the first");
        RawInput pad;
        pad.padConnected = true;
        pad.padButtons = 1u << kPadRB;
        t.Run(tabs, RawInput{});
        t.Run(tabs, pad);
        Check(tab == 1, "RB is the next tab too");
    }

    void TestDialogs()
    {
        Harness h(1920.0f, 1080.0f);
        DialogResult result = DialogResult::None;
        bool open = true;
        const Id dialog = HashId("quit");
        const auto screen = [&](Frame& f)
        {
            f.SetDefaultFocus(kA);
            MenuButton(f, kA, {76.0f, 370.0f, 520.0f, 72.0f}, "PLAY");
            MenuButton(f, kB, {76.0f, 452.0f, 520.0f, 72.0f}, "EXIT");
            if (open)
            {
                result = ConfirmDialog(f, dialog, {"QUIT TO DESKTOP?", "Your settings are saved."});
            }
        };
        h.Run(screen, RawInput{});
        h.Run(screen, RawInput{});
        h.Run(screen, RawInput{});
        Check(h.frame().Focus() == HashId("cancel", dialog), "a dialog takes the focus and the safe button has it (" + std::to_string(h.frame().Focus()) + ")");
        // the screen below is out of reach
        h.Run(screen, Keys({kUp}));
        Check(h.frame().Focus() == HashId("cancel", dialog), "the items below a dialog are not reachable");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kRight}));
        Check(h.frame().Focus() == HashId("confirm", dialog), "Right moves to Confirm");
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kEnter}));
        Check(result == DialogResult::Confirm, "Enter on Confirm confirms");
        // Back cancels
        h.Run(screen, RawInput{});
        h.Run(screen, Keys({kEsc}));
        Check(result == DialogResult::Cancel, "Back cancels");
        // closed: the focus is where it was
        open = false;
        h.Run(screen, RawInput{});
        h.Run(screen, RawInput{});
        Check(h.frame().Focus() == kA, "after the dialog the focus is back on the screen (" + std::to_string(h.frame().Focus()) + ")");
    }

    void TestScrollView()
    {
        Harness h(1920.0f, 1080.0f);
        const Id list = HashId("list");
        const auto screen = [&](Frame& f)
        {
            f.SetDefaultFocus(HashId("item/0"));
            ScrollView view(f, list, {80.0f, 300.0f, 1000.0f, 400.0f}, 30.0f * 62.0f);
            for (int i = 0; i < 30; ++i)
            {
                ValueRow(f, HashId("item/" + std::to_string(i)), {80.0f, 300.0f + 62.0f * static_cast<float>(i), 1000.0f, 58.0f}, "Row", "x");
            }
        };
        h.Run(screen, RawInput{});
        h.Run(screen, RawInput{});
        Check(h.frame().FloatState(list) == 0.0f, "the list starts at the top");
        for (int i = 0; i < 12; ++i)
        {
            h.Run(screen, RawInput{});
            h.Run(screen, Keys({kDown}));
        }
        h.Run(screen, RawInput{});
        h.Run(screen, RawInput{});
        Check(h.frame().Focus() == HashId("item/12"), "twelve Downs reach the twelfth row");
        const float offset = h.frame().FloatState(list);
        Check(offset > 0.0f, "the list scrolled to the focused row (" + std::to_string(offset) + ")");
        const float rowTop = 62.0f * 12.0f - offset;   // relative to the top of the view
        Check(rowTop >= 0.0f && rowTop + 58.0f <= 400.0f + 1.0f, "the focused row is inside the window (top " + std::to_string(rowTop) + ")");
        // the wheel
        RawInput wheel;
        wheel.mousePosition = {500.0f, 500.0f};
        h.Run(screen, wheel);
        wheel.mouseWheel = 2.0f;
        h.Run(screen, wheel);
        Check(h.frame().FloatState(list) < offset, "the wheel scrolls up");
    }

    // ---------------------------------------------------------------- the HUD

    HudState SampleState(const HudPhase phase)
    {
        HudState s;
        s.phase = phase;
        s.mode = 2;
        s.localPlayer = 0;
        s.players.resize(4);
        s.players[0] = {"Artem", 0, false, 420, 2, 0, 1, 3, 0, 0};
        s.players[1] = {"Bot Comet", 0, true, 260, 0, 1, 2, 1, 1, -1};
        s.players[2] = {"Bot Blaze", 1, true, 380, 1, 1, 0, 2, 2, -1};
        s.players[3] = {"Bot Nova", 1, true, 300, 1, 0, 1, 2, 0, -1};
        return s;
    }

    void TestHudLogic()
    {
        Hud hud;
        HudState s = SampleState(HudPhase::Countdown);
        s.clock = 300.0f;
        s.phaseTimer = 2.9f;
        hud.Update(0.016f, s);
        Check(hud.CountdownText() == "3", "the countdown shows 3 at 2.9 s");
        s.phaseTimer = 1.5f;
        hud.Update(0.016f, s);
        Check(hud.CountdownText() == "2", "2 at 1.5 s");
        s.phaseTimer = 0.4f;
        hud.Update(0.016f, s);
        Check(hud.CountdownText() == "1", "1 at 0.4 s");
        s.phase = HudPhase::Playing;
        s.phaseTimer = 0.0f;
        hud.Update(0.016f, s);
        Check(hud.CountdownText() == "GO!", "GO! when the match starts");
        for (int i = 0; i < 40; ++i)
        {
            hud.Update(0.016f, s);
        }
        Check(hud.CountdownText().empty(), "GO! is gone after half a second");

        // the clock and the overtime
        s.clock = 272.0f;
        hud.Update(0.016f, s);
        Check(hud.ClockText() == "4:32", "the clock reads m:ss (" + hud.ClockText() + ")");
        s.clock = 0.4f;
        hud.Update(0.016f, s);
        Check(hud.ClockText() == "0:01", "a part of a second counts as the second");
        s.overtime = true;
        s.clock = 42.3f;
        hud.Update(0.016f, s);
        Check(hud.ClockText() == "+0:42", "the overtime counts up (" + hud.ClockText() + ")");
        Check(hud.BannerTitle() == "OVERTIME!", "the overtime has a banner");
        for (int i = 0; i < 120; ++i)
        {
            hud.Update(0.016f, s);
        }
        Check(hud.BannerTitle().empty(), "the banner of the overtime is gone after 1.5 s");

        // a goal
        s.overtime = false;
        s.phase = HudPhase::GoalScored;
        s.phaseTimer = 3.0f;
        s.score[0] = 1;
        s.lastGoalTeam = 0;
        s.lastGoalScorer = 0;
        s.lastGoalSpeedKmh = 98.2f;
        hud.Update(0.016f, s);
        Check(hud.BannerTitle() == "GOAL!" && hud.BannerSubtitle() == "ARTEM \xC2\xB7 98 km/h", "a goal: the banner names the scorer and the speed (" + hud.BannerSubtitle() + ")");
        for (int i = 0; i < 400; ++i)
        {
            hud.Update(0.016f, s);
        }
        Check(hud.BannerTitle().empty(), "the banner goes after its time");
        // an own goal
        Hud own;
        HudState o = SampleState(HudPhase::Playing);
        own.Update(0.016f, o);
        o.phase = HudPhase::GoalScored;
        o.phaseTimer = 3.0f;
        o.lastGoalOwn = true;
        o.lastGoalTeam = 1;
        o.lastGoalScorer = 0;
        own.Update(0.016f, o);
        Check(own.BannerTitle() == "OWN GOAL", "an own goal has its own banner");

        // the feed: newest first, at most five, each lives five seconds
        Hud feed;
        HudState f = SampleState(HudPhase::Playing);
        feed.Update(0.016f, f);
        for (int i = 0; i < 7; ++i)
        {
            feed.PushEvent({HudEventType::Goal, i % 4, -1});
        }
        Check(feed.FeedSize() == 5, "the feed keeps five lines");
        for (int i = 0; i < 250; ++i)
        {
            feed.Update(0.016f, f);
        }
        Check(feed.FeedSize() == 5, "they live a little less than five seconds");
        for (int i = 0; i < 100; ++i)
        {
            feed.Update(0.016f, f);
        }
        Check(feed.FeedSize() == 0, "and then go");

        // a demolition of the player shows who did it
        Hud demo;
        HudState d = SampleState(HudPhase::Playing);
        demo.Update(0.016f, d);
        demo.PushEvent({HudEventType::Demo, 2, 0});
        Check(demo.DemolitionTitle() == "DEMOLISHED BY BOT BLAZE", "the victim sees who demolished it (" + demo.DemolitionTitle() + ")");
        Hud author;
        author.Update(0.016f, d);
        author.PushEvent({HudEventType::Demo, 0, 2});
        Check(author.DemolitionTitle() == "DEMOLITION!", "the author sees DEMOLITION!");
        Hud other;
        other.Update(0.016f, d);
        other.PushEvent({HudEventType::Demo, 2, 3});
        Check(other.DemolitionTitle().empty() && other.FeedSize() == 1, "a demolition between others is only in the feed");
    }

    // The training component: every phase draws, the banner adds to the drawing, a pack without attempts and free play draw as well, and a HUD
    // whose training is not active draws the match (the scoreboard) and nothing of the training.
    void TestHudTraining()
    {
        const auto draw = [](const HudTraining& training, const bool active)
        {
            Harness h(1920.0f, 1080.0f);
            Hud hud;
            HudState s = SampleState(HudPhase::Playing);
            s.training = training;
            s.training.active = active;
            for (int frame = 0; frame < 3; ++frame)
            {
                h.Run([&](Frame& f) { hud.Update(f.DeltaTime(), s); hud.Draw(f, false); }, RawInput{});
            }
            return h.vertices();
        };
        HudTraining t;
        t.pack = "Striker";
        t.exercise = "Rolling ball";
        t.hint = "Catch up with the ball and shoot.";
        t.index = 2;
        t.count = 8;
        t.attempt = 2;
        t.attempts = 3;
        t.timeLimit = 10.0f;
        t.timeLeft = 6.0f;
        t.elapsed = 4.0f;
        t.recordTime = 4.2f;
        t.recordHitKmh = 112.0f;
        const int plain = draw(t, false);

        t.phase = HudTrainingPhase::Running;
        const int running = draw(t, true);
        Check(running > 300, "the training plate and the timer draw (" + std::to_string(running) + " vertices)");
        t.phase = HudTrainingPhase::Ready;
        Check(draw(t, true) > running, "GET READY adds to the plate");
        t.phase = HudTrainingPhase::Success;
        t.bannerTime = 0.8f;
        t.lastHitKmh = 98.0f;
        t.newBest = true;
        const int success = draw(t, true);
        Check(success > running + 100, "the SUCCESS banner is drawn (" + std::to_string(success) + " vs " + std::to_string(running) + ")");
        t.phase = HudTrainingPhase::Miss;
        t.missReason = "TIME UP";
        Check(draw(t, true) > running + 100, "the MISS banner is drawn");
        t.phase = HudTrainingPhase::PackDone;
        t.done = 6;
        Check(draw(t, true) > plain + 100, "the summary of the pack is drawn");
        t.phase = HudTrainingPhase::FreePlay;
        t.pack = "Free play";
        t.timeLimit = 0.0f;
        t.hits = 3;
        t.infiniteBoost = true;
        Check(draw(t, true) > plain + 100, "free play is drawn with its counters");
        Check(plain > 100 && plain < running + 2000, "an inactive training is the usual match HUD (" + std::to_string(plain) + ")");
        // the keys of the game replace the usual ones
        t.phase = HudTrainingPhase::Running;
        t.hints = {{"F5", "A", "Restart"}};
        Check(draw(t, true) > 200, "own hints draw");
    }

    // ---------------------------------------------------------------- the layout

    void TestLayout()
    {
        // the metrics
        {
            Harness h(2560.0f, 1080.0f);
            h.Run([](Frame&) {}, RawInput{});
            const Metrics& m = h.frame().metrics();
            Check(Near(m.scale, 1.0f, 1e-4f) && Near(m.menu.x, 320.0f, 0.01f) && Near(m.menu.w, 1920.0f, 0.01f), "21:9: the menu column is the 16:9 area in the middle");
            Check(Near(m.designWindow.x, 2560.0f, 0.01f), "the HUD sees the whole window");
        }
        {
            Harness h(1280.0f, 720.0f);
            h.Run([](Frame&) {}, RawInput{});
            Check(Near(h.frame().metrics().scale, 2.0f / 3.0f, 1e-3f), "720p scales the design by 2/3");
        }
        {
            Harness h(1920.0f, 1080.0f, 1.25f);
            h.Run([](Frame&) {}, RawInput{});
            Check(Near(h.frame().metrics().scale, 1.25f, 1e-4f), "the UI scale multiplies it");
        }

        // the scoreboard is in the middle and the boost ring in the corner, at every size and shape of the window
        const float sizes[][2] = {{1280.0f, 720.0f}, {1920.0f, 1080.0f}, {2560.0f, 1440.0f}, {3840.0f, 2160.0f}, {2560.0f, 1080.0f}, {3440.0f, 1440.0f}};
        for (const auto& size : sizes)
        {
            const std::string label = std::to_string(static_cast<int>(size[0])) + "x" + std::to_string(static_cast<int>(size[1]));
            Hud hud;
            HudState s = SampleState(HudPhase::Playing);
            s.clock = 272.0f;
            s.boost = 33.0f;
            {
                Harness h(size[0], size[1]);
                hud.Update(0.016f, s);
                h.Run([&](Frame& f) { f.SetSpace(Space::Screen); hud.DrawScorebar(f); }, RawInput{});
                const float centre = (h.minX() + h.maxX()) * 0.5f;
                Check(Near(centre, size[0] * 0.5f, 3.0f), label + ": the scoreboard is in the middle of the window (" + std::to_string(centre) + ")");
                const float scale = h.frame().metrics().scale;
                Check(Near(h.minY(), 22.0f * scale, 2.0f), label + ": the scoreboard is 22 px from the top");
                Check(Near(h.maxX() - h.minX(), 466.0f * scale, 8.0f * scale), label + ": the scoreboard is 466 px wide at 1080p (" + std::to_string((h.maxX() - h.minX()) / scale) + ")");
            }
            {
                Harness h(size[0], size[1]);
                h.Run([&](Frame& f) { f.SetSpace(Space::Screen); hud.DrawBoostRing(f); }, RawInput{});
                const float scale = h.frame().metrics().scale;
                Check(Near(h.maxX(), size[0] - 60.0f * scale, 3.0f), label + ": the boost ring is 60 px from the right edge (" + std::to_string(h.maxX()) + ")");
                Check(Near(h.maxY(), size[1] - 50.0f * scale, 3.0f), label + ": and 50 px from the bottom (" + std::to_string(h.maxY()) + ")");
            }
            {
                // a menu button: its left edge is in the menu column
                Harness h(size[0], size[1]);
                h.Run([&](Frame& f) { MenuButton(f, kA, {76.0f, 370.0f, 520.0f, 72.0f}, "PLAY", Icon::Play); }, RawInput{});
                const Metrics& m = h.frame().metrics();
                Check(h.minX() >= m.menu.x + 70.0f * m.scale - 20.0f * m.scale && h.minX() <= m.menu.x + 80.0f * m.scale, label + ": the menu button is in the column");
            }
        }
    }

    void TestGalleryDraw()
    {
        const float sizes[][2] = {{1280.0f, 720.0f}, {1920.0f, 1080.0f}, {3840.0f, 2160.0f}, {2560.0f, 1080.0f}};
        for (const auto& size : sizes)
        {
            Harness h(size[0], size[1]);
            for (int i = 0; i < static_cast<int>(GalleryScreen::Count); ++i)
            {
                const GalleryScreen screen = static_cast<GalleryScreen>(i);
                GalleryScreen parsed;
                Check(ParseGalleryScreen(GalleryScreenName(screen), parsed) && parsed == screen, std::string("the name of ") + GalleryScreenName(screen) + " parses");
                for (int frame = 0; frame < 3; ++frame)
                {
                    h.Run([&](Frame& f) { DrawGalleryScreen(f, screen); }, RawInput{});
                }
                Check(h.vertices() > 400, std::string(GalleryScreenName(screen)) + ": the screen draws (" + std::to_string(h.vertices()) + " vertices)");
                Check(h.vertices() < 150000, std::string(GalleryScreenName(screen)) + ": and not too much (" + std::to_string(h.vertices()) + " vertices)");
            }
            Check(h.atlasPixels() > 0, "the atlas exists");
        }
        // the fonts of every size of the spec exist
        Harness fonts(1920.0f, 1080.0f);
        fonts.Run([](Frame& f)
        {
            for (int style = 0; style < static_cast<int>(FontStyle::Count); ++style)
            {
                for (const float size : Fonts::DesignSizes(static_cast<FontStyle>(style)))
                {
                    Check(f.fonts().Get(static_cast<FontStyle>(style), size).Valid(), "the font style " + std::to_string(style) + " at " + std::to_string(size) + " is baked");
                }
            }
            Check(f.fonts().Icons(24.0f).Valid(), "the icons are baked");
            // the names of the players can be Cyrillic: the text styles have it, the big sizes of the banners only what they need
            Check(f.fonts().Get(FontStyle::SemiBold, 26.0f).font->IsGlyphInFont(0x0416), "SemiBold 26 has the Cyrillic");
            Check(f.fonts().Get(FontStyle::ExtraBoldItalic, 34.0f).font->IsGlyphInFont(0x0416), "ExtraBold Italic 34 has the Cyrillic (the name under GOAL!)");
            Check(f.fonts().Get(FontStyle::BlackItalic, 220.0f).font->IsGlyphInFont('3') && f.fonts().Get(FontStyle::BlackItalic, 220.0f).font->IsGlyphInFont('G'),
                  "the countdown font has the digits and GO");
        }, RawInput{});
    }

    // ---------------------------------------------------------------- the cost of a frame

    void TestCost()
    {
        Harness h(1920.0f, 1080.0f);
        Hud hud;
        HudState s = SampleState(HudPhase::Playing);
        s.clock = 12.0f;
        s.boosting = true;
        hud.Update(0.016f, s);
        for (int i = 0; i < 5; ++i)
        {
            hud.PushEvent({HudEventType::Demo, 2, 1});
        }
        h.Run([&](Frame& f) { f.SetSpace(Space::Screen); hud.Draw(f, true); }, RawInput{});
        const int vertices = h.vertices();
        Check(vertices > 0 && vertices < 40000, "a full HUD with the table is a modest draw list (" + std::to_string(vertices) + " vertices)");

        // the CPU time of the HUD frame of a 4v4 match: scoreboard, ring, banner, feed, table (the median of 300 frames, builds of the draw list only)
        HudState big = SampleState(HudPhase::GoalScored);
        big.players.resize(8);
        for (int i = 0; i < 8; ++i)
        {
            big.players[static_cast<std::size_t>(i)] = {"Player " + std::to_string(i), i / 4, i != 0, 100 * i, i, i, i, i, i, 20};
        }
        big.mode = 4;
        big.phaseTimer = 60.0f;
        big.lastGoalTeam = 0;
        big.lastGoalScorer = 0;
        big.lastGoalSpeedKmh = 98.0f;
        big.clock = 25.0f;
        big.boosting = true;
        hud.Update(0.016f, big);
        hud.Update(0.016f, big);
        for (int i = 0; i < 5; ++i)
        {
            hud.PushEvent({HudEventType::Goal, i, -1});
        }
        std::vector<double> times;
        for (int frame = 0; frame < 300; ++frame)
        {
            hud.Update(0.002f, big);
            h.Run([&](Frame& f) { f.SetSpace(Space::Screen); hud.Draw(f, frame % 2 == 0); }, RawInput{}, 0.002f);
            times.push_back(h.lastMilliseconds());
        }
        std::sort(times.begin(), times.end());
        const double median = times[times.size() / 2];
        std::cout << "HUD frame (4v4, table half of the frames): median " << median << " ms, worst " << times.back() << " ms, " << h.vertices() << " vertices" << std::endl;
        Check(median < 1.0, "the HUD costs under 1 ms of the CPU (" + std::to_string(median) + " ms)");
    }

    // ---------------------------------------------------------------- sharp text

    // The text of the kit is sharp when its glyphs lie on the pixels as they were rasterised: the font is baked at a whole number of pixels, the text is
    // drawn at that size (a fraction of a pixel in the size scales every glyph and puts its edges between two pixels) and the glyphs stand on whole pixels.
    void TestSharpText()
    {
        Harness h(1920.0f, 1080.0f);
        h.Run([](Frame& f)
        {
            for (int style = 0; style < static_cast<int>(FontStyle::Count); ++style)
            {
                for (const float size : Fonts::DesignSizes(static_cast<FontStyle>(style)))
                {
                    const FontHandle handle = f.fonts().Get(static_cast<FontStyle>(style), size);
                    const std::string label = "style " + std::to_string(style) + " at " + std::to_string(size);
                    Check(handle.pixelSize >= 1.0f && handle.pixelSize == std::round(handle.pixelSize), label + ": baked at a whole number of pixels");
                    Check(handle.DrawSize(size, 1.0f) == handle.pixelSize, label + ": drawn at the baked size on the screen it was baked for");
                    Check(std::fabs(handle.DrawSize(size, 1.0f) - size * handle.lineScale) <= 0.5f + 1e-3f, label + ": and that is within half a pixel of the design");
                    Check(handle.DrawSize(size, 1.3f) > handle.pixelSize + 0.5f, label + ": a screen far from the baked class draws at the wanted size");
                    Check(handle.DrawSize(size, 1.02f) == handle.pixelSize || std::fabs(size * 1.02f * handle.lineScale - handle.pixelSize) > 0.5f,
                          label + ": a few per cent off the class keeps the baked size while it is within half a pixel");
                }
            }
            for (const float size : {16.0f, 20.0f, 30.0f})
            {
                const FontHandle icons = f.fonts().Icons(size);
                Check(icons.Valid() && icons.pixelSize == std::round(icons.pixelSize), "the icons are baked at a whole number of pixels");
            }

            // text at positions that are not whole pixels: every corner of every glyph is a whole pixel (so the texels land 1:1 on the screen pixels)
            paint::Text(f, FontStyle::ExtraBoldItalic, 22.0f, {101.37f, 203.61f}, Color::Hex(0xFFFFFF), "Windowed Borderless Fullscreen 144 Hz");
            paint::Text(f, FontStyle::SemiBold, 18.0f, {333.5f, 411.25f}, Color::Hex(0xFFFFFF), "Window mode and resolution ask to keep the change");
            paint::Text(f, FontStyle::BlackItalic, 84.0f, {640.75f, 100.5f}, Color::Hex(0xFFFFFF), "VIDEO");
            paint::Text(f, FontStyle::Bold, 24.0f, {900.3f, 300.8f}, Color::Hex(0xFFFFFF), "Resolution", Align::Center);
            const ImDrawList* list = ImGui::GetForegroundDrawList();
            Check(list->VtxBuffer.Size >= 4 * 80, "the text made its glyph quads (" + std::to_string(list->VtxBuffer.Size) + " vertices)");
            int offGrid = 0;
            for (int i = 0; i < list->VtxBuffer.Size; ++i)
            {
                const ImVec2 p = list->VtxBuffer[i].pos;
                if (std::fabs(p.x - std::round(p.x)) > 1e-3f || std::fabs(p.y - std::round(p.y)) > 1e-3f)
                {
                    ++offGrid;
                }
            }
            Check(offGrid == 0, "every glyph corner is on a whole pixel (" + std::to_string(offGrid) + " are not)");
        }, RawInput{});
    }

    // The atlas of the game UI is rasterised by FreeType while the atlas of a window already holds the fonts of the editor (stb_truetype): the loader is
    // changed under them. The glyphs are there, and the atlas is smaller than the oversampled one.
    void TestFontLoaderSwitch()
    {
        ImGuiContext* context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // the editor's fonts as UiManager makes them: a text font without its private use area, the icons merged into it, an icon-only font
        const fs::path fontRoot = fs::u8path(MYENGINE_SOURCE_DIR) / "assets" / "fonts";
        static const ImWchar kExcludePrivateUse[] = {0xE000, 0xF8FF, 0};
        static const ImWchar kIcon[] = {0xE140, 0xE140, 0};
        ImFontConfig text;
        text.GlyphExcludeRanges = kExcludePrivateUse;
        ImFont* editorText = io.Fonts->AddFontFromFileTTF((fontRoot / "Inter-Regular.ttf").string().c_str(), 17.0f, &text);
        ImFontConfig merged;
        merged.MergeMode = true;
        merged.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF((fontRoot / "lucide.ttf").string().c_str(), 14.0f, &merged, kIcon);
        ImFontConfig iconOnly;
        iconOnly.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF((fontRoot / "lucide.ttf").string().c_str(), 18.0f, &iconOnly, kIcon);
        if (editorText == nullptr)
        {
            io.Fonts->AddFontDefault();
        }
        Fonts fonts;
        std::string error;
        const bool baked = fonts.Bake(*io.Fonts, fs::u8path(MYENGINE_SOURCE_DIR) / "assets" / "fonts", 1.0f, &error);
        Check(baked, "the kit bakes into an atlas that has a font already: " + error);
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        Check(pixels != nullptr && width > 0 && height > 0, "the atlas is built");
        const FontHandle handle = fonts.Get(FontStyle::ExtraBoldItalic, 22.0f);
        Check(handle.Valid() && handle.font->IsGlyphInFont('W') && handle.font->IsGlyphInFont(0x0416), "the glyphs of the kit are in the atlas");
        ImGui::DestroyContext(context);
    }
}

int main()
{
    try
    {
        TestInput();
        TestFocus();
        TestRows();
        TestBindingsAndTabs();
        TestDialogs();
        TestScrollView();
        TestHudLogic();
        TestHudTraining();
        TestLayout();
        TestGalleryDraw();
        TestSharpText();
        TestFontLoaderSwitch();
        TestCost();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "rl_ui tests passed\n";
    return 0;
}
