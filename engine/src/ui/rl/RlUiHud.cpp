#include <myengine/ui/rl/RlUiHud.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <myengine/ui/rl/RlUiPaint.h>
#include <myengine/ui/rl/RlUiWidgets.h>

namespace myengine::rlui
{
    using namespace paint;

    namespace
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kFeedLife = 5.0f;
        constexpr int kFeedMax = 5;

        float EaseOut(const float t)
        {
            const float c = std::clamp(t, 0.0f, 1.0f);
            return 1.0f - (1.0f - c) * (1.0f - c);
        }

        // The pieces of the HUD are laid out in the space of the window
        class ScreenSpace
        {
        public:
            explicit ScreenSpace(Frame& f) : frame_(f), saved_(f.space()) { f.SetSpace(Space::Screen); }
            ~ScreenSpace() { frame_.SetSpace(saved_); }
            ScreenSpace(const ScreenSpace&) = delete;
            ScreenSpace& operator=(const ScreenSpace&) = delete;

        private:
            Frame& frame_;
            Space saved_;
        };

        // The multiplier of the alpha is restored when the piece is done
        class AlphaScope
        {
        public:
            AlphaScope(Frame& f, const float alpha) : frame_(f), saved_(f.Alpha()) { f.SetAlpha(saved_ * alpha); }
            ~AlphaScope() { frame_.SetAlpha(saved_); }
            AlphaScope(const AlphaScope&) = delete;
            AlphaScope& operator=(const AlphaScope&) = delete;

        private:
            Frame& frame_;
            float saved_;
        };

        std::string Upper(std::string text)
        {
            for (char& c : text)
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return text;
        }

        std::string ClockTextOf(const float seconds, const bool overtime)
        {
            const int total = std::max(static_cast<int>(std::ceil(seconds - 1e-3f)), 0);
            const int shown = overtime ? std::max(static_cast<int>(seconds), 0) : total;
            char text[24];
            std::snprintf(text, sizeof(text), "%s%d:%02d", overtime ? "+" : "", shown / 60, shown % 60);
            return text;
        }

