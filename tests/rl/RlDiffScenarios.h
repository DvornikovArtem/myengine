// RlDiffScenarios.h
// The scripted runs that both models drive in the differential test against RocketSim: the generator (tests/rl/rocketsim_diff/gen.cpp,
// built outside of the engine against github.com/ZealanL/RocketSim) and the test (RlRocketSimDiffTests.cpp) include this file, so the two
// sides get the same input tick by tick. Plain C++, no engine headers. Axes of the engine: x right, y up, z forward (the arena is
// 82 x 102 m, the floor is y = 0); lengths in metres, angles in degrees, 120 ticks a second.

#pragma once

#include <cstring>

namespace rldiff
{
    struct Input
    {
        float throttle = 0, steer = 0, pitch = 0, roll = 0;  // steer is the yaw in the air; the sign convention is the one of RlCarInput
        bool jump = false, boost = false, handbrake = false;
    };

    struct Start
    {
        float x = 0, z = -30;       // the middle of the hitbox, over the floor
        float y = 0;                // 0: standing on its wheels at the rest height
        float yawDeg = 0, rollDeg = 0, pitchDeg = 0;
        float speed = 0;            // along the nose (m/s)
    };

    struct Scenario
    {
        const char* name;
        int ticks;
        Start car;
        bool ball = false;          // a ball at rest on the floor at (ballX, ballZ)
        float ballX = 0, ballZ = 0, ballY = 0, ballVz = 0;
        bool second = false;        // a second car, at rest, at (secondX, secondZ), nose along +z
        float secondX = 0, secondZ = 0;
        float boostStart = 33.3f;
    };

    constexpr int kScenarioCount = 45;

    inline Scenario Get(int index)
    {
        Scenario s{};
        s.boostStart = 33.3f;
        const auto ground = [&](const char* name, int ticks, float speed, float z = -30.0f) {
            s.name = name;
            s.ticks = ticks;
            s.car = {};
            s.car.z = z;
            s.car.speed = speed;
        };
        switch (index)
        {
            case 0: ground("accel", 240, 0.0f, -45.0f); break;
            case 1: ground("accel_boost", 300, 0.0f, -47.0f); break;
            case 2: ground("brake", 150, 20.0f, -47.0f); break;
            case 3: ground("coast", 240, 20.0f, -47.0f); break;
            case 4: ground("turn_5", 360, 5.0f); break;
            case 5: ground("turn_10", 360, 10.0f); break;
            case 6: ground("turn_15", 360, 15.0f); break;
            case 7: ground("turn_20", 360, 20.0f); break;
            case 8: ground("turn_23_boost", 300, 22.0f); break;
            case 9: ground("half_steer_15", 360, 15.0f); break;
            case 10: ground("powerslide_14", 300, 14.0f); break;
            case 11: ground("slalom_12", 480, 12.0f, -40.0f); break;
            case 12: ground("reverse", 240, 0.0f, 20.0f); break;
            case 13: ground("brake_from_reverse", 240, 0.0f, 20.0f); break;
            case 14: ground("steer_standing", 240, 0.0f); break;
            case 15: ground("turn_10_coast", 360, 10.0f); break;
            case 16: ground("jump_tap", 300, 0.0f, -20.0f); break;
            case 17: ground("jump_hold", 300, 0.0f, -20.0f); break;
            case 18: ground("jump_double", 300, 0.0f, -20.0f); break;
            case 19: ground("flip_front", 360, 8.0f, -45.0f); break;
            case 20: ground("flip_back", 360, 8.0f, -45.0f); break;
            case 21: ground("flip_right", 360, 8.0f, -45.0f); break;
            case 22: ground("flip_left", 360, 8.0f, -45.0f); break;
            case 23: ground("flip_diag_q", 360, 8.0f, -45.0f); break;
            case 24: ground("flip_front_fast", 240, 20.0f, -48.0f); break;
            case 25: ground("air_roll_e", 200, 0.0f, -20.0f); break;
            case 26: ground("air_pitch_w", 200, 0.0f, -20.0f); break;
            case 27: ground("air_yaw_d", 200, 0.0f, -20.0f); break;
            case 28: ground("boost_air", 150, 0.0f, -20.0f); break;
            case 29:
                ground("landing_roll12", 300, 0.0f);
                s.car.y = 1.2f;
                s.car.rollDeg = 12.0f;
                break;
            case 30:
                ground("landing_pitch14_fast", 300, 8.0f);
                s.car.y = 1.0f;
                s.car.pitchDeg = 14.0f;
                break;
            case 31:
                ground("roof_drop", 300, 0.0f);
                s.car.y = 1.2f;
                s.car.rollDeg = 180.0f;
                break;
            case 32:
                ground("ball_hit", 300, 15.0f, -32.0f);
                s.ball = true;
                s.ballZ = -12.0f;
                break;
            case 33:
                ground("ball_hit_fast", 300, 20.0f, -40.0f);
                s.ball = true;
                s.ballZ = -12.0f;
                break;
            case 34:
                ground("ball_drop", 300, 0.0f, -40.0f);
                s.ball = true;
                s.ballX = 0.0f;
                s.ballZ = 10.0f;
                s.ballY = 6.0f;
                break;
            case 35:
                ground("ball_roll", 300, 0.0f, -40.0f);
                s.ball = true;
                s.ballZ = 10.0f;
                s.ballVz = 8.0f;
                break;
            case 36:
                ground("bump", 240, 20.0f, -30.0f);
                s.second = true;
                s.secondZ = -14.0f;
                break;
            case 37:
                ground("demolish", 240, 22.5f, -30.0f);
                s.second = true;
                s.secondZ = -14.0f;
                break;
            case 38: ground("hop_wave_dash", 360, 12.0f, -45.0f); break;
            case 39: ground("drive_and_jump_steer", 360, 15.0f, -45.0f); break;
            case 40:
                ground("roof_flip", 300, 0.0f);
                s.car.y = 0.2f;
                s.car.rollDeg = 175.0f;
                break;
            case 41:
                ground("side_jump", 300, 0.0f);
                s.car.y = 0.44f;
                s.car.rollDeg = 90.0f;
                break;
            case 42:
                ground("landing_roll_throttle", 300, 0.0f);
                s.car.y = 1.0f;
                s.car.rollDeg = 14.0f;
                break;
            case 43: ground("flip_cancel", 190, 8.0f, -45.0f); break;
            case 44: ground("half_flip", 190, 8.0f, -45.0f); break;
            default: ground("", 0, 0.0f); break;
        }
        return s;
    }

