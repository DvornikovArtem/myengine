// RlUiFrame.h
//
// The frame of the kit: the metrics (design px -> screen px), the input, the focus model, the scopes of the dialogs, the transition of a
// screen and the toasts. A screen (coder-2) gets a Frame& from the Host every frame and calls the widgets (RlUiWidgets.h) with it:
//
//     void MainMenu(rlui::Frame& f)
//     {
//         f.SetDefaultFocus(rlui::HashId("main/play"));
//         if (rlui::MenuButton(f, rlui::HashId("main/play"), {80, 370, 520, 72}, "PLAY", rlui::Icon::Play)) { GoTo(Screen::Play); }
//         ...
//         if (f.input().Pressed(rlui::Nav::Back)) { f.SetFocus(rlui::HashId("main/exit")); }
//     }
//
// The focus (§5.1): exactly one item has it, it is always visible (a white plate with a stripe of the accent colour), whatever device the
// player uses. Arrows / D-pad / the stick move it by the geometry of the items of the frame before (the nearest item in the direction),
// the pointer moves it by hovering, Enter / Space / A / a click confirms (the widget returns true). A widget that uses ← / → itself (a
// slider, a segmented row) takes the event first: Frame::TakeNav. Dialogs take the focus with a scope: PushScope(id) ... PopScope(); items
// of the scopes below do not react and cannot be reached.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <myengine/ui/rl/RlUiCommon.h>
#include <myengine/ui/rl/RlUiFonts.h>
#include <myengine/ui/rl/RlUiInput.h>
#include <myengine/ui/rl/RlUiTheme.h>

struct ImDrawList;

namespace myengine::rlui
{
    struct FrameDesc
    {
        float width = 1920.0f;       // the window, pixels
        float height = 1080.0f;
        float uiScale = 1.0f;        // the setting, 0.75 .. 1.25
        float deltaTime = 1.0f / 60.0f;
        double time = 0.0;           // seconds since the start: the animations (the pulse of the boost, the beams of the background)
    };

    // The scaling of a frame. scale = min(height / 1080, width / 1920) * uiScale (a window narrower than 16:9 shrinks the menu to fit).
    struct Metrics
    {
        float scale = 1.0f;
        Vec2 window{1920.0f, 1080.0f};   // pixels
        Rect menu{0.0f, 0.0f, 1920.0f, 1080.0f};  // the menu column in pixels: the 1920 x 1080 canvas scaled by `scale`, centred (21:9: pillars at the sides)
        Vec2 designWindow{1920.0f, 1080.0f};      // the whole window in design px (width / scale, height / scale): the HUD anchors to it
    };

    // Which design space the coordinates of the widgets and of the paint functions are in
    enum class Space : std::uint8_t
    {
        Menu,    // the 1920 x 1080 canvas, centred in the window (screens, dialogs)
        Screen,  // the window itself from its top-left corner, in design px (the HUD: Frame::Metrics::designWindow is its size)
    };

    // What a widget learns about its item
    struct ItemState
    {
        bool focused = false;
        bool hovered = false;
        bool activated = false;   // confirmed this frame: Enter / Space / A, or a click
        bool enabled = true;
        float glow = 0.0f;        // 0..1, the focus fading in over 0.08 s (§4.4)
    };

    class Frame
    {
    public:
        // ---- the life of a frame: the Host calls them, a screen does not
        void Begin(const FrameDesc& desc, const RawInput& raw, ImDrawList* drawList, const Fonts* fonts);
        void End();

        // ---- metrics and time
        const Metrics& metrics() const { return metrics_; }
        float S(float designPx) const { return designPx * metrics_.scale; }
        float DeltaTime() const { return deltaTime_; }
        double Time() const { return time_; }
        void SetSpace(Space space);
        Space space() const { return space_; }
        Vec2 ToScreen(Vec2 designPoint) const;          // by the current space
        Rect ToScreen(const Rect& designRect) const;
        Vec2 ToDesign(Vec2 screenPoint) const;          // the mouse
        ImDrawList* DrawList() const { return drawList_; }
        const Fonts& fonts() const { return *fonts_; }

        // ---- input
        const InputMapper& input() const { return input_; }
        InputMapper& input() { return input_; }
        Device LastDevice() const { return input_.LastDevice(); }
        // A widget that uses a direction (or another event) while it is focused takes it: true once, then the event is gone for the others
        bool TakeNav(Nav nav);

