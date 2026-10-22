// RlUiWidgets.h
//
// The widgets of the menus (§1, §2, §4, §5 of the spec). Immediate mode: a widget draws itself and returns what happened this frame. Every
// widget has an Id (HashId("screen/name"), stable from frame to frame), a Rect in DESIGN px (1920 x 1080, current space of the frame) and
// is a focus item: it shows the focus (a white plate with dark text, a stripe of the accent colour, the glow), moves it by hover, reacts to
// Enter / Space / A and a click. `enabled = false` draws it dimmed and takes no focus.
//
// The rows of the settings (OptionRow, ToggleRow, SliderRow, StepperRow, TextRow, ValueRow, BindRow) are 58 px high strips 1000-1100 px wide;
// the ← / → of a focused row change its value (the widget takes the event, the focus does not move sideways), Shift or a held key makes the
// step 5 times bigger, the pointer drags a slider.

#pragma once

#include <string>
#include <string_view>

#include <myengine/ui/rl/RlUiFrame.h>

namespace myengine::rlui
{
    // Lays out items one under another: `Next()` gives the rect of the next one. `step` is the distance between the tops (the menu buttons: 82).
    class Column
    {
    public:
        Column(const Rect& first, float step) : next_(first), step_(step) {}
        Rect Next()
        {
            const Rect r = next_;
            next_.y += step_;
            return r;
        }
        Rect Peek() const { return next_; }
        void Skip(float amount) { next_.y += amount; }

    private:
        Rect next_;
        float step_;
    };

    // ---------------------------------------------------------------- buttons

    // A big button of the main menu / the pause (520 x 72, ExtraBold Italic 38): a bevelled navy plate; focused it is a white plate with
    // dark text, a stripe of the accent colour on the left and a glow. Returns true when confirmed.
    bool MenuButton(Frame& f, Id id, const Rect& rect, std::string_view label, Icon icon = Icon::None, bool enabled = true);

    // A button of an action (START MATCH, JOIN, HOST MATCH, RESET TO DEFAULTS, PLAY AGAIN, MAIN MENU): the same look at the size of the rect. `primary`: the
    // main button of the screen, blue even without the focus.
    bool ActionButton(Frame& f, Id id, const Rect& rect, std::string_view label, Icon icon = Icon::None, bool enabled = true, bool primary = false);

    // The small bevelled button of the dialogs (Confirm / Cancel)
    bool DialogButton(Frame& f, Id id, const Rect& rect, std::string_view label, bool enabled = true);

    // ---------------------------------------------------------------- tabs, cards

    // The tabs (CONTROLS | CAMERA | VIDEO | AUDIO): bevelled, the active one is blue. Q / E and LB / RB switch them from anywhere in the
    // current scope; a click or a confirmed focus selects too. Returns the index of the active tab (the new one if it changed).
    int Tabs(Frame& f, Id id, const Rect& rect, Span<std::string_view> labels, int active);

    struct ModeCardDesc
    {
        std::string_view big;       // "2v2" (Black Italic 96)
        std::string_view name;      // "DOUBLES"
        std::string_view caption;   // "You + 3 bots"
        int playersPerTeam = 1;     // the squares of the teams
    };
    // A card of a mode (250 x 330): the bevelled plate, the big number, the squares of the players in the team colours, the caption.
    // `selected` marks the chosen mode (a blue frame); the focus makes it white. Returns true when confirmed.
    bool ModeCard(Frame& f, Id id, const Rect& rect, const ModeCardDesc& desc, bool selected);

    // ---------------------------------------------------------------- rows of the settings

    // The strip of a row: the background (navy-2, white with a stripe when focused) and the label on the left. Returns its state; the rest of
    // the rows are built on it, a screen can use it to make its own row. `content` is where the control goes (the part right of the label).
    struct RowFrame
    {
        ItemState state;
        Rect content;
    };
    RowFrame Row(Frame& f, Id id, const Rect& rect, std::string_view label, bool enabled = true, float contentLeft = 340.0f);

    // A segmented control (Rookie | Pro | All-star, Windowed | Borderless | Fullscreen, 60 | 120 | 144 ...): returns the selected index (the new one
    // after ← / → or a click on a segment).
    int OptionRow(Frame& f, Id id, const Rect& rect, std::string_view label, Span<std::string_view> options, int selected, bool enabled = true);

    // Off | On (also Toggle | Hold): returns the new value
    bool ToggleRow(Frame& f, Id id, const Rect& rect, std::string_view label, bool value, bool enabled = true, std::string_view offText = "Off", std::string_view onText = "On");

    // A slider with the value on the right (Black Italic); `valueText` is what is shown ("110", "-3.0", "100%"). Returns the new value (clamped,
    // rounded to `step`; the first and the last step are reachable).
    float SliderRow(Frame& f, Id id, const Rect& rect, std::string_view label, float value, float minimum, float maximum, float step,
                    std::string_view valueText, bool enabled = true);

    // "< 2560 x 1440 >  16:9": returns -1 / +1 when the player stepped back / forward (← → or a click on an arrow), else 0.
    int StepperRow(Frame& f, Id id, const Rect& rect, std::string_view label, std::string_view valueText, std::string_view note = {}, bool enabled = true);

    // A line of text that the player types (the address of Join by IP): the focused field shows the accent cursor and takes the typed text,
    // Backspace erases, `allowed` (if not empty) lists the characters that may be typed. Returns true when the text changed.
    bool TextRow(Frame& f, Id id, const Rect& rect, std::string_view label, std::string& text, std::size_t maxLength, std::string_view allowed = {});

