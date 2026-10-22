// The RL Tuning window: every number of assets/data/rl_tuning.json of the running Rocket League game as a drag value, by groups. A change
// is taken by the running match at once (the car, the ball and the camera read their numbers every tick); "Save to json" writes the file,
// and the game also reads the file again whenever it changes on disk.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <rl/RlTuning.h>

#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        // draws the fields the tuning reports; a collapsed group or section draws nothing
        class TuningFields final : public rl::RlTuningVisitor
        {
        public:
            bool changed = false;

            void Begin(const char* group) override
            {
                CloseSection();
                inGroup_ = true;
                sectionActive_ = false;
                groupOpen_ = ImGui::CollapsingHeader(group, std::strcmp(group, "car") == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                ImGui::PushID(group);
            }

            void End() override
            {
                CloseSection();
                ImGui::PopID();
                inGroup_ = false;
                groupOpen_ = false;
                sectionActive_ = false;
            }

            void Section(const char* title) override
            {
                CloseSection();
                sectionActive_ = true;
                sectionOpen_ = groupOpen_ && ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            }

            void Float(const char* name, float& value) override
            {
                if (Visible())
                {
                    changed |= Drag(name, value);
                }
            }

            void Int(const char* name, int& value) override
            {
                if (Visible())
                {
                    changed |= ImGui::DragInt(name, &value, 0.2f);
                }
            }

            void Floats(const char* name, float* values, int count) override
            {
                if (!Visible())
                {
                    return;
                }
                for (int i = 0; i < count; ++i)
                {
                    char label[96];
                    std::snprintf(label, sizeof(label), "%s[%d]", name, i);
                    changed |= Drag(label, values[i]);
                }
            }

            void Finish() { CloseSection(); }

        private:
            static bool Drag(const char* label, float& value)
            {
                // the step of a drag follows the size of the number: the small ones (0.06 s) and the big ones (5000) both move by feel
                const float speed = std::max(std::fabs(value) * 0.004f, 0.0004f);
                return ImGui::DragFloat(label, &value, speed, 0.0f, 0.0f, "%.5g");
            }

            // the numbers outside of any group come first and are always drawn
            bool Visible() const { return !inGroup_ || (groupOpen_ && (!sectionActive_ || sectionOpen_)); }

            void CloseSection()
            {
                if (sectionOpen_)
                {
                    ImGui::TreePop();
                }
                sectionOpen_ = false;
            }

            bool inGroup_ = false;
            bool groupOpen_ = false;
            bool sectionActive_ = false;
            bool sectionOpen_ = false;
        };
    }

    void SceneEditor::DrawRlTuningWindow()
    {
        if (!showRlTuning_ || !services_.rlTuning)
        {
            return;
        }
        rl::RlTuning* tuning = services_.rlTuning();
        if (tuning == nullptr)
        {
            return;  // no Rocket League match is running
        }

        // first time: at the right edge of the editor window, out of the way of the game
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(mainViewport->Pos.x + mainViewport->Size.x - 450.0f, mainViewport->Pos.y + 120.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(430.0f, 620.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("RL Tuning###RlTuning", &showRlTuning_))
        {
            if (ImGui::Button("Save to json") && services_.rlTuningSave)
            {
                rlTuningMessage_ = services_.rlTuningSave();
            }
            ImGui::SameLine();
            if (ImGui::Button("Read the file again") && services_.rlTuningReload)
            {
                rlTuningMessage_ = services_.rlTuningReload();
            }
            ImGui::TextDisabled("Drag a value (Ctrl+click types one). The game takes it at once.");
            if (!rlTuningMessage_.empty())
            {
                ImGui::TextUnformatted(rlTuningMessage_.c_str());
            }
            ImGui::Separator();

            ImGui::BeginChild("##RlTuningFields", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            TuningFields fields;
            tuning->Visit(fields);
            fields.Finish();
            ImGui::EndChild();
            if (fields.changed && services_.rlTuningEdited)
            {
                services_.rlTuningEdited();
                rlTuningMessage_ = "changed (not saved yet)";
            }
        }
        ImGui::End();
    }
}
