#include "PrankGenerator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

#include <myengine/assistant/AssistantBridge.h>
#include <myengine/assistant/AssistantService.h>
#include <myengine/ui/AssistantPanel.h>

#include "PrankStrings.h"
#include "PrankUi.h"

namespace myengine::ui::prank
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        constexpr float kHeaderH = 52.0f;
        constexpr float kListW = 240.0f;
        constexpr float kComposerH = 118.0f;
        constexpr float kPreviewW = 448.0f;
        constexpr float kPreviewH = 252.0f;

        const ImU32 kBgTranscript = Rgb(0x140910);
        const ImU32 kBgSide = Rgb(0x1D0C17);
        const ImU32 kLine = Rgb(0x3D1733);
        const ImU32 kDimText = Rgb(0xB48CA2);
        const ImU32 kFineText = Rgb(0x7A5A6C);

        std::uint32_t NowSeed()
        {
            return static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xFFFFFFFFu);
        }

        std::string StripToolName(const std::string& name)
        {
            const std::string prefix = "mcp__myengine__";
            return name.rfind(prefix, 0) == 0 ? name.substr(prefix.size()) : name;
        }

        std::string OneLine(std::string text, const std::size_t limit)
        {
            for (char& c : text)
            {
                if (c == '\n' || c == '\r' || c == '\t')
                {
                    c = ' ';
                }
            }
            if (text.size() > limit)
            {
                std::size_t cut = limit;
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                text = text.substr(0, cut) + "...";
            }
            return text;
        }

        // ---- the previews: a little drawing of the game, made with the draw list (the censor plate hides it until the user looks) ----
        void DrawPreview(ImDrawList* list, const ImVec2 a, const ImVec2 b, const MiniScene scene, const double time)
        {
            const float w = b.x - a.x;
            const float h = b.y - a.y;
            const float t = static_cast<float>(time);
            list->PushClipRect(a, b, true);
            switch (scene)
            {
                case MiniScene::Octopus:
                {
                    GradientV(list, a, b, Rgb(0x1B2A5A), Rgb(0x0B1030));
                    const ImVec2 c(a.x + w * 0.5f, a.y + h * 0.38f);
                    for (int i = 0; i < 8; ++i)
                    {
                        const float angle = (static_cast<float>(i) / 8.0f) * 2.0f * kPi + 0.4f;
                        const float sway = std::sin(t * 2.0f + static_cast<float>(i)) * 14.0f;
                        const ImVec2 p0(c.x + std::cos(angle) * 36.0f, c.y + 26.0f);
                        const ImVec2 p3(c.x + std::cos(angle) * 160.0f + sway, a.y + h * 0.88f + std::sin(angle) * 22.0f);
                        list->AddBezierCubic(p0, ImVec2(p0.x + std::cos(angle) * 70.0f, p0.y + 60.0f), ImVec2(p3.x - sway, p3.y - 60.0f), p3, Rgb(0xB05CE8), 9.0f);
                    }
                    list->AddCircleFilled(c, 58.0f, Rgb(0xC77DFF), 40);
                    list->AddCircleFilled(ImVec2(c.x - 20.0f, c.y - 6.0f), 11.0f, kWhite, 20);
                    list->AddCircleFilled(ImVec2(c.x + 20.0f, c.y - 6.0f), 11.0f, kWhite, 20);
                    list->AddCircleFilled(ImVec2(c.x - 18.0f, c.y - 4.0f), 5.0f, Rgb(0x1B1B1B), 12);
                    list->AddCircleFilled(ImVec2(c.x + 22.0f, c.y - 4.0f), 5.0f, Rgb(0x1B1B1B), 12);
                    list->AddRectFilled(ImVec2(c.x - 14.0f + std::sin(t * 1.6f) * 60.0f, a.y + h - 40.0f), ImVec2(c.x + 14.0f + std::sin(t * 1.6f) * 60.0f, a.y + h - 12.0f), Rgb(0xE2A12E), 3.0f);
                    break;
                }
                case MiniScene::Fox:
                {
                    GradientV(list, a, b, Rgb(0x9ED8F5), Rgb(0xDDF2FF));
                    list->AddRectFilled(ImVec2(a.x, a.y + h * 0.7f), b, Rgb(0x5DBB63));
                    const ImVec2 c(a.x + w * 0.45f + std::sin(t * 1.3f) * 40.0f, a.y + h * 0.62f);
                    list->AddCircleFilled(ImVec2(c.x + 36.0f, c.y + 18.0f), 22.0f, Rgb(0xE8741E), 24);
                    list->AddCircleFilled(c, 38.0f, Rgb(0xF08A2B), 32);
                    list->AddTriangleFilled(ImVec2(c.x - 30.0f, c.y - 18.0f), ImVec2(c.x - 12.0f, c.y - 62.0f), ImVec2(c.x - 2.0f, c.y - 30.0f), Rgb(0xF08A2B));
                    list->AddTriangleFilled(ImVec2(c.x + 30.0f, c.y - 18.0f), ImVec2(c.x + 12.0f, c.y - 62.0f), ImVec2(c.x + 2.0f, c.y - 30.0f), Rgb(0xF08A2B));
                    list->AddCircleFilled(ImVec2(c.x - 12.0f, c.y - 4.0f), 4.0f, Rgb(0x1B1B1B), 12);
                    list->AddCircleFilled(ImVec2(c.x + 12.0f, c.y - 4.0f), 4.0f, Rgb(0x1B1B1B), 12);
                    list->AddCircleFilled(ImVec2(c.x, c.y + 8.0f), 5.0f, Rgb(0x1B1B1B), 12);
                    for (int i = 0; i < 6; ++i)
                    {
                        const ImVec2 coin(a.x + 40.0f + static_cast<float>(i) * 68.0f, a.y + h * 0.82f + std::sin(t * 3.0f + static_cast<float>(i)) * 5.0f);
                        list->AddCircleFilled(coin, 10.0f, kGold, 20);
                        list->AddCircle(coin, 10.0f, Rgb(0xC9961A), 20, 2.0f);
                    }
                    break;
                }
                case MiniScene::Lava:
                {
                    GradientV(list, a, b, Rgb(0x3A0A0A), Rgb(0x7A1A06));
                    list->AddRectFilledMultiColor(ImVec2(a.x, a.y + h * 0.72f), b, Rgb(0xFF8A1E), Rgb(0xFF8A1E), Rgb(0xFF3A1E), Rgb(0xFF3A1E));
                    for (int i = 0; i < 6; ++i)
                    {
                        const float y = a.y + std::fmod(t * 90.0f + static_cast<float>(i) * 61.0f, h * 0.7f);
                        const ImVec2 c(a.x + 40.0f + static_cast<float>(i) * 72.0f, y);
                        Glow(list, ImVec2(c.x - 8.0f, c.y - 8.0f), ImVec2(c.x + 8.0f, c.y + 8.0f), Rgb(0xFFB030, 160), 12.0f, 3, 10.0f);
                        list->AddCircleFilled(c, 11.0f, Rgb(0xFFD23A), 20);
                    }
                    list->AddRectFilled(ImVec2(a.x + w * 0.5f - 14.0f, a.y + h * 0.58f), ImVec2(a.x + w * 0.5f + 14.0f, a.y + h * 0.72f), Rgb(0xE2A12E), 3.0f);
                    break;
                }
                case MiniScene::Hardcore:
                {
                    // an arena from above: the player in the middle, the red crates come for it, the coins lie around (the same kind of picture as the other games)
                    GradientV(list, a, b, Rgb(0x2B3A55), Rgb(0x16202F));
                    for (int i = 1; i < 8; ++i)
                    {
                        list->AddLine(ImVec2(a.x + w * static_cast<float>(i) / 8.0f, a.y), ImVec2(a.x + w * static_cast<float>(i) / 8.0f, b.y), Rgb(0x3A4A68), 1.0f);
                    }
                    for (int i = 1; i < 5; ++i)
                    {
                        list->AddLine(ImVec2(a.x, a.y + h * static_cast<float>(i) / 5.0f), ImVec2(b.x, a.y + h * static_cast<float>(i) / 5.0f), Rgb(0x3A4A68), 1.0f);
                    }
                    const ImVec2 c(a.x + w * 0.5f + std::sin(t * 1.5f) * 22.0f, a.y + h * 0.55f);
                    for (int i = 0; i < 5; ++i)
                    {
                        const ImVec2 coin(a.x + 60.0f + static_cast<float>(i) * 82.0f, a.y + 50.0f + static_cast<float>((i * 37) % 70));
                        list->AddCircleFilled(coin, 9.0f, kGold, 20);
                        list->AddCircle(coin, 9.0f, Rgb(0xC9961A), 20, 2.0f);
                    }
                    for (int i = 0; i < 4; ++i)
                    {
                        const float angle = static_cast<float>(i) * 1.5708f + 0.6f + t * 0.35f;
                        const float distance = 120.0f + std::sin(t * 2.0f + static_cast<float>(i)) * 14.0f;
                        const ImVec2 chaser(c.x + std::cos(angle) * distance * 1.5f, c.y + std::sin(angle) * distance * 0.7f);
                        list->AddRectFilled(ImVec2(chaser.x - 13.0f, chaser.y - 13.0f), ImVec2(chaser.x + 13.0f, chaser.y + 13.0f), Rgb(0xE0553A), 3.0f);
                        list->AddRect(ImVec2(chaser.x - 13.0f, chaser.y - 13.0f), ImVec2(chaser.x + 13.0f, chaser.y + 13.0f), Rgb(0x8F2C1A), 3.0f, 0, 2.0f);
                    }
                    list->AddRectFilled(ImVec2(c.x - 15.0f, c.y - 15.0f), ImVec2(c.x + 15.0f, c.y + 15.0f), Rgb(0x6FB3FF), 3.0f);
                    list->AddRect(ImVec2(c.x - 15.0f, c.y - 15.0f), ImVec2(c.x + 15.0f, c.y + 15.0f), Rgb(0x2E6FC4), 3.0f, 0, 2.0f);
                    Text(list, ImVec2(a.x + 12.0f, a.y + 10.0f), 16.0f, Rgb(0xE8E8E8), "1 / 1");
                    break;
                }
                case MiniScene::NakedCars:
                default:
                {
                    GradientV(list, a, b, Rgb(0x3E9E4E), Rgb(0x2C7A3A));
                    list->AddRect(ImVec2(a.x + 14.0f, a.y + 14.0f), ImVec2(b.x - 14.0f, b.y - 14.0f), Rgb(0xE8F5E8), 0.0f, 0, 2.0f);
                    list->AddLine(ImVec2(a.x + w * 0.5f, a.y + 14.0f), ImVec2(a.x + w * 0.5f, b.y - 14.0f), Rgb(0xE8F5E8), 2.0f);
                    list->AddCircle(ImVec2(a.x + w * 0.5f, a.y + h * 0.5f), 36.0f, Rgb(0xE8F5E8), 28, 2.0f);
                    for (int i = 0; i < 4; ++i)
                    {
                        const float x = a.x + 70.0f + static_cast<float>(i % 2) * 70.0f + static_cast<float>(i / 2) * 170.0f;
                        const float y = a.y + h * 0.4f + std::sin(t * 2.0f + static_cast<float>(i)) * 14.0f + static_cast<float>(i % 2) * 60.0f;
                        list->AddCircleFilled(ImVec2(x, y), 17.0f, Rgb(0x1B1B1B), 24);
                        list->AddCircleFilled(ImVec2(x, y), 8.0f, Rgb(0xC9CED4), 16);
                    }
                    list->AddCircleFilled(ImVec2(a.x + w * 0.5f + std::sin(t * 1.7f) * 30.0f, a.y + h * 0.5f), 12.0f, kWhite, 20);
                    break;
                }
            }
            list->PopClipRect();
        }

        // The plate over the picture: "adults only". `melt` 0..1 (1: gone)
        void DrawPlate(ImDrawList* list, const ImVec2 a, const ImVec2 b, const float melt)
        {
            if (melt >= 1.0f)
            {
                return;
            }
            const float alpha = 1.0f - melt;
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const int start = list->VtxBuffer.Size;
            list->AddRectFilled(a, b, WithAlpha(kHot, 0.86f), 8.0f);
            Sign18(list, ImVec2(c.x, c.y - 38.0f), 76.0f, kWhite, 0, kWhite, 6.0f, true);
            TextCentered(list, ImVec2(c.x, c.y + 28.0f), 18.0f, kWhite, str::kGenCensorTitle);
            TextCentered(list, ImVec2(c.x, c.y + 54.0f), 13.0f, WithAlpha(kWhite, 0.85f), str::kGenCensorHint);
            ScaleRotate(list, start, c, 1.0f + 0.04f * melt, 0.0f);
            FadeVertices(list, start, alpha);
        }
    }

    PrankGenerator::PrankGenerator(PrankSceneServices services) : services_(std::move(services)), inputBuffer_(1024, '\0')
    {
        seed_ = NowSeed();
    }

    PrankGenerator::~PrankGenerator() = default;

    void PrankGenerator::Shutdown()
    {
        if (panel_ != nullptr)
        {
            panel_->Shutdown();
            panel_.reset();
        }
    }

    void PrankGenerator::EnsurePanel()
    {
        if (panelTried_)
        {
            return;
        }
        panelTried_ = true;
        if (services_.makePanel)
        {
            panel_ = services_.makePanel();
            if (panel_ != nullptr && !panel_->IsEnabled())
            {
                // The assistant is off: no process, no pipe. The prepared scenes answer
                panel_->Shutdown();
                panel_.reset();
            }
        }
    }

    bool PrankGenerator::Busy() const
    {
        return !turns_.empty() && turns_.back().state == State::Working;
    }

    bool PrankGenerator::LastTurnReady() const
    {
        return !turns_.empty() && turns_.back().state == State::Ready;
    }

    bool PrankGenerator::LastTurnApplied() const
    {
        return !turns_.empty() && turns_.back().state == State::Applied;
    }

    MiniScene PrankGenerator::LastScene() const
    {
        return turns_.empty() ? MiniScene::Octopus : turns_.back().match.scene;
    }

    void PrankGenerator::WatchLast()
    {
        if (!turns_.empty())
        {
            Watch(turns_.back(), true);
        }
    }

    void PrankGenerator::Toast(const std::string& text)
    {
        toast_ = text;
        toastAge_ = 0.0f;
    }

    void PrankGenerator::NewChat()
    {
        if (panel_ != nullptr)
        {
            panel_->GetService().NewConversation();
        }
        turns_.clear();
        askSave_ = -1;
    }

    void PrankGenerator::Submit(const std::string& request)
    {
        if (Busy())
        {
            return;
        }
        std::string text = request;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        {
            text.pop_back();
        }
        std::size_t first = 0;
        while (first < text.size() && (text[first] == '\n' || text[first] == ' '))
        {
            ++first;
        }
        text = text.substr(first);
        if (text.empty())
        {
            return;
        }
        BeginTurn(text);
    }

    void PrankGenerator::BeginTurn(const std::string& request)
    {
        seed_ = seed_ * 1664525u + 1013904223u + NowSeed();
        Turn turn;
        turn.user = request;
        turn.match = MatchRequest(request, seed_);
        turn.whisper = static_cast<int>((seed_ >> 8) % str::kGenWhispers.size());
        EnsurePanel();
        if (panel_ != nullptr && panel_->IsEnabled())
        {
            assistant::AssistantService& service = panel_->GetService();
            turn.baseIndex = service.GetMessages().size();
            if (service.Send(request))
            {
                turn.assistant = true;
                usedAssistant_ = true;
                turns_.push_back(std::move(turn));
                scrollFrames_ = 4;
                return;
            }
        }
        usedAssistant_ = false;
        StartFallback(turn);
        turns_.push_back(std::move(turn));
        scrollFrames_ = 4;
    }

    void PrankGenerator::StartFallback(Turn& turn)
    {
        turn.assistant = false;
        turn.state = State::Working;
        turn.age = 0.0f;
        turn.stateAge = 0.0f;
        turn.percentIndex = 0;
        turn.steps.clear();
        turn.answer.clear();
    }

    void PrankGenerator::Update(const float dt)
    {
        if (panel_ != nullptr)
        {
            panel_->Update();
        }
        for (Turn& turn : turns_)
        {
            UpdateTurn(turn, dt);
        }
        toastAge_ += dt;
    }

    void PrankGenerator::UpdateTurn(Turn& turn, const float dt)
    {
        turn.age += dt;
        turn.stateAge += dt;
        if (turn.state == State::Working || turn.state == State::Ready)
        {
            if (turn.assistant)
            {
                UpdateAssistantTurn(turn, dt);
            }
            else
            {
                UpdateFallbackTurn(turn);
            }
        }
        else if (turn.assistant && turn.state == State::Applied && panel_ != nullptr)
        {
            UpdateAssistantTurn(turn, dt);
        }
        if (turn.state == State::Applied)
        {
            turn.reveal = std::min(1.0f, turn.reveal + dt / 0.3f);
        }
    }

    void PrankGenerator::UpdateFallbackTurn(Turn& turn)
    {
        if (turn.state != State::Working)
        {
            return;
        }
        // The percentages go by a timer: the same row as with the assistant (3 12 37 37 37 69 88 99), 3 s in all
        const float total = 3.0f;
        const int count = static_cast<int>(str::kGenPercents.size());
        turn.percentIndex = std::min(count - 1, static_cast<int>(turn.age / total * static_cast<float>(count)));
        const int stepCount = StepCount(turn.match.scene);
        turn.steps.clear();
        const int shown = std::min(stepCount, 1 + turn.percentIndex * stepCount / count);
        for (int i = 0; i < shown; ++i)
        {
            turn.steps.emplace_back(StepText(turn.match.scene, i));
        }
        if (turn.age >= total)
        {
            turn.answer = AnswerText(turn.match, seed_);
            turn.percentIndex = count - 1;
            turn.state = State::Ready;
            turn.stateAge = 0.0f;
            scrollFrames_ = 4;
        }
    }

    void PrankGenerator::UpdateAssistantTurn(Turn& turn, const float dt)
    {
        (void)dt;
        if (panel_ == nullptr)
        {
            return;
        }
        assistant::AssistantService& service = panel_->GetService();
        assistant::AssistantBridge* bridge = panel_->GetBridge();
        const auto& messages = service.GetMessages();

        std::size_t tools = 0;
        std::string text;
        bool error = false;
        std::vector<std::string> steps;
        for (std::size_t i = turn.baseIndex; i < messages.size(); ++i)
        {
            const assistant::AssistantMessage& m = messages[i];
            if (m.kind == assistant::AssistantMessage::Kind::Tool)
            {
                ++tools;
                std::string line = StripToolName(m.toolName);
                if (!m.text.empty())
                {
                    line += "(" + OneLine(m.text, 56) + ")";
                }
                steps.push_back(line);
            }
            else if (m.kind == assistant::AssistantMessage::Kind::Assistant)
            {
                if (!text.empty())
                {
                    text += "\n\n";
                }
                text += m.text;
            }
            else if (m.kind == assistant::AssistantMessage::Kind::Error)
            {
                error = true;
            }
        }
        turn.steps = steps;
        turn.toolCalls = tools;
        turn.percentIndex = std::min(static_cast<int>(str::kGenPercents.size()) - 1, static_cast<int>(tools));
        if (!text.empty())
        {
            turn.answer = text;
        }

        // A card waits for the user: the picture is under the plate, the user decides. The assistant has not said a word yet (its call waits for the card):
        // the prepared line stands in
        if (turn.answer.empty() && bridge != nullptr && !bridge->GetPending().empty())
        {
            turn.answer = AnswerText(turn.match, seed_);
        }
        if (bridge != nullptr && !bridge->GetPending().empty())
        {
            if (turn.autoApprove)
            {
                const auto pending = bridge->GetPending();
                for (const auto& approval : pending)
                {
                    bridge->Resolve(approval.id, true);
                }
            }
            else if (turn.state == State::Working)
            {
                turn.state = State::Ready;
                turn.stateAge = 0.0f;
                scrollFrames_ = 4;
            }
        }

        if (!service.IsBusy() && turn.state != State::Applied)
        {
            if (error && turn.answer.empty() && tools == 0)
            {
                // The assistant did not answer (no network, the CLI failed): the prepared scenes take over, nobody notices
                usedAssistant_ = false;
                StartFallback(turn);
                return;
            }
            if (tools == 0)
            {
                if (turn.answer.empty())
                {
                    turn.answer = AnswerText(turn.match, seed_);
                }
                turn.state = State::Done;
            }
            else if (turn.autoApprove)
            {
                turn.state = State::Applied;
                turn.reveal = 0.0f;
            }
            else if (turn.state != State::Rejected)
            {
                // The turn ended before the card was answered: the changes were not made
                turn.state = State::Ready;
            }
            turn.stateAge = 0.0f;
            scrollFrames_ = 4;
        }
        else if (!service.IsBusy() && turn.state == State::Applied)
        {
            // already shown as applied
        }
    }

    void PrankGenerator::Watch(Turn& turn, const bool force)
    {
        if (turn.state != State::Ready && turn.state != State::Rejected)
        {
            return;
        }
        if (!force && services_.sceneDirty && services_.sceneDirty())
        {
            askSave_ = static_cast<int>(&turn - turns_.data());
            return;
        }
        askSave_ = -1;
        if (turn.assistant)
        {
            turn.autoApprove = true;
            if (panel_ != nullptr && panel_->GetBridge() != nullptr)
            {
                const auto pending = panel_->GetBridge()->GetPending();
                for (const auto& approval : pending)
                {
                    panel_->GetBridge()->Resolve(approval.id, true);
                }
            }
            if (!panel_->GetService().IsBusy())
            {
                // The turn is over already (the cards were void): ask once more, quietly, for the same thing
                turn.state = State::Working;
                turn.baseIndex = panel_->GetService().GetMessages().size();
                if (!panel_->GetService().Send(turn.user))
                {
                    usedAssistant_ = false;
                    StartFallback(turn);
                }
                return;
            }
            turn.state = State::Applied;
            turn.reveal = 0.0f;
            turn.stateAge = 0.0f;
            return;
        }
        if (ApplyScene(turn))
        {
            turn.state = State::Applied;
            turn.reveal = 0.0f;
            turn.stateAge = 0.0f;
        }
        else
        {
            turn.state = State::Error;
            turn.answer = str::kGenSceneMissing;
        }
    }

    bool PrankGenerator::ApplyScene(Turn& turn)
    {
        const std::filesystem::path file = services_.root / SceneFile(turn.match.scene);
        std::ifstream stream(file, std::ios::binary);
        if (!stream || !services_.capture || !services_.restore)
        {
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (services_.isPlaying && services_.isPlaying() && services_.stopPlay)
        {
            services_.stopPlay();
        }
        const std::string before = services_.capture();
        if (!services_.restore(text))
        {
            return false;
        }
        if (services_.recordUndo)
        {
            services_.recordUndo("Generate", before);
        }
        return true;
    }

    void PrankGenerator::Reject(Turn& turn)
    {
        if (turn.assistant && panel_ != nullptr)
        {
            if (panel_->GetBridge() != nullptr)
            {
                panel_->GetBridge()->RejectAll("The user said not now.");
            }
            panel_->GetService().Cancel();
        }
        turn.state = State::Rejected;
        turn.stateAge = 0.0f;
        turn.note = str::kGenNotNowReply;
    }

    // ------------------------------------------------------------------------------------------------------------------------

    bool PrankGenerator::Draw(ImDrawList* list, const ImVec2 min, const ImVec2 max, const double time, const float dt)
    {
        now_ = time;
        (void)dt;
        const float width = max.x - min.x;
        list->PushClipRect(min, max, true);
        list->AddRectFilled(min, max, kBgTranscript);

        bool back = false;
        DrawHeader(list, min, width, back);
        const float bodyTop = min.y + kHeaderH;
        DrawHistory(list, ImVec2(min.x, bodyTop), max.y - bodyTop);

        const ImVec2 tMin(min.x + kListW, bodyTop);
        const ImVec2 tMax(max.x, max.y - kComposerH);
        // The transcript: a scrolling child that holds the dialogue
        if (turns_.empty())
        {
            DrawEmpty(list, tMin, ImVec2(tMax.x - tMin.x, tMax.y - tMin.y));
        }
        else
        {
            ImGui::SetCursorScreenPos(tMin);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            if (ImGui::BeginChild("##pgenscroll", ImVec2(tMax.x - tMin.x, tMax.y - tMin.y), false, ImGuiWindowFlags_NoBackground))
            {
                ImDrawList* childList = ImGui::GetWindowDrawList();
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                // The dialogue follows the end of the text while the reader is at the end (the answer grows, the state changes)
                const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f;
                const float inner = tMax.x - tMin.x - 72.0f;
                float y = 24.0f;
                for (std::size_t i = 0; i < turns_.size(); ++i)
                {
                    y += DrawTurn(childList, turns_[i], i, ImVec2(origin.x + 36.0f, origin.y + y), inner, true, time);
                    y += 18.0f;
                }
                ImGui::SetCursorScreenPos(origin);
                ImGui::Dummy(ImVec2(1.0f, y + 24.0f));
                if (scrollFrames_ > 0 || (atBottom && y != lastHeight_))
                {
                    scrollFrames_ = std::max(0, scrollFrames_ - 1);
                    ImGui::SetScrollHereY(1.0f);
                }
                lastHeight_ = y;
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        DrawComposer(list, ImVec2(tMin.x, tMax.y), ImVec2(tMax.x - tMin.x, kComposerH));
        DrawSaveDialog(list, tMin, ImVec2(max.x, max.y));
        DrawToast(list, tMin, tMax);
        list->PopClipRect();
        return back;
    }

    void PrankGenerator::DrawHeader(ImDrawList* list, const ImVec2 min, const float width, bool& back)
    {
        const ImVec2 max(min.x + width, min.y + kHeaderH);
        list->AddRectFilledMultiColor(min, ImVec2(min.x + width * 0.5f, max.y), Rgb(0x3A0B25), Rgb(0x2A0A1C), Rgb(0x2A0A1C), Rgb(0x3A0B25));
        list->AddRectFilledMultiColor(ImVec2(min.x + width * 0.5f, min.y), max, Rgb(0x2A0A1C), Rgb(0x1D0C17), Rgb(0x1D0C17), Rgb(0x2A0A1C));
        list->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), kLine, 1.0f);

        const ImVec2 backMin(min.x + 14.0f, min.y + 12.0f);
        const ImVec2 backMax(backMin.x + 28.0f, backMin.y + 28.0f);
        bool hovered = false;
        if (Hit("##pgenback", backMin, backMax, &hovered))
        {
            back = true;
        }
        if (hovered)
        {
            list->AddRectFilled(backMin, backMax, Rgb(0xFFFFFF, 28), 6.0f);
        }
        const ImVec2 bc((backMin.x + backMax.x) * 0.5f, (backMin.y + backMax.y) * 0.5f);
        list->AddLine(ImVec2(bc.x + 6.0f, bc.y), ImVec2(bc.x - 6.0f, bc.y), kWhite, 1.8f);
        list->AddLine(ImVec2(bc.x - 6.0f, bc.y), ImVec2(bc.x - 1.0f, bc.y - 5.0f), kWhite, 1.8f);
        list->AddLine(ImVec2(bc.x - 6.0f, bc.y), ImVec2(bc.x - 1.0f, bc.y + 5.0f), kWhite, 1.8f);

        Sign18(list, ImVec2(min.x + 70.0f, min.y + 26.0f), 30.0f, kRed, Rgb(0x2A0710), kWhite, 2.5f);
        Text(list, ImVec2(min.x + 94.0f, min.y + 14.0f), 17.0f, kWhite, str::kGenTitle);
        const float titleW = TextWidth(17.0f, str::kGenTitle);
        const ImVec2 chipMin(min.x + 94.0f + titleW + 12.0f, min.y + 15.0f);
        const float chipW = TextWidth(12.0f, str::kGenPremium) + 20.0f;
        list->AddRectFilled(chipMin, ImVec2(chipMin.x + chipW, chipMin.y + 22.0f), kGold, 11.0f);
        TextCentered(list, ImVec2(chipMin.x + chipW * 0.5f, chipMin.y + 11.0f), 12.0f, Rgb(0x3A2300), str::kGenPremium);

        // Right: the model and the incognito chip
        const bool assistant = panel_ != nullptr && panel_->IsEnabled();
        const char* model = assistant ? str::kGenModelClaude : str::kGenModelLocal;
        const float inc = TextWidth(12.0f, str::kGenIncognito) + 38.0f;
        const ImVec2 incMin(max.x - 16.0f - inc, min.y + 15.0f);
        list->AddRectFilled(incMin, ImVec2(max.x - 16.0f, incMin.y + 22.0f), Rgb(0x3A1130), 11.0f);
        list->AddRectFilled(ImVec2(incMin.x + 11.0f, incMin.y + 10.0f), ImVec2(incMin.x + 20.0f, incMin.y + 17.0f), Rgb(0xFFB3D1), 2.0f);
        list->AddRect(ImVec2(incMin.x + 12.5f, incMin.y + 5.0f), ImVec2(incMin.x + 18.5f, incMin.y + 11.0f), Rgb(0xFFB3D1), 3.0f, 0, 1.5f);
        Text(list, ImVec2(incMin.x + 26.0f, incMin.y + 4.0f), 12.0f, Rgb(0xFFB3D1), str::kGenIncognito);
        TextRight(list, incMin.x - 14.0f, min.y + 18.0f, 12.0f, kDimText, model);
    }

    void PrankGenerator::DrawHistory(ImDrawList* list, const ImVec2 min, const float height)
    {
        list->AddRectFilled(min, ImVec2(min.x + kListW, min.y + height), kBgSide);
        list->AddLine(ImVec2(min.x + kListW - 0.5f, min.y), ImVec2(min.x + kListW - 0.5f, min.y + height), kLine, 1.0f);
        // "+ New request"
        const ImVec2 nMin(min.x + 12.0f, min.y + 12.0f);
        const ImVec2 nMax(min.x + kListW - 12.0f, nMin.y + 44.0f);
        bool hovered = false;
        if (Hit("##pgennew", nMin, nMax, &hovered))
        {
            NewChat();
        }
        list->AddRectFilled(nMin, nMax, hovered ? Rgb(0x3A1130) : Rgb(0x2A0F22), 8.0f);
        list->AddRect(nMin, nMax, Rgb(0x6A2350), 8.0f);
        list->AddLine(ImVec2(nMin.x + 18.0f, nMin.y + 22.0f), ImVec2(nMin.x + 30.0f, nMin.y + 22.0f), kWhite, 2.0f);
        list->AddLine(ImVec2(nMin.x + 24.0f, nMin.y + 16.0f), ImVec2(nMin.x + 24.0f, nMin.y + 28.0f), kWhite, 2.0f);
        Text(list, ImVec2(nMin.x + 42.0f, nMin.y + 13.0f), 15.0f, kWhite, str::kGenNew);

        float y = nMax.y + 14.0f;
        int index = 0;
        for (const char* title : str::kGenHistory)
        {
            const ImVec2 rMin(min.x + 8.0f, y);
            const ImVec2 rMax(min.x + kListW - 8.0f, y + 34.0f);
            char id[24];
            std::snprintf(id, sizeof(id), "##pgenhist%d", index++);
            bool rowHover = false;
            if (Hit(id, rMin, rMax, &rowHover))
            {
                Toast(str::kGenHistoryToast);
            }
            if (rowHover)
            {
                list->AddRectFilled(rMin, rMax, Rgb(0x2A0F22), 6.0f);
            }
            TextFit(list, ImVec2(rMin.x + 12.0f, y + 8.0f), kListW - 40.0f, 13.0f, kDimText, title);
            y += 34.0f;
        }
        TextWrapped(list, ImVec2(min.x + 16.0f, min.y + height - 56.0f), kListW - 32.0f, 12.0f, kFineText, str::kGenHistoryFine, false, 16.0f, 3);
    }

    void PrankGenerator::DrawEmpty(ImDrawList* list, const ImVec2 min, const ImVec2 size)
    {
        const float cx = min.x + size.x * 0.5f;
        const float top = min.y + std::max(10.0f, (size.y - 330.0f) * 0.5f);
        Sign18(list, ImVec2(cx, top + 42.0f), 84.0f, kRed, Rgb(0x2A0710), kWhite, 6.0f, true);
        TextCentered(list, ImVec2(cx, top + 118.0f), 30.0f, kWhite, str::kGenGreeting);
        TextCentered(list, ImVec2(cx, top + 158.0f), 17.0f, kDimText, str::kGenQuestion);
        // The chips
        float total = 0.0f;
        for (const char* chip : str::kGenChips)
        {
            total += TextWidth(13.0f, chip) + 40.0f + 10.0f;
        }
        float x = cx - (total - 10.0f) * 0.5f;
        int index = 0;
        static const char* const kIcons[4] = {"flame", "tentacle", "paw", "skull"};
        for (const char* chip : str::kGenChips)
        {
            const float w = TextWidth(13.0f, chip) + 40.0f;
            const ImVec2 a(x, top + 188.0f);
            const ImVec2 b(x + w, a.y + 32.0f);
            char id[24];
            std::snprintf(id, sizeof(id), "##pgenchip%d", index);
            bool hovered = false;
            if (Hit(id, a, b, &hovered))
            {
                Submit(chip);
            }
            list->AddRectFilled(a, b, hovered ? Rgb(0x3A1130) : Rgb(0x2A0F22), 16.0f);
            list->AddRect(a, b, Rgb(0x6A2350), 16.0f);
            // a little drawn icon in place of the emoji
            const ImVec2 ic(a.x + 18.0f, (a.y + b.y) * 0.5f);
            const ImU32 colors[4] = {Rgb(0xFF8A1E), Rgb(0xB05CE8), Rgb(0xF08A2B), Rgb(0xE8E8E8)};
            list->AddCircleFilled(ic, 6.0f, colors[index], 16);
            (void)kIcons;
            Text(list, ImVec2(a.x + 30.0f, a.y + 7.0f), 13.0f, Rgb(0xFFD9EA), chip);
            x += w + 10.0f;
            ++index;
        }
        TextCentered(list, ImVec2(cx, top + 252.0f), 12.0f, kFineText, str::kGenEmptyFine);
    }

    void PrankGenerator::DrawComposer(ImDrawList* list, const ImVec2 min, const ImVec2 size)
    {
        const ImVec2 max(min.x + size.x, min.y + size.y);
        list->AddRectFilled(min, max, kBgSide);
        list->AddLine(min, ImVec2(max.x, min.y), kLine, 1.0f);
        const ImVec2 fMin(min.x + 24.0f, min.y + 18.0f);
        const ImVec2 fMax(max.x - 24.0f - 160.0f - 16.0f, max.y - 20.0f);
        list->AddRectFilled(fMin, fMax, Rgb(0x0E050B), 8.0f);
        list->AddRect(fMin, fMax, Rgb(0x5A1D46), 8.0f);

        ImGui::SetCursorScreenPos(ImVec2(fMin.x + 8.0f, fMin.y + 8.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kWhite));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
        if (focusInput_)
        {
            ImGui::SetKeyboardFocusHere();
            focusInput_ = false;
        }
        const bool entered = ImGui::InputTextMultiline("##pgeninput", inputBuffer_.data(), inputBuffer_.size(), ImVec2(fMax.x - fMin.x - 16.0f, fMax.y - fMin.y - 16.0f),
                                                       ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
        const bool active = ImGui::IsItemActive();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        if (inputBuffer_[0] == '\0' && !active)
        {
            Text(list, ImVec2(fMin.x + 14.0f, fMin.y + 12.0f), 14.0f, kFineText, str::kGenPlaceholder);
        }
        const bool busy = Busy();
        const ImVec2 bMin(max.x - 24.0f - 160.0f, min.y + 18.0f);
        const ImVec2 bMax(bMin.x + 160.0f, bMin.y + 80.0f);
        const bool clicked = Button(list, "##pgensend", bMin, bMax, busy ? str::kGenStop : str::kGenSend, ButtonKind::Primary, 16.0f);
        if (busy)
        {
            if (clicked && !turns_.empty())
            {
                Turn& turn = turns_.back();
                if (turn.assistant && panel_ != nullptr)
                {
                    panel_->GetService().Cancel();
                }
                turn.state = State::Done;
                turn.answer = str::kGenStopped;
            }
        }
        else if (entered || clicked)
        {
            const std::string text(inputBuffer_.data());
            inputBuffer_[0] = '\0';
            Submit(text);
            focusInput_ = true;
        }
    }

    void PrankGenerator::DrawSaveDialog(ImDrawList* list, const ImVec2 areaMin, const ImVec2 areaMax)
    {
        if (askSave_ < 0 || askSave_ >= static_cast<int>(turns_.size()))
        {
            askSave_ = -1;
            return;
        }
        list->AddRectFilled(areaMin, areaMax, Rgb(0x000000, 150));
        const ImVec2 c((areaMin.x + areaMax.x) * 0.5f, (areaMin.y + areaMax.y) * 0.5f);
        const ImVec2 a(c.x - 260.0f, c.y - 100.0f);
        const ImVec2 b(c.x + 260.0f, c.y + 100.0f);
        Glow(list, a, b, WithAlpha(kHot, 0.3f), 18.0f, 4, 12.0f);
        list->AddRectFilled(a, b, Rgb(0x26101F), 12.0f);
        list->AddRect(a, b, kLine, 12.0f);
        Text(list, ImVec2(a.x + 24.0f, a.y + 22.0f), 18.0f, kWhite, str::kGenSaveTitle);
        TextWrapped(list, ImVec2(a.x + 24.0f, a.y + 56.0f), 472.0f, 14.0f, kDimText, str::kGenSaveBody, false, 19.0f, 3);
        const float by = b.y - 56.0f;
        if (Button(list, "##pgsave", ImVec2(a.x + 24.0f, by), ImVec2(a.x + 24.0f + 190.0f, by + 36.0f), str::kGenSaveAndWatch, ButtonKind::Primary, 13.0f))
        {
            if (services_.saveScene)
            {
                services_.saveScene();
            }
            Watch(turns_[static_cast<std::size_t>(askSave_)], true);
        }
        if (Button(list, "##pgnosave", ImVec2(a.x + 24.0f + 200.0f, by), ImVec2(a.x + 24.0f + 200.0f + 190.0f, by + 36.0f), str::kGenWatchNoSave, ButtonKind::Ghost, 13.0f))
        {
            Watch(turns_[static_cast<std::size_t>(askSave_)], true);
        }
        if (Button(list, "##pgcancel", ImVec2(b.x - 24.0f - 70.0f, by), ImVec2(b.x - 24.0f, by + 36.0f), str::kGenCancel, ButtonKind::Ghost, 13.0f))
        {
            askSave_ = -1;
        }
    }

    void PrankGenerator::DrawToast(ImDrawList* list, const ImVec2 areaMin, const ImVec2 areaMax)
    {
        if (toastAge_ >= 2.2f || toast_.empty())
        {
            return;
        }
        const float alpha = toastAge_ < 2.0f ? 1.0f : 1.0f - (toastAge_ - 2.0f) / 0.2f;
        const float w = TextWidth(13.0f, toast_.c_str()) + 32.0f;
        const float cx = (areaMin.x + areaMax.x) * 0.5f;
        const ImVec2 a(cx - w * 0.5f, areaMax.y - 56.0f);
        const ImVec2 b(cx + w * 0.5f, a.y + 38.0f);
        list->AddRectFilled(a, b, Rgb(0x1B1B1B, static_cast<unsigned>(235.0f * alpha)), 6.0f);
        TextCentered(list, ImVec2(cx, (a.y + b.y) * 0.5f), 13.0f, WithAlpha(kWhite, alpha), toast_.c_str());
    }

    // One exchange: the request of the user and the answer of the generator. Returns the height it takes
    float PrankGenerator::DrawTurn(ImDrawList* list, Turn& turn, const std::size_t index, const ImVec2 origin, const float width, const bool draw, const double time)
    {
        (void)draw;
        float y = 0.0f;

        // The user's bubble, to the right
        {
            const float maxW = std::min(760.0f, width * 0.8f);
            const float textW = std::min(maxW - 32.0f, TextWidth(15.0f, turn.user.c_str()));
            const float bubbleW = textW + 32.0f;
            const float textH = WrappedHeight(maxW - 32.0f, 15.0f, turn.user.c_str(), 20.0f);
            const ImVec2 a(origin.x + width - bubbleW, origin.y);
            const ImVec2 b(origin.x + width, origin.y + textH + 24.0f);
            list->AddRectFilled(a, b, Rgb(0x4A1236), 10.0f);
            TextWrapped(list, ImVec2(a.x + 16.0f, a.y + 12.0f), maxW - 32.0f, 15.0f, kWhite, turn.user.c_str(), false, 20.0f);
            y += textH + 24.0f + 12.0f;
        }

        // The generator's bubble
        const float bubbleW = std::min(760.0f, width);
        const float inner = bubbleW - 40.0f;
        const bool working = turn.state == State::Working;
        const bool showPicture = !working && turn.state != State::Done && turn.state != State::Error;

        // Measure first, then draw the background, then the contents
        auto layout = [&](const bool paint) -> float
        {
            float h = 16.0f;
            const float x = origin.x + 22.0f;
            const float top = origin.y + y;
            if (working)
            {
                char title[64];
                std::snprintf(title, sizeof(title), "%s %d%%", str::kGenWorking, str::kGenPercents[static_cast<std::size_t>(std::min<int>(turn.percentIndex, static_cast<int>(str::kGenPercents.size()) - 1))]);
                if (paint)
                {
                    Text(list, ImVec2(x, top + h), 16.0f, kWhite, title);
                }
                h += 28.0f;
                // The bar: slanted stripes that run to the right
                if (paint)
                {
                    const ImVec2 a(x, top + h);
                    const ImVec2 b(x + inner, a.y + 10.0f);
                    list->AddRectFilled(a, b, Rgb(0x3A1130), 5.0f);
                    const float fraction = static_cast<float>(str::kGenPercents[static_cast<std::size_t>(std::min<int>(turn.percentIndex, static_cast<int>(str::kGenPercents.size()) - 1))]) / 100.0f;
                    const float fillRight = a.x + inner * fraction;
                    list->PushClipRect(a, ImVec2(fillRight, b.y), true);
                    const float shift = static_cast<float>(std::fmod(time * 30.0, 20.0));
                    for (float sx = a.x - 20.0f + shift; sx < fillRight + 10.0f; sx += 20.0f)
                    {
                        const ImVec2 quad[] = {ImVec2(sx, b.y), ImVec2(sx + 10.0f, b.y), ImVec2(sx + 20.0f, a.y), ImVec2(sx + 10.0f, a.y)};
                        list->AddConvexPolyFilled(quad, 4, Rgb(0xFF2D7A));
                        const ImVec2 quad2[] = {ImVec2(sx + 10.0f, b.y), ImVec2(sx + 20.0f, b.y), ImVec2(sx + 30.0f, a.y), ImVec2(sx + 20.0f, a.y)};
                        list->AddConvexPolyFilled(quad2, 4, Rgb(0xFF6AA3));
                    }
                    list->PopClipRect();
                }
                h += 10.0f + 12.0f;
                for (const std::string& step : turn.steps)
                {
                    if (paint)
                    {
                        const bool last = &step == &turn.steps.back();
                        const std::string line = std::string(last ? "~ " : "+ ") + step;
                        Text(list, ImVec2(x, top + h), 12.0f, kDimText, line.c_str());
                    }
                    h += 18.0f;
                }
                h += 6.0f;
                if (paint)
                {
                    Text(list, ImVec2(x, top + h), 12.0f, kFineText, str::kGenWhispers[static_cast<std::size_t>(turn.whisper) % str::kGenWhispers.size()]);
                }
                h += 18.0f + 14.0f;
                return h;
            }

            // The answer
            if (!turn.answer.empty())
            {
                if (paint)
                {
                    TextWrapped(list, ImVec2(x, top + h), inner, 15.0f, Rgb(0xF4E3EB), turn.answer.c_str(), false, 21.0f);
                }
                h += WrappedHeight(inner, 15.0f, turn.answer.c_str(), 21.0f) + 12.0f;
            }
            if (showPicture)
            {
                const ImVec2 a(x, top + h);
                const ImVec2 b(a.x + std::min(kPreviewW, inner), a.y + kPreviewH * std::min(1.0f, inner / kPreviewW));
                if (paint)
                {
                    list->AddRectFilled(ImVec2(a.x - 1.0f, a.y - 1.0f), ImVec2(b.x + 1.0f, b.y + 1.0f), kLine, 9.0f);
                    DrawPreview(list, a, b, turn.match.scene, time);
                    DrawPlate(list, a, b, turn.reveal);
                    if (turn.reveal < 0.5f && (turn.state == State::Ready || turn.state == State::Rejected))
                    {
                        char id[32];
                        std::snprintf(id, sizeof(id), "##pgplate%zu", index);
                        if (Hit(id, a, b))
                        {
                            Watch(turn, false);
                        }
                    }
                }
                h += (b.y - a.y) + 14.0f;
                // The buttons
                const ImVec2 bMin(x, top + h);
                if (turn.state == State::Ready)
                {
                    if (paint)
                    {
                        char idA[32];
                        char idB[32];
                        std::snprintf(idA, sizeof(idA), "##pgwatch%zu", index);
                        std::snprintf(idB, sizeof(idB), "##pgnot%zu", index);
                        if (Button(list, idA, bMin, ImVec2(bMin.x + 170.0f, bMin.y + 40.0f), str::kGenWatch, ButtonKind::Primary, 15.0f))
                        {
                            Watch(turn, false);
                        }
                        if (Button(list, idB, ImVec2(bMin.x + 182.0f, bMin.y), ImVec2(bMin.x + 182.0f + 130.0f, bMin.y + 40.0f), str::kGenNotNow, ButtonKind::Ghost, 15.0f))
                        {
                            Reject(turn);
                        }
                    }
                    h += 40.0f + 8.0f;
                }
                else if (turn.state == State::Applied)
                {
                    if (paint)
                    {
                        char id[32];
                        std::snprintf(id, sizeof(id), "##pgplay%zu", index);
                        const ImVec2 pMax(bMin.x + 170.0f, bMin.y + 40.0f);
                        bool hovered = false;
                        const bool clicked = Hit(id, bMin, pMax, &hovered);
                        GradientV(list, bMin, pMax, hovered ? Rgb(0x4CC56C) : Rgb(0x3CB35A), hovered ? Rgb(0x7CF09A) : Rgb(0x6BE38A), 6.0f);
                        const ImVec2 tc(bMin.x + 28.0f, bMin.y + 20.0f);
                        list->AddTriangleFilled(ImVec2(tc.x - 6.0f, tc.y - 8.0f), ImVec2(tc.x - 6.0f, tc.y + 8.0f), ImVec2(tc.x + 8.0f, tc.y), Rgb(0x0F2A17));
                        Text(list, ImVec2(bMin.x + 48.0f, bMin.y + 10.0f), 15.0f, Rgb(0x0F2A17), str::kGenPlay);
                        Text(list, ImVec2(bMin.x + 184.0f, bMin.y + 13.0f), 12.0f, kFineText, str::kGenApplied);
                        if (clicked && services_.playNow)
                        {
                            services_.playNow();
                        }
                    }
                    h += 40.0f + 8.0f;
                }
                else if (turn.state == State::Rejected)
                {
                    if (paint)
                    {
                        Text(list, ImVec2(x, top + h), 13.0f, kDimText, str::kGenNotNowReply);
                    }
                    h += 22.0f;
                }
            }
            h += 10.0f;
            return h;
        };

        const float height = layout(false);
        const ImVec2 a(origin.x, origin.y + y);
        const ImVec2 b(origin.x + bubbleW, a.y + height);
        list->AddRectFilled(a, b, Rgb(0x26101F), 10.0f);
        list->AddRect(a, b, kLine, 10.0f);
        list->AddRectFilled(a, ImVec2(a.x + 3.0f, b.y), kHot, 10.0f, ImDrawFlags_RoundCornersLeft);
        layout(true);
        return y + height;
    }
}
