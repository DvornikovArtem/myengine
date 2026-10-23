// RlVideoWindowChange.h
//
// The window mode and the resolution of the video settings of the game (Settings > Video, docs/game/rl-ui.md): what the player has CHOSEN and what is APPLIED.
// A step of the resolution or a click on a window mode only changes the chosen (pending) value; the window is not touched. The APPLY button applies both at
// once and opens the question "KEEP THESE SETTINGS?" for 15 s: Keep leaves the new window, Revert (or the end of the time) brings the old one back, and the
// chosen values are the old ones again. Leaving the tab with a choice that was not applied asks "APPLY THE DISPLAY CHANGES?": Apply goes the same way,
// Discard makes the chosen values the applied ones again. It is the logic only (no window, no UI), so that it is tested without either.

#pragma once

namespace myengine::game
{
    // A window mode and a size of it
    struct RlWindowChoice
    {
        int mode = 0;     // 0 Windowed, 1 Borderless, 2 Fullscreen (core::DisplayMode)
        int width = 0;
        int height = 0;
    };

    // The same window: the same mode and, except for Borderless (it takes the desktop), the same size
    bool SameWindow(const RlWindowChoice& a, const RlWindowChoice& b);

    class RlVideoWindowChange
    {
    public:
        static constexpr float kKeepSeconds = 15.0f;

        // The applied window is this one (the settings were loaded or reset): the chosen one is the same, no question is open
        void Set(const RlWindowChoice& applied);

        const RlWindowChoice& Applied() const { return applied_; }
        const RlWindowChoice& Pending() const { return pending_; }

        // The choice of the player: only the pending values change (and nothing while the question of Keep / Revert is open)
        void SetPendingMode(int mode);
        void SetPendingSize(int width, int height);

        // The chosen window is not the applied one
        bool Dirty() const;

        // APPLY: the chosen window becomes the applied one and the question of Keep / Revert opens. false (and nothing happens) when there is nothing to apply
        // or the question is open already. When it is true the caller hands Applied() to the window ONCE.
        bool Apply();

        bool Keeping() const { return keeping_; }
        float Remaining() const { return remaining_; }
        // Keep: the applied window stays
        void Keep();
        // Revert: the applied window is the one before APPLY and so is the chosen one; true when there was a question to answer
        // (the caller hands Applied() to the window ONCE)
        bool Revert();
        // Time passes while the question is open. true when it ended with that (the same as Revert()).
        bool Tick(float seconds);

        // Discard: the chosen window is the applied one again (the window is not touched, it never was)
        void Discard();

    private:
        RlWindowChoice applied_;
        RlWindowChoice pending_;
        RlWindowChoice backup_;
        bool keeping_ = false;
        float remaining_ = 0.0f;
    };
}
