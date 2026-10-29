// DoomKeys.h
//
// The key codes of DOOM (doomkeys.h of the original): what DoomGame::PostKey takes. Letters and digits are their lower case ASCII.

#pragma once

namespace doom::key
{
    constexpr int kRight = 0xae;
    constexpr int kLeft = 0xac;
    constexpr int kUp = 0xad;
    constexpr int kDown = 0xaf;
    constexpr int kStrafeLeft = 0xa0;     // ',' in the original
    constexpr int kStrafeRight = 0xa1;    // '.'
    constexpr int kUse = 0xa2;
    constexpr int kFire = 0xa3;
    constexpr int kEscape = 27;
    constexpr int kEnter = 13;
    constexpr int kTab = 9;
    constexpr int kBackspace = 0x7f;
    constexpr int kPause = 0xff;
    constexpr int kEquals = 0x3d;
    constexpr int kMinus = 0x2d;
    constexpr int kShift = 0x80 + 0x36;   // run
    constexpr int kCtrl = 0x80 + 0x1d;
    constexpr int kAlt = 0x80 + 0x38;     // strafe while held
    constexpr int kF1 = 0x80 + 0x3b;      // F2..F10 follow
    constexpr int kF11 = 0x80 + 0x57;
    constexpr int kF12 = 0x80 + 0x58;
}
