// PrankGenerator.h
//
// The end of the easter egg tab: "AI generator 18+". It is the real assistant of the editor (AssistantService behind a second AssistantPanel with its own prompt,
// assets/assistant/prank_prompt.md) dressed up as an adult generator: whatever is asked, a harmless little game is built in the open scene. Without the
// assistant (off, or no CLI) the same screen works from prepared mini-scenes picked by key words (PrankMatcher). Internal to engine/src/ui/editor.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui/imgui.h>

#include "PrankMatcher.h"

namespace myengine::ui
{
    class AssistantPanel;
}

namespace myengine::ui::prank
{
    // What the generator needs from the editor
    struct PrankSceneServices
    {
        std::filesystem::path root;                                              // the repository root (assets/prank/scenes/...)
        std::function<std::string()> capture;                                    // the open scene as a snapshot
        std::function<bool(const std::string&)> restore;                         // puts a snapshot into the scene
        std::function<void(const std::string& label, const std::string& before)> recordUndo;   // one undo step
        std::function<bool()> sceneDirty;                                        // unsaved changes in the open scene
        std::function<bool()> saveScene;
        std::function<bool()> isPlaying;
        std::function<void()> stopPlay;
        std::function<void()> playNow;                                           // the Viewport comes to the front and the game starts
        // The second assistant panel (null: the editor has no assistant at all); called once, when the first request is sent
        std::function<std::unique_ptr<AssistantPanel>()> makePanel;
    };

    class PrankGenerator
    {
    public:
        explicit PrankGenerator(PrankSceneServices services);
        ~PrankGenerator();

        // Every frame, also while the tab is hidden: the assistant runs on
        void Update(float dt);
        // Draws the screen in [min, max]. Returns true when the "back" arrow was pressed
        bool Draw(ImDrawList* list, ImVec2 min, ImVec2 max, double time, float dt);
        void Shutdown();

        // The way in: a request typed or chosen. For the tests and the screenshots (no mouse needed)
        void Submit(const std::string& request);
        // True while the answer of a request is being made
        bool Busy() const;
        // What the last request used: the assistant (true) or the prepared scenes (false)
        bool UsedAssistant() const { return usedAssistant_; }
        // For the tests: the last turn
        int TurnCount() const { return static_cast<int>(turns_.size()); }
        bool LastTurnReady() const;
        bool LastTurnApplied() const;
        MiniScene LastScene() const;
        // Applies the scene of the last turn as the button "watch" does (the test and the screenshots)
        void WatchLast();

    private:
        enum class State
        {
            Working,
            Ready,     // the answer is there, the picture is hidden under the plate, the buttons wait
            Applied,   // the plate is gone, "play" is offered
            Rejected,
            Done,      // an answer without a scene
            Error,
        };

        struct Turn
        {
            std::string user;
            State state = State::Working;
            bool assistant = false;      // the real assistant answers (false: the prepared scenes)
            Match match;
            std::string answer;
            std::vector<std::string> steps;
            int percentIndex = 0;
            float age = 0.0f;            // seconds since the turn began
            float stateAge = 0.0f;       // seconds in the current state
            float reveal = 0.0f;         // the plate melting: 0 hidden under it .. 1 gone
            int whisper = 0;
            std::size_t baseIndex = 0;   // the first message of the assistant that belongs to this turn
            std::size_t toolCalls = 0;
            bool autoApprove = false;
            bool wantsWatch = false;
            std::string note;
        };

        void BeginTurn(const std::string& request);
        void StartFallback(Turn& turn);
        void UpdateTurn(Turn& turn, float dt);
        void UpdateAssistantTurn(Turn& turn, float dt);
        void UpdateFallbackTurn(Turn& turn);
        void Watch(Turn& turn, bool force);
        bool ApplyScene(Turn& turn);
        void Reject(Turn& turn);
        void NewChat();
        void EnsurePanel();
        float DrawTurn(ImDrawList* list, Turn& turn, std::size_t index, ImVec2 origin, float width, bool draw, double time);
        void DrawHeader(ImDrawList* list, ImVec2 min, float width, bool& back);
        void DrawHistory(ImDrawList* list, ImVec2 min, float height);
        void DrawEmpty(ImDrawList* list, ImVec2 min, ImVec2 size);
        void DrawComposer(ImDrawList* list, ImVec2 min, ImVec2 size);
        void DrawSaveDialog(ImDrawList* list, ImVec2 areaMin, ImVec2 areaMax);
        void DrawToast(ImDrawList* list, ImVec2 areaMin, ImVec2 areaMax);
        void Toast(const std::string& text);

        PrankSceneServices services_;
        std::unique_ptr<AssistantPanel> panel_;
        bool panelTried_ = false;
        bool usedAssistant_ = false;
        std::vector<Turn> turns_;
        std::string input_;
        std::vector<char> inputBuffer_;
        bool scrollDown_ = false;
        int scrollFrames_ = 0;
        std::size_t lastCount_ = 0;
        std::string toast_;
        float toastAge_ = 100.0f;
        int askSave_ = -1;          // the turn that waits for the answer of "save the open scene?"
        std::uint32_t seed_ = 1;
        double now_ = 0.0;
        float lastHeight_ = 0.0f;
        bool focusInput_ = true;
    };
}