        const HudPlayer* PlayerAt(const HudState& state, const int index)
        {
            return index >= 0 && index < static_cast<int>(state.players.size()) ? &state.players[static_cast<std::size_t>(index)] : nullptr;
        }
    }

    void Hud::Reset()
    {
        feed_.clear();
        banner_ = {};
        demolition_ = {};
        ballCamHint_ = 0.0f;
        boostFlash_ = 0.0f;
        pulse_ = 0.0f;
        goAge_ = 1e9f;
        haveState_ = false;
        state_ = {};
        previous_ = {};
    }

    void Hud::Update(const float deltaTime, const HudState& state)
    {
        previous_ = haveState_ ? state_ : state;
        state_ = state;
        const bool first = !haveState_;
        haveState_ = true;
        pulse_ += deltaTime;

        if (!first)
        {
            // a goal: the phase turns to GoalScored
            if (previous_.phase != HudPhase::GoalScored && state_.phase == HudPhase::GoalScored)
            {
                const HudPlayer* scorer = PlayerAt(state_, state_.lastGoalScorer);
                banner_ = {};
                banner_.kind = state_.lastGoalOwn ? Banner::Kind::OwnGoal : Banner::Kind::Goal;
                banner_.title = state_.lastGoalOwn ? "OWN GOAL" : "GOAL!";
                banner_.team = state_.lastGoalTeam;
                std::string subtitle = scorer != nullptr ? Upper(scorer->name) : std::string();
                if (state_.lastGoalSpeedKmh > 0.5f)
                {
                    char speed[32];
                    std::snprintf(speed, sizeof(speed), "%d km/h", static_cast<int>(std::lround(state_.lastGoalSpeedKmh)));
                    subtitle += subtitle.empty() ? std::string(speed) : " \xC2\xB7 " + std::string(speed);
                }
                banner_.subtitle = subtitle;
                banner_.duration = state_.phaseTimer > 0.6f ? state_.phaseTimer : 3.0f;
            }
            if (!previous_.overtime && state_.overtime)
            {
                ShowOvertime();
            }
            if (previous_.phase == HudPhase::Countdown && state_.phase == HudPhase::Playing)
            {
                goAge_ = 0.0f;
            }
            if (state_.boost > previous_.boost + 5.0f)
            {
                boostFlash_ = style::kBoostFlash;
            }
            if (previous_.ballCam != state_.ballCam)
            {
                ballCamHint_ = 2.0f;
            }
        }

        banner_.age += deltaTime;
        if (banner_.kind != Banner::Kind::None && banner_.age > banner_.duration)
        {
            banner_ = {};
        }
        demolition_.age += deltaTime;
        if (demolition_.kind != Banner::Kind::None && demolition_.age > demolition_.duration)
        {
            demolition_ = {};
        }
        for (FeedLine& line : feed_)
        {
            line.age += deltaTime;
        }
        feed_.erase(std::remove_if(feed_.begin(), feed_.end(), [](const FeedLine& line) { return line.age > kFeedLife; }), feed_.end());
        goAge_ += deltaTime;
        boostFlash_ = std::max(boostFlash_ - deltaTime, 0.0f);
        ballCamHint_ = std::max(ballCamHint_ - deltaTime, 0.0f);
        if (state_.phase != HudPhase::Countdown)
        {
            firstMinutesLeft_ = std::max(firstMinutesLeft_ - deltaTime, 0.0f);
        }
    }

    std::string Hud::CountdownText() const
    {
        if (state_.phase == HudPhase::Countdown && state_.phaseTimer > 0.0f)
        {
            const int number = static_cast<int>(std::ceil(state_.phaseTimer - 1e-4f));
            return number >= 1 && number <= 9 ? std::to_string(number) : std::string();
        }
        return goAge_ < 0.5f ? "GO!" : "";
    }

    std::string Hud::ClockText() const { return ClockTextOf(state_.clock, state_.overtime); }

    void Hud::ShowOvertime()
    {
        banner_ = {};
        banner_.kind = Banner::Kind::Overtime;
        banner_.title = "OVERTIME!";
        banner_.duration = 1.5f;
    }

    void Hud::PushEvent(const HudEvent& event)
    {
        const HudPlayer* who = PlayerAt(state_, event.player);
        const HudPlayer* other = PlayerAt(state_, event.other);
        if (who == nullptr)
        {
            return;
        }
        FeedLine line;
        line.event = event;
        line.team[0] = who->team;
        line.text[0] = who->name;
        switch (event.type)
        {
        case HudEventType::Goal: line.text[1] = " scored"; break;
        case HudEventType::OwnGoal: line.text[1] = " scored an own goal"; break;
        case HudEventType::Assist: line.text[1] = " assisted"; break;
        case HudEventType::Save: line.text[1] = " saved"; break;
        case HudEventType::Shot: line.text[1] = " shot"; break;
        case HudEventType::Demo:
            line.text[1] = " demolished ";
            if (other != nullptr)
            {
                line.text[2] = other->name;
                line.team[1] = other->team;
            }
            break;
        }
        feed_.insert(feed_.begin(), line);
        if (static_cast<int>(feed_.size()) > kFeedMax)
        {
            feed_.resize(kFeedMax);
        }

        if (event.type == HudEventType::Demo)
        {
            if (event.other == state_.localPlayer && who != nullptr)
            {
                demolition_ = {};
                demolition_.kind = Banner::Kind::Demolition;
                demolition_.title = "DEMOLISHED BY " + Upper(who->name);
                demolition_.duration = 1.2f;
            }
            else if (event.player == state_.localPlayer)
            {
                demolition_ = {};
                demolition_.kind = Banner::Kind::Demolition;
                demolition_.title = "DEMOLITION!";
                demolition_.duration = 1.2f;
            }
        }
    }

    // ---------------------------------------------------------------- the components

    void Hud::DrawScorebar(Frame& f) const
    {
        ScreenSpace space(f);
        const float centre = f.metrics().designWindow.x * 0.5f;
        const float top = style::kHudMargin;
        const float height = style::kScoreBlockHeight;
        const float b = Bevel(height);
        const float scoreWidth = 120.0f;
        const float timerWidth = 210.0f;
        const bool practice = state_.mode == 0;
        const float total = practice ? timerWidth : scoreWidth * 2.0f + timerWidth;
        float x = centre - (total + b) * 0.5f;

        const auto scoreBlock = [&](const int team, const float left)
        {
            const Rect rect{left, top, scoreWidth + b, height};
            ParallelogramGradient(f, rect, style::TeamColor(team), style::TeamDark(team));
            TextIn(f, FontStyle::BlackItalic, 60.0f, {rect.x + b * 0.5f, rect.y, scoreWidth, rect.h}, style::kText, std::to_string(state_.score[team]), Align::Center);
        };
        if (!practice)
        {
            scoreBlock(0, x);
            x += scoreWidth;
        }
        const Rect timer{x, top, timerWidth + b, height};
        Parallelogram(f, timer, style::kNavySolid);
        // the white strip on top of the timer (the top edge, 4 px)
        Parallelogram(f, {timer.x, timer.y, timer.w, 4.0f}, style::kText, b);
        // the clock: the last 30 s blink white <-> red once a second, the last 10 s pop by 8 % at the start of every second
        const bool overtime = state_.overtime;
        Color clockColor = style::kText;
        float clockScale = 1.0f;
        if (!overtime && state_.clock <= 30.0f && state_.phase != HudPhase::Finished)
        {
            const int whole = static_cast<int>(std::ceil(state_.clock));
            clockColor = (whole % 2 == 0) ? style::kText : style::kBad;
            if (state_.clock <= 10.0f)
            {
                const float since = static_cast<float>(whole) - state_.clock;   // seconds since the second began
                clockScale = 1.0f + 0.08f * std::sin(kPi * std::clamp(since / 0.3f, 0.0f, 1.0f));
            }
        }
        const std::string clock = ClockTextOf(state_.clock, overtime);
        TextIn(f, FontStyle::BlackItalic, 52.0f * clockScale, {timer.x + b * 0.5f, timer.y - (overtime ? 6.0f : 0.0f), timerWidth, timer.h}, clockColor, clock, Align::Center);
        if (overtime)
        {
            Text(f, FontStyle::Bold, 16.0f, {timer.x + b * 0.5f + timerWidth * 0.5f, timer.y + 60.0f}, style::kAccent, "OVERTIME", Align::Center, 0.1f);
        }
        if (!practice)
        {
            scoreBlock(1, x + timerWidth);
        }
    }

    void Hud::DrawCountdown(Frame& f) const
    {
        ScreenSpace space(f);
        const Vec2 window = f.metrics().designWindow;
        const float cx = window.x * 0.5f;
        const float cy = window.y * 0.435f;
        std::string text;
        float age = 0.0f;
        float remaining = 1.0f;
        if (state_.phase == HudPhase::Countdown && state_.phaseTimer > 0.0f)
        {
            const int number = static_cast<int>(std::ceil(state_.phaseTimer - 1e-4f));
            if (number < 1 || number > 9)
            {
                return;
            }
            text = std::to_string(number);
            remaining = state_.phaseTimer - static_cast<float>(number - 1);
            age = 1.0f - remaining;
        }
        else if (goAge_ < 0.5f)
        {
            text = "GO!";
            age = goAge_;
            remaining = 0.5f - goAge_;
        }
        else
        {
            return;
        }
        // 140 % -> 100 % over 0.15 s, fades in the last 0.15 s of its second
        const float scale = 1.0f + 0.4f * (1.0f - EaseOut(age / style::kCountdownPop));
        const float alpha = std::clamp(remaining / 0.15f, 0.0f, 1.0f);
        AlphaScope fade(f, alpha);
        const float size = 220.0f * scale;
        const float lineHeight = TextHeight(f, FontStyle::BlackItalic, size);
        RadialGlow(f, {cx, cy}, 330.0f * scale, 260.0f * scale, style::kAccent.Faded(0.55f));
        TextShadow(f, FontStyle::BlackItalic, size, {cx, cy - lineHeight * 0.52f}, style::kText, text, style::kBlueDark, 8.0f * scale, Align::Center);
    }

    void Hud::DrawBoostRing(Frame& f) const
    {
        ScreenSpace space(f);
        const Vec2 window = f.metrics().designWindow;
        const float diameter = style::kBoostRingSize;
        const Vec2 centre{window.x - 60.0f - diameter * 0.5f, window.y - 50.0f - diameter * 0.5f};
        const float amount = std::clamp(state_.boost, 0.0f, 100.0f) / 100.0f;
        Circle(f, centre, diameter * 0.5f, Color::Hex(0x09142e, 0.78f));
        const float start = 135.0f * kPi / 180.0f;
        const float sweep = 270.0f * kPi / 180.0f;
        Arc(f, centre, 100.0f, 16.0f, start, start + sweep, Color::Hex(0x1c3466));
        if (amount > 0.001f)
        {
            // while the boost burns the ring pulses between 1.0 and 1.25 of its brightness (a share towards white)
            const float pulse = state_.boosting ? 0.5f + 0.5f * std::sin(pulse_ * 2.0f * kPi * style::kBoostPulseHz) : 0.0f;
            const float lift = 0.2f * pulse + (boostFlash_ > 0.0f ? 0.6f * (boostFlash_ / style::kBoostFlash) : 0.0f);
            const auto brighten = [&](const Color c) { return Color{c.r + (1.0f - c.r) * lift, c.g + (1.0f - c.g) * lift, c.b + (1.0f - c.b) * lift, c.a}; };
            ArcGradient(f, centre, 100.0f, 16.0f, start, start + sweep * amount, brighten(style::kBoostB), brighten(style::kBoostA));
        }
        const int shown = static_cast<int>(std::lround(std::clamp(state_.boost, 0.0f, 100.0f)));
        TextIn(f, FontStyle::BlackItalic, 64.0f, {centre.x - 80.0f, centre.y - 52.0f, 160.0f, 80.0f}, shown <= 0 ? style::kBad : style::kText, std::to_string(shown), Align::Center);
        Text(f, FontStyle::Bold, 16.0f, {centre.x, centre.y + 28.0f}, style::kTextDim, "BOOST", Align::Center, 0.2f);
    }

    void Hud::DrawBanners(Frame& f) const
    {
        ScreenSpace space(f);
        const Vec2 window = f.metrics().designWindow;
        if (banner_.kind != Banner::Kind::None)
        {
            const float in = style::kGoalIn;
            const float out = style::kGoalOut;
            float offset = 0.0f;
            float alpha = 1.0f;
            if (banner_.age < in)
            {
                const float t = EaseOut(banner_.age / in);
                offset = (1.0f - t) * window.x * 0.55f;
                alpha = t;
            }
            else if (banner_.age > banner_.duration - out)
            {
                const float t = EaseOut((banner_.age - (banner_.duration - out)) / out);
                offset = -t * window.x * 0.45f;
                alpha = 1.0f - t;
            }
            AlphaScope fade(f, alpha);
            const Rect rect{window.x * 0.5f - 460.0f + offset, window.y * 0.375f - 105.0f, 920.0f, 210.0f};
            const bool overtime = banner_.kind == Banner::Kind::Overtime;
            const int team = std::clamp(banner_.team, 0, 1);
            const Color base = overtime ? style::kNavySolid : style::TeamColor(team);
            Glow(f, rect, (overtime ? style::kAccent : base).Faded(0.32f), 26.0f, 4);
            ParallelogramGradient3(f, rect, base.Faded(0.12f), base, base.Faded(0.12f));
            if (!overtime)
            {
                Circle(f, {rect.x + rect.w - 100.0f, rect.y + 45.0f}, 45.0f, style::TeamHi(team).Faded(0.4f));
            }
            const Color shadow = overtime ? style::kBlueDark : style::TeamDark(team);
            const float titleY = rect.y - 10.0f;
            TextShadow(f, FontStyle::BlackItalic, 150.0f, {rect.x + rect.w * 0.5f, titleY}, style::kText, banner_.title, shadow, 5.0f, Align::Center);
            if (!banner_.subtitle.empty())
            {
                Text(f, FontStyle::ExtraBoldItalic, 34.0f, {rect.x + rect.w * 0.5f, rect.y + rect.h - 48.0f}, style::kText, banner_.subtitle, Align::Center);
            }
        }
        if (demolition_.kind != Banner::Kind::None)
        {
            const float t = demolition_.age;
            const float alpha = std::clamp(std::min(t / 0.1f, (demolition_.duration - t) / 0.2f), 0.0f, 1.0f);
            AlphaScope fade(f, alpha);
            const Rect rect{window.x * 0.5f - 280.0f, window.y - 170.0f, 560.0f, 56.0f};
            Parallelogram(f, rect, style::kNavySolid.Faded(0.92f));
            ParallelogramOutline(f, rect, style::kBad.Faded(0.8f), 1.5f);
            IconGlyph(f, Icon::Bolt, 26.0f, {rect.x + 44.0f, rect.y + rect.h * 0.5f}, style::kBoostA);
            TextIn(f, FontStyle::ExtraBoldItalic, 26.0f, {rect.x + 70.0f, rect.y, rect.w - 70.0f, rect.h}, style::kText, demolition_.title, Align::Left, 8.0f);
            if (state_.demolished && state_.respawnTimer > 0.0f)
            {
                char text[40];
                std::snprintf(text, sizeof(text), "Respawning %.1f", static_cast<double>(state_.respawnTimer));
                Text(f, FontStyle::SemiBold, 22.0f, {window.x * 0.5f, rect.y + rect.h + 8.0f}, style::kTextDim, text, Align::Center);
            }
        }
        else if (state_.demolished && state_.respawnTimer > 0.0f)
        {
            char text[40];
            std::snprintf(text, sizeof(text), "Respawning %.1f", static_cast<double>(state_.respawnTimer));
            Text(f, FontStyle::SemiBold, 26.0f, {window.x * 0.5f, window.y - 150.0f}, style::kTextDim, text, Align::Center);
        }
    }

    void Hud::DrawFeed(Frame& f) const
    {
        ScreenSpace space(f);
        const Vec2 window = f.metrics().designWindow;
        float y = 140.0f;
        const float right = window.x - 40.0f;
        for (const FeedLine& line : feed_)
        {
            const float in = std::clamp(line.age / style::kFeedIn, 0.0f, 1.0f);
            const float out = std::clamp((kFeedLife - line.age) / style::kFeedFade, 0.0f, 1.0f);
            AlphaScope fade(f, std::min(EaseOut(in), out));
            const float offset = (1.0f - EaseOut(in)) * 60.0f;
            // the width: icon, pieces
            float width = 0.0f;
            for (const std::string& piece : line.text)
            {
                width += TextWidth(f, FontStyle::Bold, 22.0f, piece);
            }
            const float plateWidth = width + 66.0f;
            const Rect plate{right - plateWidth + offset, y, plateWidth, 44.0f};
            Parallelogram(f, plate, style::kNavy);
            ParallelogramOutline(f, plate, style::kLine, 1.0f);
            Icon icon = Icon::Ball;
            Color iconColor = style::TeamHi(line.team[0]);
            switch (line.event.type)
            {
            case HudEventType::Goal:
            case HudEventType::OwnGoal:
            case HudEventType::Shot: icon = Icon::Ball; break;
            case HudEventType::Assist: icon = Icon::Ball; break;
            case HudEventType::Save: icon = Icon::Shield; break;
            case HudEventType::Demo: icon = Icon::Bolt; iconColor = style::TeamHi(line.team[0]); break;
            }
            IconGlyph(f, icon, 20.0f, {plate.x + 30.0f, y + 22.0f}, iconColor);
            float x = plate.x + 52.0f;
            for (int i = 0; i < 4; ++i)
            {
                if (line.text[i].empty())
                {
                    continue;
                }
                const bool name = i == 0 || i == 2;
                const Color color = name ? style::TeamHi(i == 0 ? line.team[0] : line.team[1]) : style::kText;
                TextIn(f, FontStyle::Bold, 22.0f, {x, y, 400.0f, 44.0f}, color, line.text[i]);
                x += TextWidth(f, FontStyle::Bold, 22.0f, line.text[i]);
            }
            y += 52.0f;
        }
    }

    void Hud::DrawBallCamHint(Frame& f) const
    {
        const bool visible = ballCamHint_ > 0.0f || firstMinutesLeft_ > 0.0f;
        if (!visible || state_.phase == HudPhase::Finished || state_.phase == HudPhase::GoalScored)
        {
            return;
        }
        ScreenSpace space(f);
        const Vec2 window = f.metrics().designWindow;
        const float alpha = ballCamHint_ > 0.0f ? std::clamp(ballCamHint_ / 0.3f, 0.0f, 1.0f) : 1.0f;
        AlphaScope fade(f, firstMinutesLeft_ > 0.0f ? 1.0f : alpha);
        const std::string key = state_.ballCamKey;
        const float keyWidth = std::max(TextWidth(f, FontStyle::Bold, 20.0f, key) + 24.0f, 38.0f);
        const std::string label = std::string("Ball cam \xC2\xB7 ") + (state_.ballCam ? "ON" : "OFF");
        const float labelWidth = TextWidth(f, FontStyle::SemiBold, 22.0f, label);
        const float total = keyWidth + 12.0f + labelWidth;
        const float x = window.x * 0.5f - total * 0.5f;
        const float y = window.y - 72.0f;
        KeyCap(f, {x, y}, key);
        Text(f, FontStyle::SemiBold, 22.0f, {x + keyWidth + 12.0f, y + 6.0f}, style::kTextDim, label);
    }

    void Hud::DrawPing(Frame& f) const
    {
        ScreenSpace space(f);
        float y = 36.0f;
        if (state_.pingMs >= 0)
        {
            const Color color = state_.pingMs < 60 ? style::kGood : (state_.pingMs < 120 ? style::kMvp : style::kBad);
            IconGlyph(f, Icon::Wifi, 20.0f, {50.0f, y + 12.0f}, color);
            Text(f, FontStyle::Bold, 20.0f, {70.0f, y}, style::kTextDim, std::to_string(state_.pingMs) + " ms");
            y += 36.0f;
        }
        if (state_.showFps)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.0f FPS", static_cast<double>(state_.fps));
            Text(f, FontStyle::Bold, 20.0f, {40.0f, y}, style::kTextDim, text);
        }
    }

    float Hud::DrawScoreTable(Frame& f, const Rect& rect, const HudState& state, const bool pings, const bool teamBars, const bool footer)
    {
        const float colScore = 670.0f;
        const float colGoals = 775.0f;
        const float colAssists = 865.0f;
        const float colSaves = 955.0f;
        const float colShots = 1045.0f;
        const float colDemos = 1135.0f;
        ColumnCaption(f, {rect.x + 80.0f, rect.y}, "PLAYER");
        ColumnCaption(f, {rect.x + colScore, rect.y}, "SCORE", Align::Center);
        ColumnCaption(f, {rect.x + colGoals, rect.y}, "GOALS", Align::Center);
        ColumnCaption(f, {rect.x + colAssists, rect.y}, "ASSISTS", Align::Center);
        ColumnCaption(f, {rect.x + colSaves, rect.y}, "SAVES", Align::Center);
        ColumnCaption(f, {rect.x + colShots, rect.y}, "SHOTS", Align::Center);
        ColumnCaption(f, {rect.x + colDemos, rect.y}, "DEMOS", Align::Center);

        float y = rect.y + 27.0f;
        const int teams = state.mode == 0 ? 1 : 2;
        for (int team = 0; team < teams; ++team)
        {
            std::vector<const HudPlayer*> rows;
            for (std::size_t i = 0; i < state.players.size(); ++i)
            {
                if (state.players[i].team == team)
                {
                    rows.push_back(&state.players[i]);
                }
            }
            if (teamBars)
            {
                std::stable_sort(rows.begin(), rows.end(), [](const HudPlayer* a, const HudPlayer* b) { return a->score > b->score; });
            }
            if (state.mode != 0 && teamBars)
            {
                const Rect bar{rect.x - 5.0f, y, rect.w + 5.0f, 44.0f};
                ParallelogramGradient(f, bar, style::TeamColor(team), style::TeamColor(team).Faded(0.0f));
                const std::string label = std::string(team == 0 ? "BLUE" : "ORANGE") + " \xC2\xB7 " + std::to_string(state.score[team]);
                TextIn(f, FontStyle::BlackItalic, 26.0f, {bar.x + 28.0f, bar.y, 400.0f, bar.h}, style::kText, label);
                y += 50.0f;
            }
            for (const HudPlayer* player : rows)
            {
                const bool local = PlayerAt(state, state.localPlayer) == player;
                const Rect row{rect.x, y, rect.w, 56.0f};
                FillRect(f, row, local ? Color::Hex(0x2c3a5c, 0.92f) : style::kNavySolid.Faded(0.9f));
                FillRect(f, {row.x, row.y, 6.0f, row.h}, style::TeamColor(team));
                IconGlyph(f, player->bot ? Icon::Robot : Icon::Person, 20.0f, {row.x + 30.0f, row.y + row.h * 0.5f}, style::kTextDim);
                TextIn(f, FontStyle::SemiBold, 26.0f, {row.x + 80.0f, row.y, 540.0f, row.h}, style::kText, player->name);
                TextIn(f, FontStyle::BlackItalic, 26.0f, {row.x + colScore - 60.0f, row.y, 120.0f, row.h}, style::kText, std::to_string(player->score), Align::Center);
                const int values[5] = {player->goals, player->assists, player->saves, player->shots, player->demos};
                const float columns[5] = {colGoals, colAssists, colSaves, colShots, colDemos};
                for (int c = 0; c < 5; ++c)
                {
                    TextIn(f, FontStyle::SemiBold, 26.0f, {row.x + columns[c] - 50.0f, row.y, 100.0f, row.h}, style::kText, std::to_string(values[c]), Align::Center);
                }
                y += 59.0f;
            }
            y += teamBars ? 14.0f : 0.0f;
        }
        if (!footer)
        {
            return y;
        }
        y += 4.0f;
        float x = rect.x;
        if (pings)
        {
            std::string text = "Ping:";
            for (const HudPlayer& player : state.players)
            {
                if (player.pingMs >= 0)
                {
                    // the host of a private match has no ping to itself
                    text += " " + player.name + " " + (player.pingMs == 0 ? std::string("host") : std::to_string(player.pingMs));
                }
            }
            IconGlyph(f, Icon::Wifi, 18.0f, {x + 9.0f, y + 14.0f}, style::kTextDim);
            Text(f, FontStyle::SemiBold, 20.0f, {x + 24.0f, y + 4.0f}, style::kTextDim, text);
            x += 24.0f + TextWidth(f, FontStyle::SemiBold, 20.0f, text) + 32.0f;
        }
        Text(f, FontStyle::SemiBold, 20.0f, {x, y + 4.0f}, style::kTextDim, "Hold");
        x += TextWidth(f, FontStyle::SemiBold, 20.0f, "Hold") + 8.0f;
        x += KeyCap(f, {x, y}, "Tab") + 12.0f;
        Text(f, FontStyle::SemiBold, 20.0f, {x, y + 4.0f}, style::kTextDim, "/");
        x += 20.0f;
        KeyCap(f, {x, y}, "Back");
        return y + 40.0f;
    }

    void Hud::Draw(Frame& f, const bool scoreboardHeld, const bool paused)
    {
        ScreenSpace space(f);
        if (paused)
        {
            AlphaScope dim(f, 0.45f);
            DrawScorebar(f);
            return;
        }
        if (scoreboardHeld)
        {
            // the match goes dim under the table: the scoreboard, the ping and the table only (§3.6)
            Dim(f, Color::Hex(0x030814, 0.45f));
            DrawScorebar(f);
            DrawPing(f);
            const float width = 1200.0f;
            DrawScoreTable(f, {f.metrics().designWindow.x * 0.5f - width * 0.5f, 150.0f, width, 0.0f}, state_, state_.pingMs >= 0);
            return;
        }
        DrawScorebar(f);
        DrawPing(f);
        DrawCountdown(f);
        if (state_.localPlayer >= 0)
        {
            DrawBoostRing(f);
        }
        DrawBanners(f);
        DrawFeed(f);
        DrawBallCamHint(f);
    }
}
