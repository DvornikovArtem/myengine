#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include <DirectXMath.h>
#include <imgui/imgui.h>
#include "ImGuizmo.h"

#include <myengine/ecs/World.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/editor/TransformGizmo.h>

namespace
{
    namespace ecs = myengine::ecs;
    namespace editor = myengine::editor;
    using myengine::ecs::components::TransformComponent;

    constexpr float kWidth = 1280.0f;
    constexpr float kHeight = 720.0f;

    void Check(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // A headless ImGui frame with a full-window "Viewport" window, the way the editor hosts the gizmo
    struct Fixture
    {
        Fixture()
        {
            IMGUI_CHECKVERSION();
            context = ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.DisplaySize = ImVec2(kWidth, kHeight);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

            const DirectX::XMVECTOR eye = DirectX::XMVectorSet(0.0f, 3.0f, -6.0f, 1.0f);
            const DirectX::XMMATRIX rotation = DirectX::XMMatrixRotationRollPitchYaw(DirectX::XMConvertToRadians(20.0f), 0.0f, 0.0f);
            const DirectX::XMVECTOR forward = DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rotation);
            const DirectX::XMVECTOR up = DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), rotation);
            view = DirectX::XMMatrixLookToLH(eye, forward, up);
            projection = DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(60.0f), kWidth / kHeight, 0.05f, 200.0f);

            first = world.CreateEntity();
            world.Emplace<TransformComponent>(first).position = {0.0f, 0.5f, 2.0f};
            second = world.CreateEntity();
            world.Emplace<TransformComponent>(second).position = {3.0f, 0.5f, 2.0f};
        }

        ~Fixture()
        {
            ImGui::DestroyContext(context);
        }

        // One frame; returns whether the gizmo reported a change
        bool Frame(const ecs::EntityId entity, const ImVec2 mouse, const bool leftDown)
        {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, leftDown);

            ImGui::NewFrame();
            ImGuizmo::BeginFrame();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(kWidth, kHeight), ImGuiCond_Always);
            ImGui::Begin(
                "Viewport",
                nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);

            const bool changed = gizmo.DrawAndHandle(editor::TransformGizmo::Context{
                world,
                entity,
                1u,
                editor::ViewportRect{0.0f, 0.0f, kWidth, kHeight},
                view,
                projection,
                editor::GizmoOperation::Translate,
                editor::GizmoSpace::World,
                true,
                nullptr,
            });

            ImGui::End();
            ImGui::Render();
            return changed;
        }

        ImVec2 ProjectToScreen(const ecs::EntityId entity) const
        {
            const auto& position = world.TryGet<TransformComponent>(entity)->position;
            const DirectX::XMVECTOR clip = DirectX::XMVector4Transform(
                DirectX::XMVectorSet(position.x, position.y, position.z, 1.0f),
                view * projection);
            DirectX::XMFLOAT4 value{};
            DirectX::XMStoreFloat4(&value, clip);
            return ImVec2(
                (value.x / value.w * 0.5f + 0.5f) * kWidth,
                (1.0f - (value.y / value.w * 0.5f + 0.5f)) * kHeight);
        }

        // Finds a screen point where the gizmo of `entity` is hovered and that is clear of the entity centre
        ImVec2 FindHandle(const ecs::EntityId entity)
        {
            const ImVec2 centre = ProjectToScreen(entity);
            for (float offset = 24.0f; offset <= 120.0f; offset += 4.0f)
            {
                const ImVec2 probe(centre.x + offset, centre.y);
                Frame(entity, probe, false);
                Frame(entity, probe, false);
                if (gizmo.IsHovered())
                {
                    return probe;
                }
            }
            throw std::runtime_error("No gizmo handle found to the right of the entity centre");
        }

        // Presses the left button for two frames; the gizmo is expected to start a drag during them
        void Press(const ecs::EntityId entity, const ImVec2 mouse)
        {
            Frame(entity, mouse, true);
            Frame(entity, mouse, true);
        }

        void Release(const ecs::EntityId entity, const ImVec2 mouse)
        {
            Frame(entity, mouse, false);
            Frame(entity, mouse, false);
        }

        float X(const ecs::EntityId entity) const
        {
            return world.TryGet<TransformComponent>(entity)->position.x;
        }

        ImGuiContext* context = nullptr;
        ecs::World world;
        editor::TransformGizmo gizmo;
        DirectX::XMMATRIX view = DirectX::XMMatrixIdentity();
        DirectX::XMMATRIX projection = DirectX::XMMatrixIdentity();
        ecs::EntityId first = ecs::kInvalidEntity;
        ecs::EntityId second = ecs::kInvalidEntity;
    };

    // The click that grabs a handle must be reported as "gizmo busy" in the same frame the viewport
    // picks, otherwise the click also re-selects whatever is under the cursor and the drag is lost
    void TestDragIsReportedAndMovesEntity()
    {
        Fixture fixture;
        fixture.Frame(fixture.first, ImVec2(5.0f, 5.0f), false);
        fixture.Frame(fixture.first, ImVec2(5.0f, 5.0f), false);

        const ImVec2 handle = fixture.FindHandle(fixture.first);
        const float startX = fixture.X(fixture.first);

        fixture.Frame(fixture.first, handle, true);
        Check(fixture.gizmo.IsHovered(), "The click on a gizmo handle was not reported as hovered");
        Check(fixture.gizmo.IsUsing(), "The click on a gizmo handle did not start a drag");

        bool changed = false;
        for (int step = 1; step <= 10; ++step)
        {
            changed |= fixture.Frame(fixture.first, ImVec2(handle.x + 4.0f * static_cast<float>(step), handle.y), true);
            Check(fixture.gizmo.IsUsing(), "The drag was lost while the button was held");
        }
        Check(changed, "Dragging a handle never reported a change");
        const float draggedX = fixture.X(fixture.first);
        Check(std::fabs(draggedX - startX) > 0.05f, "The entity did not follow the mouse");

        fixture.Release(fixture.first, ImVec2(handle.x + 40.0f, handle.y));
        Check(!fixture.gizmo.IsUsing(), "The drag did not end after the button was released");
        Check(std::fabs(fixture.X(fixture.first) - draggedX) < 1e-4f, "The entity moved after the button was released");
    }

    // Selecting another entity mid-drag must cancel the drag, otherwise it resumes from the mouse position
    // of whatever frame the first entity is selected again, and the entity jumps across the scene
    void TestSelectionChangeCancelsDrag()
    {
        Fixture fixture;
        fixture.Frame(fixture.first, ImVec2(5.0f, 5.0f), false);
        fixture.Frame(fixture.first, ImVec2(5.0f, 5.0f), false);

        const ImVec2 handle = fixture.FindHandle(fixture.first);
        fixture.Press(fixture.first, handle);
        Check(fixture.gizmo.IsUsing(), "The click on a gizmo handle did not start a drag");
        fixture.Frame(fixture.first, ImVec2(handle.x + 10.0f, handle.y), true);
        const float draggedX = fixture.X(fixture.first);

        const ImVec2 farAway(handle.x + 400.0f, handle.y + 200.0f);
        fixture.Frame(fixture.second, farAway, true);
        Check(!fixture.gizmo.IsUsing(), "The drag continued after another entity was selected");

        fixture.Release(fixture.second, farAway);
        fixture.Frame(fixture.first, farAway, false);
        fixture.Frame(fixture.first, farAway, false);
        Check(!fixture.gizmo.IsUsing(), "A stale drag resumed once the first entity was selected again");
        Check(std::fabs(fixture.X(fixture.first) - draggedX) < 1e-4f, "The first entity jumped after it was selected again");
    }
}

int main()
{
    try
    {
        TestDragIsReportedAndMovesEntity();
        TestSelectionChangeCancelsDrag();
        std::cout << "OK: gizmo reports the drag on the click frame, follows the mouse, ends on release and drops a stale drag on selection change\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