        // ---- the focus
        Id Focus() const { return focus_; }
        bool IsFocused(Id id) const { return id != kNoId && id == focus_; }
        void SetFocus(Id id);
        // This item has the focus when the current scope has none yet (the first frame of a screen, after a dialog was closed)
        void SetDefaultFocus(Id id);
        // A new screen: forget the focus, so that the default one is taken (and the transition of the screen starts, §4.4)
        void ResetFocus();
        // Registers an item (every widget does it): hover, focus, confirmation. `designRect` is in the current space.
        ItemState Item(Id id, const Rect& designRect, bool enabled = true);

        // Small state that a widget keeps between frames, by its id (the plate chosen in a row of bindings, the offset of a list, the timer of a pop)
        int& IntState(Id id);
        float& FloatState(Id id);

        // The scopes of the modal dialogs: while one is pushed only its items live. The scope of the screen is 0.
        void PushScope(Id scope);
        void PopScope();
        // Are the items registered right now alive (the scope being laid out is the one that holds the focus)? Widgets that read the keys without an
        // item (the tabs: Q / E) check it so that a dialog above them keeps them quiet.
        bool ScopeLive() const { return (scopes_.empty() ? kNoId : scopes_.back()) == activeScope_; }
        Id Scope() const { return scopes_.empty() ? kNoId : scopes_.back(); }

        // A field of text is being edited (TextRow calls it): the letters W A S D Q E R are text then, the navigation ignores them (from the next frame)
        void SetTextEditing() { textEditingNext_ = true; }

        // ---- the transition of a screen (§1: 0.18 s, the old fades out, the new slides in 40 px and fades in)
        void StartTransition();
        float TransitionAlpha() const;     // 0..1: multiply the alpha of the screen by it
        float TransitionOffset() const;    // design px to add to x of the screen (-40 .. 0)

        // The alpha everything drawn from now on is multiplied by (banners fading in and out, the dimmed scoreboard of the pause): 1 = as is
        void SetAlpha(float alpha) { alpha_ = alpha; }
        float Alpha() const { return alpha_; }

        // ---- toasts (the swap of two bindings: "Boost: Left mouse <-> Jump", yellow, 2 s)
        void ShowToast(std::string_view text, Color color = style::kMvp, float seconds = 2.0f);

        // ---- scrolling lists (see ScrollView in RlUiWidgets.h): the offset of the list that is being laid out
        float ScrollOffset() const { return scrollOffset_; }

    private:
        friend class ScrollView;
        struct ItemRecord
        {
            Id id = kNoId;
            Id scope = kNoId;
            Rect rect;      // screen pixels: the geometry for the move of the focus and for the pointer
            Rect design;    // the rect as it was passed (design px of its space, content coordinates in a list)
            bool enabled = true;
        };
        struct GlowRecord
        {
            Id id = kNoId;
            float value = 0.0f;
        };
        struct Toast
        {
            std::string text;
            Color color;
            float left = 0.0f;
            float total = 0.0f;
        };

        void DrawToasts();
        void MoveFocus(Nav direction);

        Metrics metrics_;
        ImDrawList* drawList_ = nullptr;
        const Fonts* fonts_ = nullptr;
        InputMapper input_;
        float deltaTime_ = 0.0f;
        double time_ = 0.0;
        Space space_ = Space::Menu;

        Id focus_ = kNoId;
        Id defaultFocus_ = kNoId;
        std::vector<Id> scopes_;
        std::vector<ItemRecord> items_;       // registered in this frame
        std::vector<ItemRecord> lastItems_;   // the frame before: the geometry for the move of the focus
        std::vector<GlowRecord> glows_;
        struct StateRecord
        {
            Id id = kNoId;
            int i = 0;
            float f = 0.0f;
        };
        std::vector<StateRecord> states_;
        StateRecord& State(Id id);
        float transition_ = 1.0f;             // 0 -> 1
        std::vector<Toast> toasts_;
        float scrollOffset_ = 0.0f;
        const void* activeScroll_ = nullptr;   // the ScrollView being laid out
        Rect scrollView_;
        Id scrollId_ = kNoId;
        bool textEditingNext_ = false;
        float alpha_ = 1.0f;
        bool focusFromCode_ = false;
        Id activeScope_ = kNoId;               // the scope whose items are alive in this frame (pushed last frame)
        Id nextScope_ = kNoId;                 // the scope pushed in this frame
        Id savedFocus_ = kNoId;                // the focus of the screen below a dialog
        std::vector<std::pair<Id, Id>> defaults_;   // (scope, default focus)
    };
}