    // A value that is only shown (Port 27015, Arena: Standard · night): focusable, no control
    // `emphasis`: the value in Black Italic (a number, Port 27015); otherwise in the quiet text-dim of the notes (Recent 192.168.1.42 · yesterday)
    ItemState ValueRow(Frame& f, Id id, const Rect& rect, std::string_view label, std::string_view value, bool enabled = true, bool emphasis = true);

    // The row of a binding (Controls): the action on the left, two plates (keyboard / mouse, gamepad). ← / → in the focused row choose the plate (the frame
    // of the accent colour); the result tells what the player asked for.
    struct BindRowResult
    {
        bool focused = false;
        int plate = 0;            // 0 keyboard / mouse, 1 gamepad: the chosen one
        bool rebind = false;      // Enter / A / a click on a plate
        bool clear = false;       // Del / X
        bool resetRow = false;    // R / Y
    };
    BindRowResult BindRow(Frame& f, Id id, const Rect& rect, std::string_view action, std::string_view keyboardText, std::string_view padText, bool enabled = true);

    // The captions of the columns of the table of bindings / of the scoreboard (16 px, letter spacing 0.1 em, text-dim)
    void ColumnCaption(Frame& f, Vec2 pos, std::string_view text, Align align = Align::Left);

    // ---------------------------------------------------------------- panels, chips, bars

    // A bevelled panel with the thin frame (the right-hand explanations, Host / Join by IP, the pause info); `stripe` draws the accent stripe on its left edge
    void Panel(Frame& f, const Rect& rect, bool stripe = false, bool solid = false);
    // The title block of a screen: the small line ("SETTINGS", cyan or dim, 24) and the big one (Black Italic 96)
    // `big` may have two lines (a newline in the text); `brand` draws the small line in the accent colour (the name of the engine on the main menu)
    void ScreenTitle(Frame& f, std::string_view small, std::string_view big, Vec2 pos = {80.0f, 60.0f}, bool brand = false);
    // A chip of the corner (OFFLINE, V0.1 · G1): the right edge of the chip is at `right`
    // Returns the width of the chip (to put the next one beside it)
    float Chip(Frame& f, Vec2 rightTop, std::string_view text, Icon icon = Icon::None);
    // A bevelled bar for a progress (loading): `fraction` 0..1, the gradient blue -> accent
    void ProgressBar(Frame& f, const Rect& rect, float fraction);

    // ---------------------------------------------------------------- the hints of the controls (§5.3)

    // What a hint shows next to the text. The keyboard part and the pad part of one hint are shown together as in the mock-ups; the glyphs of
    // the device used last are bright, the others dim.
    enum class Glyph : std::uint8_t
    {
        None,
        UpDown, LeftRight, AllArrows, Enter, Esc, Del, R, Tab, Space, QE,   // the keys
        PadA, PadB, PadX, PadY, PadLBRB, PadBack, PadStart, PadDpad          // the pad
    };
    struct Hint
    {
        Glyph keyboard = Glyph::None;
        Glyph pad = Glyph::None;
        std::string_view text;
    };
    // The row of hints at the bottom left (y 1010, 22 px). Typical sets:
    //   main menu {UpDown/PadA "Select"} {Enter/- "Confirm"} {Esc/PadB "Back"};  settings {Enter/PadA "Rebind"} {Del/PadX "Clear"} {R/PadY "Reset row"} {Esc/PadB "Back"}
    void HintBar(Frame& f, Span<Hint> hints);
    // One key cap with any text ("Space", "Tab", "Esc"): the tip of the loading screen, the ball cam hint. Returns its width (design px).
    float KeyCap(Frame& f, Vec2 pos, std::string_view text, bool dim = false);

    // ---------------------------------------------------------------- dialogs

    enum class DialogResult : std::uint8_t
    {
        None,
        Confirm,
        Cancel,
    };

    struct ConfirmDesc
    {
        std::string_view title;                 // "QUIT TO DESKTOP?"
        std::string_view text;                  // one or two lines
        std::string_view confirmLabel = "Confirm";
        std::string_view cancelLabel = "Cancel";
    };
    // The small bevelled panel in the middle over the dimmed screen (§1). It takes its own scope, the focus is on Cancel (the safe one), Back =
    // Cancel. Call it every frame while the dialog is open; it returns Confirm / Cancel once.
    DialogResult ConfirmDialog(Frame& f, Id id, const ConfirmDesc& desc);

    struct RebindDesc
    {
        std::string_view header;       // "REBIND · KEYBOARD / MOUSE"
        std::string_view action;       // "JUMP"
        std::string_view prompt;       // "Press a key or a mouse button..."
        std::string_view note;         // "Esc - cancel · Del - clear · the key is already used by "
        std::string_view conflict;     // "Boost": shown in yellow after the note (empty: no conflict)
        float remaining = 1.0f;        // the timer bar, 1 -> 0 over 5 s
    };
    // Only draws the window of the rebind (dim, the panel, the bar); catching the key is the screen's job (InputMapper::Raw + CaptureToken).
    void RebindDialog(Frame& f, const RebindDesc& desc);

    // ---------------------------------------------------------------- lists

    // A list taller than its window: `view` is the window, `contentHeight` the height of everything. Lay the items out at y - offset(), draw them
    // between the constructor and the destructor; the focused item scrolls into the view by itself, the wheel scrolls, and the list is clipped.
    class ScrollView
    {
    public:
        ScrollView(Frame& f, Id id, const Rect& view, float contentHeight);
        ~ScrollView();
        ScrollView(const ScrollView&) = delete;
        ScrollView& operator=(const ScrollView&) = delete;
        float Offset() const { return offset_; }

    private:
        Frame& frame_;
        float offset_ = 0.0f;
        bool clipped_ = false;
    };
}
