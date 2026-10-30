// DoomMenuWidgets.h
//
// The look of the menu of FlyDoom (.team/design/fd-menu-spec.md): "a specimen on the table of a laboratory". Straight corners, thin lines, a red DOOM for the action and a cyan for
// everything of the fly brain; the big words in a narrow poster face (Big Shoulders Display), the data in a monospaced one (JetBrains Mono). The widgets are built on the primitives
// and the focus model of the UI kit (rlui::Frame::Item, paint::*); the kit itself is not changed, except that a game can name its own fonts (rlui::SetFontSet).
//
// Every size is in design px of Space::Menu (the 1080p of the mock-ups).

#pragma once

#include <myengine/ui/rl/RlUiFrame.h>
#include <myengine/ui/rl/RlUiPaint.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace myengine::game::fd
{
    using rlui::Align;
    using rlui::Color;
    using rlui::Frame;
    using rlui::FontStyle;
    using rlui::Id;
    using rlui::Rect;
    using rlui::Vec2;

    // ---- tokens (§1.1)
    namespace tone
    {
        constexpr Color kInk = Color::Hex(0x0B0C0D);
        constexpr Color kSheet = Color::Hex(0x0E0F11, 0.90f);
        constexpr Color kSheetSolid = Color::Hex(0x0E0F11);
        constexpr Color kLine = Color::Hex(0x2A2C30);
        constexpr Color kLine2 = Color::Hex(0x4B4E54);
        constexpr Color kBone = Color::Hex(0xECE6D8);
        constexpr Color kText = Color::Hex(0xD9D3C5);
        constexpr Color kDim = Color::Hex(0x9C978B);
        constexpr Color kFaint = Color::Hex(0x5F5C56);
        constexpr Color kRed = Color::Hex(0xFF3D24);
        constexpr Color kBrain = Color::Hex(0x48D8EC);
        constexpr Color kAmber = Color::Hex(0xFFB224);
        constexpr Color kOnRed = Color::Hex(0x160604);
    }

    // The four slots of the kit under the names of this look (the enum names of the kit are those of Exo 2)
    constexpr FontStyle kDisplay = FontStyle::BlackItalic;       // Big Shoulders Display Black: the headlines
    constexpr FontStyle kItem = FontStyle::ExtraBoldItalic;      // Big Shoulders Display ExtraBold: the items, the buttons
    constexpr FontStyle kMonoBold = FontStyle::Bold;             // JetBrains Mono Bold: values, tags, keys
    constexpr FontStyle kMono = FontStyle::SemiBold;             // JetBrains Mono Regular: text, labels

    // The files and the sizes of the four slots (§1.2); the game hands it to rlui::SetFontSet before the host bakes the fonts
    const rlui::FontSet& FontSet();

    // ---- sound events (§6); the menu plays them, the sounds of the WAD (DoomMenuSounds) or nothing
    enum class Sfx : std::uint8_t
    {
        Focus,
        Select,
        Back,
        Change,
        Deny,
        Start,
        PauseOpen,
        PauseClose,
        Leave,
    };

    // What the widgets share: the sound, the motion setting and the bar of the focus that slides from item to item
    struct Fx
    {
        std::function<void(Sfx)> play;
        bool reduced = false;
        // the bar of the focus (§5.3): the widget that has the focus tells where it is, the bar of the next frame moves there in 120 ms
        struct Bar
        {
            Rect rect{};         // where it is drawn now
            Rect target{};       // where the focused item is
            float alpha = 0.0f;
            float plank = 6.0f;       // the width of the red stripe at the left
            float targetPlank = 6.0f;
            bool wanted = false;      // an item has the focus in this frame
            bool placed = false;
            float gate = 1.0f;        // the alpha of the item that comes in (the entry): the bar comes with it, not before it
            float shift = 0.0f;       // and its shift in x
        } bar;
        void Play(const Sfx sfx) const
        {
            if (play)
            {
                play(sfx);
            }
        }
    };

    // ---- text
    std::string Upper(std::string_view text);
    // Capitals with the letters spaced by `spacing` em
    void Caps(Frame& f, FontStyle style, float size, Vec2 pos, Color color, std::string_view text, Align align = Align::Left, float spacing = 0.12f);
    float CapsWidth(Frame& f, FontStyle style, float size, std::string_view text, float spacing = 0.12f);
    // The text with its capitals' top (`Top`) or the middle of its capitals (`Mid`) at y: the faces have different ascents
    void TextTop(Frame& f, FontStyle style, float size, Vec2 pos, Color color, std::string_view text, Align align = Align::Left, float spacing = 0.0f);
    void TextMid(Frame& f, FontStyle style, float size, Vec2 pos, Color color, std::string_view text, Align align = Align::Left, float spacing = 0.0f);
    void CapsMid(Frame& f, FontStyle style, float size, Vec2 pos, Color color, std::string_view text, Align align = Align::Left, float spacing = 0.12f);
    // A text in the width: the words go to the next line (the lines are returned, the face and the size decide)
    std::vector<std::string> Wrap(Frame& f, FontStyle style, float size, std::string_view text, float width);

    // A run of a line of text: the caps of the hints, the status line of the top
    struct Run
    {
        std::string text;
        bool bold = false;
        Color color = tone::kDim;
        bool dot = false;      // a round dot (the pulse of the brain), `text` is ignored
        bool check = false;    // a check mark
    };
    float RunsWidth(Frame& f, const std::vector<Run>& runs, float size, float spacing = 0.12f);
    void DrawRuns(Frame& f, const std::vector<Run>& runs, float size, Vec2 pos, Align align, float spacing = 0.12f);   // pos.y is the middle of the capitals

    // ---- the page
    // The ground of the menu when no brain is behind it: the ink
    void PaintGround(Frame& f);
    // The grid and the vignette, the marks of the corners of the frame (they grow from the corner while `grow` goes 0 -> 1)
    // `rightLimit` (design px of the window) > 0: the marks of the right corners stand there, not at the edge of the window (the pause over a game with a panel of the brain)
    void PaintFrame(Frame& f, float grow = 1.0f, bool grid = true, float rightLimit = 0.0f);
    // The cap of a key (or of a pad button) with its left edge at pos.x and its middle at pos.y; returns the width. Keys: "UP", "DOWN", "LEFT", "RIGHT" or the text of the cap.
    float KeyCapDraw(Frame& f, Vec2 pos, const std::string& key, float alpha = 1.0f);
    // The top line: crumbs on the left, status segments on the right (the vertical lines between them)
    void TopLeft(Frame& f, const std::vector<Run>& runs);
    void TopRight(Frame& f, const std::vector<std::vector<Run>>& segments, float right = 1824.0f);
    // A text that fits the width: it takes a smaller size when it is too long and ends in "..." when even the smallest is (the lines of the panel in the game, the names of files)
    void DrawFitted(Frame& f, FontStyle style, float size, Vec2 pos, Color color, std::string_view text, float maxWidth, Align align = Align::Left, float spacing = 0.0f);

    // ---- the bar of the focus
    // Draws the bar where it is now and moves it toward the item that has the focus. Call it after the sheets and before the items of the screen.
    void DrawBar(Frame& f, Fx& fx);
    // The focused item says where it is (the widgets do it themselves)
    void BarTarget(Fx& fx, const Rect& rect, float plank);

    // ---- widgets
    struct MenuItemSpec
    {
        std::string index;      // "01"
        std::string word;       // "PLAY"
        std::string sub;        // a line under the word ("LAST: FLY PLAYS · E1M1 · HURT ME PLENTY"); empty: none
        bool enabled = true;
    };
    // A big word of the main screen or of the pause; the item is 76 px high, 104 with a line under it. Returns its state (activated: Enter or a click).
    rlui::ItemState MenuItem(Frame& f, Fx& fx, Id id, float y, const MenuItemSpec& spec, float alpha = 1.0f, float dx = 0.0f);

    struct Tag
    {
        std::string text;
        Color color = tone::kLine2;
    };
    // Tags right-aligned at x (the right edge), the middle of the tag at y; returns the left edge
    float DrawTags(Frame& f, float right, float midY, const std::vector<Tag>& tags);

    struct ModeRowSpec
    {
        std::string index;      // "01"
        std::string name;       // "FLY PLAYS"
        std::string caption;    // "A brain plays"
        std::vector<Tag> tags;
        bool available = true;  // false: SOON (the row can be read, not started)
        std::string soon;       // the tag of an unavailable mode
        bool chosen = false;    // the mode of the dossier when the focus is elsewhere (the red 3 px)
    };
    rlui::ItemState ModeRow(Frame& f, Fx& fx, Id id, const Rect& rect, const ModeRowSpec& spec);

    // The frame of a row of the setup: the label on the left, the line below, the bar of the focus. The widgets below put their control on the right.
    rlui::ItemState SetupRow(Frame& f, Fx& fx, Id id, const Rect& rect, std::string_view label, bool enabled = true, std::string_view tag = {});

    struct Edit
    {
        int delta = 0;            // a stepper: -1 / +1
        bool changed = false;
        bool activated = false;   // Enter on the row (not a click on a control)
        bool focused = false;
        bool hovered = false;
    };
    // ◂ VALUE meta ▸
    Edit Stepper(Frame& f, Fx& fx, Id id, const Rect& rect, std::string_view label, std::string_view value, std::string_view meta = {});
    // AUTO | GPU | CPU : `selected` is the index; the new one when changed. `readOnlyTag`: shown, not changed (an option of N4 that does not work yet).
    Edit Segments(Frame& f, Fx& fx, Id id, const Rect& rect, std::string_view label, const std::vector<std::string>& labels, int& selected, bool readOnly = false, std::string_view tag = {});
    // A slider: t 0..1; `keyStep` gives the new t for a key (-1 / +1); `ticks` marks every 1 / ticks, `midTick` makes the middle one taller
    Edit Slider(Frame& f, Fx& fx, Id id, const Rect& rect, std::string_view label, float& t, const std::function<float(float, int)>& keyStep, std::string_view valueText, int ticks = 10, bool midTick = false);

    enum class ButtonKind : std::uint8_t
    {
        Primary,
        Secondary,
    };
    // START: 380 x 76; `keyHint` ("ENTER", "N") on the right. `enabled` false: no fill, a frame of the line, the text faint (it still takes the focus: Enter says no).
    rlui::ItemState Button(Frame& f, Fx& fx, Id id, const Rect& rect, std::string_view label, ButtonKind kind, bool canDo, std::string_view keyHint, bool arrow);

    // A hint at the bottom: keys and the word ("UP" "DOWN" -> "SELECT"). Keys: "UP", "DOWN", "LEFT", "RIGHT", "ENTER", "ESC", "TAB" ... any other text is the cap itself.
    struct Hint
    {
        std::vector<std::string> keys;
        std::string text;
    };
    void Hints(Frame& f, const std::vector<Hint>& hints, float alpha = 1.0f);

    // An amber square and a line (instead of a red text of an error)
    void Note(Frame& f, Vec2 pos, std::string_view text, float width = 0.0f, Color color = tone::kAmber, float size = 15.0f);

    // "01  SOUND ———" the heading of a part of a sheet
    void Section(Frame& f, float x, float y, float width, std::string_view index, std::string_view name);
    // A sheet: the fill, the frame and the ticks in the corners
    void Sheet(Frame& f, const Rect& rect, float alpha = 1.0f);

    // The signal path of a mode (§3.2): the nodes and the arrows between them
    enum class NodeKind : std::uint8_t
    {
        Red,       // DOOM FRAME, KEYS, YOU, DOOM
        Eye,       // a part of the brain (a frame of the brain colour)
        Brain,     // the brain (filled)
    };
    struct ChainNode
    {
        std::string text;
        std::string meta;       // "LPLC2 LC4": small, after the text
        NodeKind kind = NodeKind::Red;
        bool both = false;      // the arrow after it goes both ways
    };
    // `grow` 0..1: the nodes come in from the left one after the other, the arrows grow
    void Chain(Frame& f, float x, float y, const std::vector<ChainNode>& nodes, float grow = 1.0f, float maxRight = 1e9f);

    // A ring with the ticks and a needle: the test of the mouse
    void Dial(Frame& f, Vec2 center, float radius, float angleDegrees);

    // The dialog of the exit (§3.6): a sheet over a dim screen. Returns 1 for LEAVE, -1 for STAY / Esc / N, 0 while it is open.
    int ConfirmQuit(Frame& f, Fx& fx, std::string_view title, std::string_view line, float appear);
}
