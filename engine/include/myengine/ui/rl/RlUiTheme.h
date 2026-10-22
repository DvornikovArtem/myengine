// RlUiTheme.h
//
// The tokens of the visual language (§4 of the spec): colours, sizes, the bevel. Sizes are design pixels at 1080p; Frame::S(px) turns
// them into screen pixels.

#pragma once

#include <myengine/ui/rl/RlUiCommon.h>

namespace myengine::rlui::style
{
    // ---- colours (§4.1)
    constexpr Color kBlue = Color::Hex(0x1f8fff);
    constexpr Color kBlueHi = Color::Hex(0x6cc0ff);
    constexpr Color kBlueDark = Color::Hex(0x0b3f94);
    constexpr Color kOrange = Color::Hex(0xff8a1f);
    constexpr Color kOrangeHi = Color::Hex(0xffc07a);
    constexpr Color kOrangeDark = Color::Hex(0xa84800);
    constexpr Color kNavy = Color::Hex(0x09142e, 0.86f);      // the background of panels and buttons
    constexpr Color kNavySolid = Color::Hex(0x0a1630);
    constexpr Color kNavy2 = Color::Hex(0x13254d);            // rows
    constexpr Color kNavy3 = Color::Hex(0x1c3466);            // tracks
    constexpr Color kLine = Color::Hex(0x78aaff, 0.28f);      // the thin frame of big panels
    constexpr Color kText = Color::Hex(0xe8f0ff);
    constexpr Color kTextDim = Color::Hex(0x8ea3c8);
    constexpr Color kTextFaint = Color::Hex(0x56688c);
    constexpr Color kFocus = Color::Hex(0xffffff);            // the focus: a white plate ...
    constexpr Color kFocusText = Color::Hex(0x08142c);        // ... with dark text
    constexpr Color kAccent = Color::Hex(0x35d6ff);           // the stripe of the focus, the glow, the cursor
    constexpr Color kBoostA = Color::Hex(0xffd21f);
    constexpr Color kBoostB = Color::Hex(0xff6a00);
    constexpr Color kMvp = Color::Hex(0xffd21f);
    constexpr Color kGood = Color::Hex(0x46e08a);
    constexpr Color kBad = Color::Hex(0xff4d5e);
    constexpr Color kDim = Color::Hex(0x030814, 0.6f);        // the dimming behind a pause / a dialog
    constexpr Color kPadA = Color::Hex(0x46e08a);             // the buttons of the pad in the hints
    constexpr Color kPadB = Color::Hex(0xff4d5e);
    constexpr Color kPadX = Color::Hex(0x4b9bff);
    constexpr Color kPadY = Color::Hex(0xffd21f);

    // the colour of a team (0 blue, 1 orange) in its roles
    constexpr Color TeamColor(const int team) { return team == 0 ? kBlue : kOrange; }
    constexpr Color TeamHi(const int team) { return team == 0 ? kBlueHi : kOrangeHi; }
    constexpr Color TeamDark(const int team) { return team == 0 ? kBlueDark : kOrangeDark; }

    // ---- shape (§4.2)
    constexpr float kBevelDegrees = 12.0f;
    constexpr float kBevelTan = 0.21256f;  // tan(12 deg): the top edge of a parallelogram of height h is shifted right by h * kBevelTan

    // ---- sizes of the elements (design px at 1080p)
    constexpr float kMenuButtonWidth = 520.0f;
    constexpr float kMenuButtonHeight = 72.0f;
    constexpr float kMenuButtonStep = 82.0f;
    constexpr float kRowHeight = 58.0f;
    constexpr float kBindPlateWidth = 230.0f;
    constexpr float kBindPlateHeight = 42.0f;
    constexpr float kModeCardWidth = 250.0f;
    constexpr float kModeCardHeight = 330.0f;
    constexpr float kHudMargin = 22.0f;
    constexpr float kScoreBlockHeight = 84.0f;
    constexpr float kBoostRingSize = 230.0f;

    // ---- motion (§4.4), seconds
    constexpr float kScreenTransition = 0.18f;
    constexpr float kFocusGlowIn = 0.08f;
    constexpr float kCountdownPop = 0.15f;
    constexpr float kGoalIn = 0.25f;
    constexpr float kGoalOut = 0.2f;
    constexpr float kFeedIn = 0.15f;
    constexpr float kFeedFade = 0.3f;
    constexpr float kBoostPulseHz = 6.0f;
    constexpr float kBoostFlash = 0.2f;
}
