// PrankGags.h
//
// The gags between the folders of the easter egg tab (docs/editor/prank-tab.md): the captcha with the naked polygons, the paywall, "are you Ivan", the age
// check, the video that stops at 99 %, the UAC window. Each is a dialog drawn over the explorer. Internal to engine/src/ui/editor.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <imgui/imgui.h>

#include "PrankExplorer.h"

namespace myengine::ui::prank
{
    // The Face ID of the age check (Should): the tab gives it when the editor has Face ID and a camera. Called from the main thread only
    struct AgeCheckFace
    {
        enum class Phase { Scanning, Recognized, Other, Unknown };
        struct State
        {
            Phase phase = Phase::Scanning;
            std::string name;
            ImTextureID picture = 0;    // the mirrored frame of the camera, 0 while there is none
            float aspect = 1.0f;
        };
        std::function<bool()> available;                // there is a camera and the models
        std::function<void()> start;                    // the camera goes on
        std::function<void()> stop;                     // the camera goes off at once
        std::function<State()> poll;                    // the recognition so far
    };

    class PrankGags
    {
    public:
        void SetAgeCheckFace(AgeCheckFace face) { face_ = std::move(face); }

        // Opens a gag; `video` is the 99 % gag of the video file (no level change)
        void Open(GagKind kind, bool video = false);
        bool Active() const { return kind_ != GagKind::None; }
        GagKind Kind() const { return kind_; }

        // Draws the gag over [areaMin, areaMax]. Returns true in the frame the gag ends; then Passed() tells whether it was passed (false: Esc)
        bool Draw(ImDrawList* list, ImVec2 areaMin, ImVec2 areaMax, double time, float dt);
        bool Passed() const { return passed_; }
        bool WasVideo() const { return video_; }
        // The toast the explorer shows after the gag (empty: none)
        const std::string& ToastAfter() const { return toastAfter_; }
        // The fill of the address bar while the 99 % gag runs (negative: none)
        float AddressFill() const { return addressFill_; }
        // The explorer shows "working" instead of the files
        bool Working() const { return kind_ == GagKind::Load99; }

        // For the screenshots: the gag shows its state at a given moment
        void ForceAge(float seconds) { age_ = seconds; }

    private:
        void Finish(bool passed, const char* toast);
        void DrawScrim(ImDrawList* list, ImVec2 min, ImVec2 max, unsigned rgb, float alpha, float eased);
        void DrawCaptcha(ImDrawList* list, ImVec2 center, double time);
        void DrawPaywall(ImDrawList* list, ImVec2 center, double time);
        void DrawIvan(ImDrawList* list, ImVec2 center);
        void DrawAge(ImDrawList* list, ImVec2 areaMin, ImVec2 areaMax, ImVec2 center, double time, float dt);
        void DrawLoad(ImDrawList* list, ImVec2 areaMin, ImVec2 areaMax, ImVec2 center, double time);
        void DrawUac(ImDrawList* list, ImVec2 center);

        GagKind kind_ = GagKind::None;
        bool video_ = false;
        bool passed_ = false;
        bool finished_ = false;
        float age_ = 0.0f;
        std::string toastAfter_;
        float addressFill_ = -1.0f;
        AgeCheckFace face_;

        // captcha
        bool selected_[9]{};
        float shrink_[9]{};
        // paywall
        float paying_ = -1.0f;
        // ivan
        bool ivanCaught_ = false;
        // age
        ImVec2 runPosition_{};
        ImVec2 runFrom_{};
        ImVec2 runTo_{};
        float runAge_ = 1.0f;
        bool runInit_ = false;
        bool faceStarted_ = false;
        AgeCheckFace::State faceState_;
        float faceDoneAge_ = -1.0f;
        float faceWaitAge_ = 0.0f;
        std::uint32_t random_ = 99u;
        // load
        float errorAge_ = -1.0f;
        float closingAge_ = -1.0f;
    };
}