    // The input of tick `t` (0-based) of scenario `index`.
    inline Input At(int index, int t)
    {
        Input in;
        const auto holdDodge = [&](int jumpFrom, int jumpTo, int secondAt, float pitch, float steer, float roll) {
            in.jump = (t >= jumpFrom && t < jumpTo) || (t >= secondAt && t < secondAt + 3);
            if (t >= secondAt && t < secondAt + 3)
            {
                in.pitch = pitch;
                in.steer = steer;
                in.roll = roll;
            }
        };
        switch (index)
        {
            case 0: in.throttle = 1.0f; break;
            case 1: in.throttle = 1.0f; in.boost = true; break;
            case 2: in.throttle = -1.0f; break;
            case 3: break;
            case 4: case 5: case 6: case 7: in.throttle = 1.0f; in.steer = 1.0f; break;
            case 8: in.throttle = 1.0f; in.steer = 1.0f; in.boost = true; break;
            case 9: in.throttle = 1.0f; in.steer = 0.5f; break;
            case 10:
                in.throttle = 1.0f;
                in.steer = t < 120 ? 1.0f : 0.0f;
                in.handbrake = t < 120;
                break;
            case 11:
                in.throttle = 1.0f;
                in.steer = ((t / 60) % 2 == 0) ? 1.0f : -1.0f;
                break;
            case 12: in.throttle = -1.0f; break;
            case 13: in.throttle = t < 100 ? -1.0f : 1.0f; break;
            case 14: in.steer = 1.0f; in.throttle = 0.0f; break;
            case 15: in.steer = 1.0f; break;
            case 16: in.jump = t >= 10 && t < 13; break;
            case 17: in.jump = t >= 10 && t < 40; break;
            case 18: in.jump = (t >= 10 && t < 40) || (t >= 60 && t < 63); break;
            case 19: holdDodge(10, 40, 50, -1.0f, 0.0f, 0.0f); break;
            case 20: holdDodge(10, 40, 50, 1.0f, 0.0f, 0.0f); break;
            case 21: holdDodge(10, 40, 50, 0.0f, 1.0f, 0.0f); break;
            case 22: holdDodge(10, 40, 50, 0.0f, -1.0f, 0.0f); break;
            case 23: holdDodge(10, 40, 50, -1.0f, 0.0f, -1.0f); break;
            case 24: holdDodge(10, 40, 50, -1.0f, 0.0f, 0.0f); break;
            case 25:
                in.jump = t >= 10 && t < 40;
                if (t >= 50 && t < 170) in.roll = 1.0f;
                break;
            case 26:
                in.jump = t >= 10 && t < 40;
                if (t >= 50 && t < 140) in.pitch = -1.0f;
                break;
            case 27:
                in.jump = t >= 10 && t < 40;
                if (t >= 50 && t < 170) in.steer = 1.0f;
                break;
            case 28:
                in.jump = t >= 10 && t < 40;
                if (t >= 50 && t < 170) { in.boost = true; in.pitch = -0.5f; }
                break;
            case 29: case 30: case 31: break;
            case 32: case 33: in.throttle = 1.0f; break;
            case 34: case 35: break;
            case 36: in.throttle = 1.0f; break;
            case 37: in.throttle = 1.0f; in.boost = true; break;
            case 38:
                in.throttle = 1.0f;
                in.jump = (t >= 10 && t < 13) || (t == 30);  // a hop and a dodge at about the touchdown
                if (t == 30) in.pitch = -1.0f;
                break;
            case 39:
                in.throttle = 1.0f;
                in.steer = 0.7f;
                in.jump = t >= 20 && t < 50;
                break;
            case 40: case 41: in.jump = t >= 20 && t < 24; break;
            case 43:
                // a front flip, the stick pulled back at once: the flip is cancelled
                holdDodge(10, 40, 50, -1.0f, 0.0f, 0.0f);
                if (t >= 53 && t < 120) in.pitch = 1.0f;
                break;
            case 44:
                // a back flip, cancelled at about a half turn by the stick the other way
                holdDodge(10, 40, 50, 1.0f, 0.0f, 0.0f);
                if (t >= 53 + 30 && t < 53 + 60) in.pitch = -1.0f;
                if (t >= 53 + 60 && t < 53 + 120)
                {
                    in.roll = 1.0f;
                }
                break;
            case 42: in.throttle = 1.0f; break;
            default: break;
        }
        return in;
    }
}
