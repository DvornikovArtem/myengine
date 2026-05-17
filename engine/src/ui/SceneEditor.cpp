#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <DirectXCollision.h>
#include <DirectXMath.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <imgui/misc/imgui_stdlib.h>

#include <myengine/core/Logger.h>
#include <myengine/core/ServiceLocator.h>
#include <myengine/editor/EditorCommandHistory.h>
#include <myengine/editor/TransformGizmo.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/physics/PhysicsWorldState.h>
#include <myengine/resource/ResourceManager.h>
#include <myengine/scene/TransformUtils.h>
#include <myengine/ui/SceneEditor.h>

namespace myengine::ui
{
    namespace
    {
        constexpr char kHierarchyWindowName[] = "Scene Hierarchy";
        constexpr char kInspectorWindowName[] = "Inspector";
        constexpr char kStatisticsWindowName[] = "Statistics";
        constexpr char kViewportWindowName[] = "Viewport";
        constexpr char kMaterialEditorWindowName[] = "Material Editor";
        constexpr char kAssetBrowserWindowName[] = "Asset Browser";
        constexpr char kMeshPayloadType[] = "MYENGINE_ASSET_MESH";
        constexpr char kMaterialPayloadType[] = "MYENGINE_ASSET_MATERIAL";
        constexpr char kTexturePayloadType[] = "MYENGINE_ASSET_TEXTURE";
        constexpr char kDefaultMaterialPath[] = "assets/materials/default.material.json";
        constexpr char kDefaultShaderPath[] = "assets/shaders/textured_lit.shader.json";
        constexpr char kCubeMeshPath[] = "assets/models/crate.obj";
        constexpr char kSphereMeshPath[] = "assets/models/sphere.obj";
        constexpr float kViewportToolbarPadding = 12.0f;
        constexpr float kViewportToolbarHeight = 44.0f;
        constexpr float kDefaultRenderableRadius = 0.8660254f;

        bool MatchesWindowBinding(ecs::World& world, const ecs::EntityId entity, const core::WindowId windowId)
        {
            const auto* binding = world.TryGet<ecs::components::WindowBindingComponent>(entity);
            return binding == nullptr || binding->windowId == 0 || binding->windowId == windowId;
        }

        std::string EntityLabel(ecs::World& world, const ecs::EntityId entity)
        {
            const auto* tag = world.TryGet<ecs::components::TagComponent>(entity);
            const std::string name = tag != nullptr && !tag->name.empty()
                ? tag->name
                : "Entity";
            return name + "##entity_" + std::to_string(entity);
        }

        std::string EntityDisplayName(ecs::World& world, const ecs::EntityId entity)
        {
            const auto* tag = world.TryGet<ecs::components::TagComponent>(entity);
            if (tag != nullptr && !tag->name.empty())
            {
                return tag->name;
            }

            return "Entity_" + std::to_string(entity);
        }

        std::string FileNameLabel(const std::string& path)
        {
            if (path.empty())
            {
                return "<none>";
            }

            return std::filesystem::path(path).filename().string();
        }

        std::string FormatBytes(const std::uint64_t bytes)
        {
            static constexpr std::array<const char*, 4> units{"B", "KB", "MB", "GB"};
            double value = static_cast<double>(bytes);
            std::size_t unitIndex = 0;
            while (value >= 1024.0 && unitIndex + 1 < units.size())
            {
                value /= 1024.0;
                ++unitIndex;
            }

            std::ostringstream stream;
            stream.setf(std::ios::fixed);
            stream.precision(unitIndex == 0 ? 0 : 2);
            stream << value << ' ' << units[unitIndex];
            return stream.str();
        }

        std::string SanitizeStem(const std::filesystem::path& path)
        {
            std::string stem = path.stem().string();
            if (stem.empty())
            {
                stem = "asset";
            }

            for (char& character : stem)
            {
                const unsigned char code = static_cast<unsigned char>(character);
                if (!(std::isalnum(code) || character == '_' || character == '-'))
                {
                    character = '_';
                }
            }

            return stem;
        }

        std::string CanonicalAssetId(std::filesystem::path path)
        {
            if (path.empty())
            {
                return {};
            }

            while (path.has_extension())
            {
                path = path.stem();
            }

            std::string result;
            const std::string fileName = path.filename().string();
            result.reserve(fileName.size());
            for (const char character : fileName)
            {
                const unsigned char code = static_cast<unsigned char>(character);
                if (std::isalnum(code))
                {
                    result.push_back(static_cast<char>(std::tolower(code)));
                }
            }

            return result;
        }

        const char* PreviewMeshPath(const editor::MaterialPreviewShape shape)
        {
            return shape == editor::MaterialPreviewShape::Cube ? kCubeMeshPath : kSphereMeshPath;
        }

        bool MaterialEquals(const resource::MaterialAsset& lhs, const resource::MaterialAsset& rhs)
        {
            return lhs.shaderPath == rhs.shaderPath &&
                lhs.texturePath == rhs.texturePath &&
                lhs.tint.r == rhs.tint.r &&
                lhs.tint.g == rhs.tint.g &&
                lhs.tint.b == rhs.tint.b &&
                lhs.tint.a == rhs.tint.a;
        }

        void DrawSceneVisibilityMask(const editor::ViewportRect& viewportRect)
        {
            ImGuiViewport* mainViewport = ImGui::GetMainViewport();
            if (mainViewport == nullptr)
            {
                return;
            }

            ImDrawList* backgroundDrawList = ImGui::GetBackgroundDrawList(mainViewport);
            if (backgroundDrawList == nullptr)
            {
                return;
            }

            const ImVec2 frameMin = mainViewport->Pos;
            const ImVec2 frameMax = ImVec2(
                mainViewport->Pos.x + mainViewport->Size.x,
                mainViewport->Pos.y + mainViewport->Size.y);
            const ImU32 maskColor = ImGui::GetColorU32(ImVec4(0.08f, 0.09f, 0.11f, 1.0f));

            if (!viewportRect.IsValid())
            {
                backgroundDrawList->AddRectFilled(frameMin, frameMax, maskColor);
                return;
            }

            const float viewportMinX = std::clamp(viewportRect.x, frameMin.x, frameMax.x);
            const float viewportMinY = std::clamp(viewportRect.y, frameMin.y, frameMax.y);
            const float viewportMaxX = std::clamp(viewportRect.x + viewportRect.width, frameMin.x, frameMax.x);
            const float viewportMaxY = std::clamp(viewportRect.y + viewportRect.height, frameMin.y, frameMax.y);

            if (viewportMinY > frameMin.y)
            {
                backgroundDrawList->AddRectFilled(frameMin, ImVec2(frameMax.x, viewportMinY), maskColor);
            }

            if (viewportMaxY < frameMax.y)
            {
                backgroundDrawList->AddRectFilled(ImVec2(frameMin.x, viewportMaxY), frameMax, maskColor);
            }

            if (viewportMinX > frameMin.x)
            {
                backgroundDrawList->AddRectFilled(
                    ImVec2(frameMin.x, viewportMinY),
                    ImVec2(viewportMinX, viewportMaxY),
                    maskColor);
            }

            if (viewportMaxX < frameMax.x)
            {
                backgroundDrawList->AddRectFilled(
                    ImVec2(viewportMaxX, viewportMinY),
                    ImVec2(frameMax.x, viewportMaxY),
                    maskColor);
            }
        }

        std::vector<ecs::EntityId> CollectVisibleEntities(ecs::World& world, const core::WindowId windowId)
        {
            std::vector<ecs::EntityId> entities = world.GetEntities();
            entities.erase(
                std::remove_if(
                    entities.begin(),
                    entities.end(),
                    [&](const ecs::EntityId entity)
                    {
                        return !MatchesWindowBinding(world, entity, windowId);
                    }),
                entities.end());

            std::sort(
                entities.begin(),
                entities.end(),
                [&](const ecs::EntityId lhs, const ecs::EntityId rhs)
                {
                    const std::string lhsName = EntityDisplayName(world, lhs);
                    const std::string rhsName = EntityDisplayName(world, rhs);
                    if (lhsName != rhsName)
                    {
                        return lhsName < rhsName;
                    }

                    return lhs < rhs;
                });
            return entities;
        }

        std::array<float, 3> ToFloat3(const ecs::components::Vec3& value)
        {
            return {value.x, value.y, value.z};
        }

        void FromFloat3(const std::array<float, 3>& value, ecs::components::Vec3& target)
        {
            target.x = value[0];
            target.y = value[1];
            target.z = value[2];
        }

        void SetEditorTheme()
        {
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding = 8.0f;
            style.ChildRounding = 6.0f;
            style.FrameRounding = 5.0f;
            style.GrabRounding = 5.0f;
            style.PopupRounding = 6.0f;
            style.TabRounding = 5.0f;
            style.ScrollbarRounding = 8.0f;
            style.WindowPadding = ImVec2(12.0f, 10.0f);
            style.FramePadding = ImVec2(8.0f, 6.0f);
            style.ItemSpacing = ImVec2(8.0f, 6.0f);
            style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
            style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.96f);
            style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.14f, 0.98f);
            style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.17f, 0.20f, 1.0f);
            style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.26f, 0.31f, 1.0f);
            style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.33f, 0.39f, 1.0f);
            style.Colors[ImGuiCol_Header] = ImVec4(0.21f, 0.27f, 0.33f, 1.0f);
            style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.27f, 0.35f, 0.42f, 1.0f);
            style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.31f, 0.41f, 0.50f, 1.0f);
            style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.28f, 0.34f, 1.0f);
            style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.29f, 0.37f, 0.45f, 1.0f);
            style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.44f, 0.53f, 1.0f);
            style.Colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.18f, 0.22f, 1.0f);
            style.Colors[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.28f, 0.34f, 1.0f);
            style.Colors[ImGuiCol_TabSelected] = ImVec4(0.24f, 0.30f, 0.37f, 1.0f);
            style.Colors[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.08f, 0.10f, 1.0f);
            style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
            style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
            style.Colors[ImGuiCol_Separator] = ImVec4(0.25f, 0.30f, 0.36f, 1.0f);
            style.Colors[ImGuiCol_CheckMark] = ImVec4(0.90f, 0.67f, 0.26f, 1.0f);
            style.Colors[ImGuiCol_DockingPreview] = ImVec4(0.90f, 0.67f, 0.26f, 0.65f);
        }

        resource::MaterialAsset CloneMaterialAsset(const resource::MaterialAsset& asset)
        {
            return asset;
        }

        DirectX::BoundingSphere BuildPickBounds(ecs::World& world, const ecs::EntityId entity, const DirectX::XMMATRIX& worldMatrix)
        {
            DirectX::BoundingSphere result{};

            DirectX::XMVECTOR scaleVector = DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
            DirectX::XMVECTOR rotationVector = DirectX::XMQuaternionIdentity();
            DirectX::XMVECTOR translationVector = DirectX::XMVectorZero();
            if (!DirectX::XMMatrixDecompose(&scaleVector, &rotationVector, &translationVector, worldMatrix))
            {
                translationVector = DirectX::XMVectorZero();
            }

            DirectX::XMFLOAT3 worldScale{};
            DirectX::XMStoreFloat3(&worldScale, scaleVector);
            worldScale.x = std::max(std::abs(worldScale.x), 0.001f);
            worldScale.y = std::max(std::abs(worldScale.y), 0.001f);
            worldScale.z = std::max(std::abs(worldScale.z), 0.001f);

            DirectX::XMFLOAT3 center{};
            DirectX::XMStoreFloat3(&center, translationVector);
            float radius = std::max({worldScale.x, worldScale.y, worldScale.z}) * kDefaultRenderableRadius;

            if (const auto* collider = world.TryGet<ecs::components::ColliderComponent>(entity); collider != nullptr)
            {
                const DirectX::XMVECTOR colliderCenter = DirectX::XMVector3TransformCoord(
                    DirectX::XMVectorSet(collider->offset.x, collider->offset.y, collider->offset.z, 1.0f),
                    worldMatrix);
                DirectX::XMStoreFloat3(&center, colliderCenter);

                if (collider->type == ecs::components::ColliderType::Sphere)
                {
                    radius = collider->radius * std::max({worldScale.x, worldScale.y, worldScale.z});
                }
                else
                {
                    const float extentX = collider->halfExtents.x * worldScale.x;
                    const float extentY = collider->halfExtents.y * worldScale.y;
                    const float extentZ = collider->halfExtents.z * worldScale.z;
                    radius = std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ);
                }
            }

            result.Center = center;
            result.Radius = std::max(radius, 0.05f);
            return result;
        }

        bool IntersectRayWithLocalAabb(
            const DirectX::XMVECTOR& rayOriginLocal,
            const DirectX::XMVECTOR& rayDirectionLocal,
            const DirectX::XMFLOAT3& minBounds,
            const DirectX::XMFLOAT3& maxBounds,
            float& outDistance)
        {
            DirectX::XMFLOAT3 origin{};
            DirectX::XMFLOAT3 direction{};
            DirectX::XMStoreFloat3(&origin, rayOriginLocal);
            DirectX::XMStoreFloat3(&direction, rayDirectionLocal);

            float tMin = 0.0f;
            float tMax = std::numeric_limits<float>::max();

            const auto testAxis =
                [&](const float originAxis, const float directionAxis, const float minAxis, const float maxAxis)
                {
                    if (std::abs(directionAxis) <= 1e-6f)
                    {
                        return originAxis >= minAxis && originAxis <= maxAxis;
                    }

                    const float inverseDirection = 1.0f / directionAxis;
                    float t1 = (minAxis - originAxis) * inverseDirection;
                    float t2 = (maxAxis - originAxis) * inverseDirection;
                    if (t1 > t2)
                    {
                        std::swap(t1, t2);
                    }

                    tMin = std::max(tMin, t1);
                    tMax = std::min(tMax, t2);
                    return tMin <= tMax;
                };

            if (!testAxis(origin.x, direction.x, minBounds.x, maxBounds.x) ||
                !testAxis(origin.y, direction.y, minBounds.y, maxBounds.y) ||
                !testAxis(origin.z, direction.z, minBounds.z, maxBounds.z))
            {
                return false;
            }

            outDistance = tMin >= 0.0f ? tMin : tMax;
            return outDistance >= 0.0f;
        }

        bool IntersectRayWithEntity(
            ecs::World& world,
            const ecs::EntityId entity,
            const DirectX::XMVECTOR& rayOriginWorld,
            const DirectX::XMVECTOR& rayDirectionWorld,
            const DirectX::XMMATRIX& worldMatrix,
            float& outDistance)
        {
            if (const auto* collider = world.TryGet<ecs::components::ColliderComponent>(entity); collider != nullptr)
            {
                if (collider->type == ecs::components::ColliderType::Sphere)
                {
                    const DirectX::BoundingSphere bounds = BuildPickBounds(world, entity, worldMatrix);
                    return bounds.Intersects(rayOriginWorld, rayDirectionWorld, outDistance);
                }

                DirectX::XMVECTOR determinant = DirectX::XMVectorZero();
                const DirectX::XMMATRIX inverseWorld = DirectX::XMMatrixInverse(&determinant, worldMatrix);
                if (DirectX::XMVector3NearEqual(determinant, DirectX::XMVectorZero(), DirectX::XMVectorReplicate(1e-6f)))
                {
                    return false;
                }

                const DirectX::XMVECTOR rayOriginLocal = DirectX::XMVector3TransformCoord(rayOriginWorld, inverseWorld);
                const DirectX::XMVECTOR rayDirectionLocal =
                    DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(rayDirectionWorld, inverseWorld));

                const DirectX::XMFLOAT3 minBounds{
                    collider->offset.x - collider->halfExtents.x,
                    collider->offset.y - collider->halfExtents.y,
                    collider->offset.z - collider->halfExtents.z,
                };
                const DirectX::XMFLOAT3 maxBounds{
                    collider->offset.x + collider->halfExtents.x,
                    collider->offset.y + collider->halfExtents.y,
                    collider->offset.z + collider->halfExtents.z,
                };

                float localDistance = 0.0f;
                if (!IntersectRayWithLocalAabb(rayOriginLocal, rayDirectionLocal, minBounds, maxBounds, localDistance))
                {
                    return false;
                }

                const DirectX::XMVECTOR localHitPoint =
                    DirectX::XMVectorAdd(rayOriginLocal, DirectX::XMVectorScale(rayDirectionLocal, localDistance));
                const DirectX::XMVECTOR worldHitPoint = DirectX::XMVector3TransformCoord(localHitPoint, worldMatrix);
                const DirectX::XMVECTOR deltaWorld = DirectX::XMVectorSubtract(worldHitPoint, rayOriginWorld);
                outDistance = DirectX::XMVectorGetX(DirectX::XMVector3Length(deltaWorld));
                return true;
            }

            const DirectX::BoundingSphere bounds = BuildPickBounds(world, entity, worldMatrix);
            return bounds.Intersects(rayOriginWorld, rayDirectionWorld, outDistance);
        }

        bool BuildScreenRay(
            const ImVec2& mousePosition,
            const editor::ViewportRect& renderRect,
            const DirectX::XMMATRIX& viewMatrix,
            const DirectX::XMMATRIX& projectionMatrix,
            DirectX::XMVECTOR& outOrigin,
            DirectX::XMVECTOR& outDirection)
        {
            if (!renderRect.IsValid())
            {
                return false;
            }

            const float normalizedX = (mousePosition.x - renderRect.x) / std::max(renderRect.width, 1.0f);
            const float normalizedY = (mousePosition.y - renderRect.y) / std::max(renderRect.height, 1.0f);
            const float ndcX = normalizedX * 2.0f - 1.0f;
            const float ndcY = 1.0f - normalizedY * 2.0f;

            DirectX::XMVECTOR determinant = DirectX::XMVectorZero();
            const DirectX::XMMATRIX inverseViewProjection =
                DirectX::XMMatrixInverse(&determinant, viewMatrix * projectionMatrix);
            if (DirectX::XMVector3NearEqual(determinant, DirectX::XMVectorZero(), DirectX::XMVectorReplicate(1e-6f)))
            {
                return false;
            }

            const DirectX::XMVECTOR nearPoint = DirectX::XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
            const DirectX::XMVECTOR farPoint = DirectX::XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
            outOrigin = DirectX::XMVector3TransformCoord(nearPoint, inverseViewProjection);
            const DirectX::XMVECTOR farWorld = DirectX::XMVector3TransformCoord(farPoint, inverseViewProjection);
            outDirection = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(farWorld, outOrigin));
            return true;
        }
    }

    SceneEditor::SceneEditor() = default;
    SceneEditor::~SceneEditor() = default;

    void SceneEditor::Initialize(SceneEditorServices services)
    {
        services_ = std::move(services);
        history_ = std::make_unique<editor::EditorCommandHistory>();
        gizmo_ = std::make_unique<editor::TransformGizmo>();
        history_->Clear();
        pendingSceneMutationSnapshot_.clear();
        pendingGizmoMutationSnapshot_.clear();
        gizmoWasUsing_ = false;
        initialized_ = true;
    }

    void SceneEditor::Shutdown()
    {
        if (history_ != nullptr)
        {
            history_->Clear();
        }
        gizmo_.reset();
        history_.reset();
        pendingSceneMutationSnapshot_.clear();
        pendingGizmoMutationSnapshot_.clear();
        gizmoWasUsing_ = false;
        initialized_ = false;
    }

    void SceneEditor::BuildWindowUi(const SceneEditorWindowContext& windowContext)
    {
        if (!initialized_ || services_.world == nullptr || services_.resourceManager == nullptr)
        {
            return;
        }

        ValidateSelection();
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        windowState.viewportHovered = false;
        windowState.viewportFocused = false;
        windowState.viewportAcceptsCameraNavigation = false;
        windowState.gizmoHovered = false;
        windowState.gizmoActive = false;
        windowState.viewport = {};
        windowState.viewportToolbar = {};
        windowState.materialPreviewEnabled = false;
        windowState.materialPreviewMaterialPath.clear();

        HandleKeyboardShortcuts(windowContext);
        BuildDockSpace(windowContext);
        BuildHierarchyPanel(windowContext);
        BuildInspectorPanel(windowContext);
        BuildStatisticsPanel(windowContext);
        BuildViewportPanel(windowContext);
        DrawSceneVisibilityMask(windowState.viewport);
        BuildMaterialEditorPanel(windowContext);
        BuildAssetBrowserPanel(windowContext);

        if (editorState.showImGuiDemo)
        {
            ImGui::ShowDemoWindow(&editorState.showImGuiDemo);
        }
    }

    bool SceneEditor::CanStartSceneNavigation(const core::WindowId windowId, const float mouseX, const float mouseY) const
    {
        const auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        const auto* windowState = editorState.FindWindowState(windowId);
        if (windowState == nullptr || !windowState->viewport.IsValid())
        {
            return false;
        }

        const bool insideViewport =
            mouseX >= windowState->viewport.x &&
            mouseX <= windowState->viewport.x + windowState->viewport.width &&
            mouseY >= windowState->viewport.y &&
            mouseY <= windowState->viewport.y + windowState->viewport.height;
        if (!insideViewport)
        {
            return false;
        }

        const bool insideToolbar =
            windowState->viewportToolbar.IsValid() &&
            mouseX >= windowState->viewportToolbar.x &&
            mouseX <= windowState->viewportToolbar.x + windowState->viewportToolbar.width &&
            mouseY >= windowState->viewportToolbar.y &&
            mouseY <= windowState->viewportToolbar.y + windowState->viewportToolbar.height;

        return !insideToolbar && !windowState->gizmoHovered && !windowState->gizmoActive;
    }

    void SceneEditor::BuildDockSpace(const SceneEditorWindowContext& windowContext)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        const ImGuiDockNodeFlags dockSpaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
        const ImGuiID dockspaceId = ImGui::GetID("MyEngineEditorDockSpace");
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport(), dockSpaceFlags);
        ImGui::PopStyleColor(3);

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        if (!windowState.dockLayoutInitialized)
        {
            CreateDefaultDockLayout(windowContext);
            windowState.dockLayoutInitialized = true;
        }

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, editorState.mode == editor::RuntimeMode::Edit) && services_.saveScene)
                {
                    if (services_.saveScene())
                    {
                        editorState.sceneDirty = false;
                    }
                }

                if (ImGui::MenuItem("Load Scene", "Ctrl+O", false, editorState.mode == editor::RuntimeMode::Edit) && services_.loadScene)
                {
                    if (services_.loadScene())
                    {
                        editorState.selectedEntity = ecs::kInvalidEntity;
                        editorState.sceneDirty = false;
                        history_->Clear();
                    }
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Quit") && services_.requestQuit)
                {
                    services_.requestQuit();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit"))
            {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, history_ != nullptr && history_->CanUndo() && editorState.mode == editor::RuntimeMode::Edit))
                {
                    history_->Undo();
                }

                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, history_ != nullptr && history_->CanRedo() && editorState.mode == editor::RuntimeMode::Edit))
                {
                    history_->Redo();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View"))
            {
                ImGui::MenuItem(kHierarchyWindowName, nullptr, &editorState.showHierarchy);
                ImGui::MenuItem(kInspectorWindowName, nullptr, &editorState.showInspector);
                ImGui::MenuItem(kStatisticsWindowName, nullptr, &editorState.showStatistics);
                ImGui::MenuItem(kViewportWindowName, nullptr, &editorState.showViewport);
                ImGui::MenuItem(kMaterialEditorWindowName, nullptr, &editorState.showMaterialEditor);
                ImGui::MenuItem(kAssetBrowserWindowName, nullptr, &editorState.showAssetBrowser);
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Help"))
            {
                ImGui::MenuItem("ImGui Demo", nullptr, &editorState.showImGuiDemo);
                ImGui::EndMenu();
            }

            ImGui::EndMainMenuBar();
        }
    }

    bool SceneEditor::BuildToolbar(const SceneEditorWindowContext& windowContext, const editor::ViewportRect& viewportRect)
    {
        (void)windowContext;

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();
        const float overlayWidth = std::min(std::max(viewportRect.width - kViewportToolbarPadding * 2.0f, 320.0f), 860.0f);
        const ImVec2 overlayPosition{viewportRect.x + kViewportToolbarPadding, viewportRect.y + kViewportToolbarPadding};

        ImGui::SetCursorScreenPos(overlayPosition);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.17f, 0.20f, 0.24f, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.24f, 0.29f, 0.34f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.28f, 0.34f, 0.40f, 0.98f));

        ImGui::BeginGroup();

        const bool isEditMode = editorState.mode == editor::RuntimeMode::Edit;
        ImGui::TextDisabled("%s", isEditMode ? "Edit" : "Play");
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::Button("Play"))
        {
            if (isEditMode && services_.captureSceneSnapshot)
            {
                editorState.playModeSnapshot = services_.captureSceneSnapshot();
                editorState.mode = editor::RuntimeMode::Play;
                physicsState.physicsPaused = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop") && editorState.mode == editor::RuntimeMode::Play)
        {
            const bool restoredScene =
                editorState.playModeSnapshot.empty() ||
                (services_.restoreSceneSnapshot != nullptr &&
                    services_.restoreSceneSnapshot(editorState.playModeSnapshot));

            if (restoredScene)
            {
                editorState.mode = editor::RuntimeMode::Edit;
                physicsState.physicsPaused = true;
                editorState.playModeSnapshot.clear();
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        const bool editEnabled = isEditMode;
        if (!editEnabled)
        {
            ImGui::BeginDisabled();
        }

        if (ImGui::RadioButton("Translate", editorState.gizmoOperation == editor::GizmoOperation::Translate))
        {
            editorState.gizmoOperation = editor::GizmoOperation::Translate;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", editorState.gizmoOperation == editor::GizmoOperation::Rotate))
        {
            editorState.gizmoOperation = editor::GizmoOperation::Rotate;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale", editorState.gizmoOperation == editor::GizmoOperation::Scale))
        {
            editorState.gizmoOperation = editor::GizmoOperation::Scale;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::RadioButton("Local", editorState.gizmoSpace == editor::GizmoSpace::Local))
        {
            editorState.gizmoSpace = editor::GizmoSpace::Local;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("World", editorState.gizmoSpace == editor::GizmoSpace::World))
        {
            editorState.gizmoSpace = editor::GizmoSpace::World;
        }

        if (!editEnabled)
        {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", editorState.sceneDirty ? "modified" : "saved");

        ImGui::EndGroup();

        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        (void)overlayWidth;

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
        return hovered;
    }

    void SceneEditor::BuildHierarchyPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showHierarchy)
        {
            return;
        }

        ecs::World& world = *services_.world;
        if (ImGui::Begin(kHierarchyWindowName))
        {
            const bool editEnabled = editorState.mode == editor::RuntimeMode::Edit;
            if (!editEnabled)
            {
                ImGui::BeginDisabled();
            }

            if (ImGui::Button("Create Empty"))
            {
                const std::string beforeSnapshot = services_.captureSceneSnapshot != nullptr ? services_.captureSceneSnapshot() : std::string();
                const ecs::EntityId entity = world.CreateEntity();
                world.Emplace<ecs::components::TagComponent>(entity).name = "Empty_" + std::to_string(entity);
                world.Emplace<ecs::components::TransformComponent>(entity);
                world.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = windowContext.windowId;
                editorState.selectedEntity = entity;
                RecordSceneMutationImmediate("Create Empty Entity", beforeSnapshot);
            }

            ImGui::SameLine();
            if (ImGui::Button("Delete Selected") && editorState.selectedEntity != ecs::kInvalidEntity)
            {
                DeleteSelectedEntity();
            }

            if (!editEnabled)
            {
                ImGui::EndDisabled();
            }

            ImGui::Separator();

            const auto visibleEntities = CollectVisibleEntities(world, windowContext.windowId);
            std::vector<ecs::EntityId> rootEntities;
            rootEntities.reserve(visibleEntities.size());

            for (const ecs::EntityId entity : visibleEntities)
            {
                const auto* hierarchy = world.TryGet<ecs::components::HierarchyComponent>(entity);
                const bool hasVisibleParent =
                    hierarchy != nullptr &&
                    hierarchy->parent != ecs::kInvalidEntity &&
                    world.IsAlive(hierarchy->parent) &&
                    MatchesWindowBinding(world, hierarchy->parent, windowContext.windowId);
                if (!hasVisibleParent)
                {
                    rootEntities.push_back(entity);
                }
            }

            for (const ecs::EntityId entity : rootEntities)
            {
                DrawEntityNode(windowContext.windowId, entity);
            }
        }
        ImGui::End();
    }

    void SceneEditor::BuildInspectorPanel(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showInspector)
        {
            return;
        }

        ecs::World& world = *services_.world;
        if (ImGui::Begin(kInspectorWindowName))
        {
            const ecs::EntityId entity = editorState.selectedEntity;
            if (entity == ecs::kInvalidEntity || !world.IsAlive(entity))
            {
                ImGui::TextDisabled("Select an entity from the hierarchy.");
                ImGui::End();
                return;
            }

            ImGui::Text("Entity %u", entity);
            ImGui::Separator();

            const bool editEnabled = editorState.mode == editor::RuntimeMode::Edit;
            if (!editEnabled)
            {
                ImGui::BeginDisabled();
            }

            if (auto* tag = world.TryGet<ecs::components::TagComponent>(entity); tag != nullptr)
            {
                if (ImGui::CollapsingHeader("Tag", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::InputText("Name", &tag->name);
                    RecordSceneMutationFromItem("Rename Entity");
                }
            }

            if (auto* transform = world.TryGet<ecs::components::TransformComponent>(entity); transform != nullptr)
            {
                if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    auto position = ToFloat3(transform->position);
                    if (ImGui::DragFloat3("Position", position.data(), 0.05f))
                    {
                        FromFloat3(position, transform->position);
                    }
                    RecordSceneMutationFromItem("Edit Transform Position");

                    auto rotation = ToFloat3(transform->rotationDeg);
                    if (ImGui::DragFloat3("Rotation", rotation.data(), 0.5f))
                    {
                        FromFloat3(rotation, transform->rotationDeg);
                    }
                    RecordSceneMutationFromItem("Edit Transform Rotation");

                    auto scale = ToFloat3(transform->scale);
                    if (ImGui::DragFloat3("Scale", scale.data(), 0.02f, 0.01f, 200.0f))
                    {
                        FromFloat3(scale, transform->scale);
                    }
                    RecordSceneMutationFromItem("Edit Transform Scale");
                }
            }

            if (auto* renderer = world.TryGet<ecs::components::MeshRendererComponent>(entity); renderer != nullptr)
            {
                if (ImGui::CollapsingHeader("MeshRenderer", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    const auto meshKeys = services_.resourceManager->GetKnownMeshKeys();
                    const auto materialKeys = services_.resourceManager->GetKnownMaterialKeys();

                    if (ImGui::BeginCombo("Mesh", FileNameLabel(renderer->meshPath).c_str()))
                    {
                        for (const auto& meshKey : meshKeys)
                        {
                            const bool selected = meshKey == renderer->meshPath;
                            if (ImGui::Selectable(FileNameLabel(meshKey).c_str(), selected))
                            {
                                const std::string beforeMeshSnapshot = CaptureSceneSnapshot();
                                renderer->meshPath = meshKey;
                                RecordSceneMutationImmediate("Change Mesh", beforeMeshSnapshot);
                            }
                            if (selected)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    if (ImGui::BeginCombo("Material", FileNameLabel(renderer->materialPath).c_str()))
                    {
                        for (const auto& materialKey : materialKeys)
                        {
                            const bool selected = materialKey == renderer->materialPath;
                            if (ImGui::Selectable(FileNameLabel(materialKey).c_str(), selected))
                            {
                                const std::string beforeMaterialSnapshot = CaptureSceneSnapshot();
                                renderer->materialPath = materialKey;
                                RecordSceneMutationImmediate("Change Material", beforeMaterialSnapshot);
                            }
                            if (selected)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    if (auto materialResource = services_.resourceManager->Load<resource::MaterialAsset>(renderer->materialPath);
                        materialResource != nullptr)
                    {
                        ImGui::TextDisabled("Shared material asset");
                        ImGui::TextWrapped("%s", renderer->materialPath.c_str());

                        resource::MaterialAsset beforeAsset = CloneMaterialAsset(materialResource->asset);
                        auto textureKeys = services_.resourceManager->GetKnownTextureKeys();
                        if (ImGui::BeginCombo("Texture", FileNameLabel(materialResource->asset.texturePath).c_str()))
                        {
                            for (const auto& textureKey : textureKeys)
                            {
                                const bool selected = textureKey == materialResource->asset.texturePath;
                                if (ImGui::Selectable(FileNameLabel(textureKey).c_str(), selected))
                                {
                                    materialResource->asset.texturePath = textureKey;
                                    services_.resourceManager->Load<resource::TextureAsset>(textureKey);
                                }
                                if (selected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                        if ((ImGui::IsItemDeactivatedAfterEdit() || !ImGui::IsItemActive()) &&
                            !MaterialEquals(beforeAsset, materialResource->asset))
                        {
                            PushMaterialAssetCommand(
                                "Change Renderer Texture",
                                renderer->materialPath,
                                beforeAsset,
                                CloneMaterialAsset(materialResource->asset));
                        }

                        beforeAsset = CloneMaterialAsset(materialResource->asset);
                        float tint[4]{
                            materialResource->asset.tint.r,
                            materialResource->asset.tint.g,
                            materialResource->asset.tint.b,
                            materialResource->asset.tint.a,
                        };
                        if (ImGui::ColorEdit4("Tint", tint))
                        {
                            materialResource->asset.tint = {tint[0], tint[1], tint[2], tint[3]};
                        }
                        if ((ImGui::IsItemDeactivatedAfterEdit() || !ImGui::IsItemActive()) &&
                            !MaterialEquals(beforeAsset, materialResource->asset))
                        {
                            PushMaterialAssetCommand(
                                "Change Renderer Tint",
                                renderer->materialPath,
                                beforeAsset,
                                CloneMaterialAsset(materialResource->asset));
                        }

                        if (ImGui::Button("Open Material Editor"))
                        {
                            editorState.showMaterialEditor = true;
                        }
                    }

                    bool visible = renderer->visible;
                    if (ImGui::Checkbox("Visible", &visible))
                    {
                        const std::string beforeVisibilitySnapshot = CaptureSceneSnapshot();
                        renderer->visible = visible;
                        RecordSceneMutationImmediate("Toggle Renderer Visibility", beforeVisibilitySnapshot);
                    }
                    ImGui::TextDisabled("%s", renderer->materialPath.c_str());
                }
            }

            if (auto* rigidbody = world.TryGet<ecs::components::RigidbodyComponent>(entity); rigidbody != nullptr)
            {
                if (ImGui::CollapsingHeader("Rigidbody", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    bool useGravity = rigidbody->useGravity;
                    if (ImGui::Checkbox("Use Gravity", &useGravity))
                    {
                        const std::string beforeGravitySnapshot = CaptureSceneSnapshot();
                        rigidbody->useGravity = useGravity;
                        RecordSceneMutationImmediate("Toggle Rigidbody Gravity", beforeGravitySnapshot);
                    }

                    bool isKinematic = rigidbody->isKinematic;
                    if (ImGui::Checkbox("Is Kinematic", &isKinematic))
                    {
                        const std::string beforeKinematicSnapshot = CaptureSceneSnapshot();
                        rigidbody->isKinematic = isKinematic;
                        RecordSceneMutationImmediate("Toggle Rigidbody Kinematic", beforeKinematicSnapshot);
                    }

                    auto velocity = ToFloat3(rigidbody->velocity);
                    ImGui::BeginDisabled();
                    ImGui::DragFloat3("Velocity", velocity.data(), 0.0f);
                    ImGui::EndDisabled();
                }
            }

            if (auto* collider = world.TryGet<ecs::components::ColliderComponent>(entity); collider != nullptr)
            {
                if (ImGui::CollapsingHeader("BoxCollider", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    int type = collider->type == ecs::components::ColliderType::Sphere ? 1 : 0;
                    if (ImGui::Combo("Type", &type, "Box\0Sphere\0"))
                    {
                        const std::string beforeTypeSnapshot = CaptureSceneSnapshot();
                        collider->type = type == 1 ? ecs::components::ColliderType::Sphere : ecs::components::ColliderType::Box;
                        RecordSceneMutationImmediate("Change Collider Type", beforeTypeSnapshot);
                    }

                    if (collider->type == ecs::components::ColliderType::Box)
                    {
                        auto halfExtents = ToFloat3(collider->halfExtents);
                        if (ImGui::DragFloat3("Half Extents", halfExtents.data(), 0.02f, 0.01f, 100.0f))
                        {
                            FromFloat3(halfExtents, collider->halfExtents);
                        }
                        RecordSceneMutationFromItem("Edit Collider Extents");
                    }
                    else
                    {
                        float radius = collider->radius;
                        if (ImGui::DragFloat("Radius", &radius, 0.02f, 0.01f, 100.0f))
                        {
                            collider->radius = radius;
                        }
                        RecordSceneMutationFromItem("Edit Collider Radius");
                    }

                    auto offset = ToFloat3(collider->offset);
                    if (ImGui::DragFloat3("Offset", offset.data(), 0.02f))
                    {
                        FromFloat3(offset, collider->offset);
                    }
                    RecordSceneMutationFromItem("Edit Collider Offset");

                    float friction = collider->friction;
                    if (ImGui::DragFloat("Friction", &friction, 0.01f, 0.0f, 2.0f))
                    {
                        collider->friction = friction;
                    }
                    RecordSceneMutationFromItem("Edit Collider Friction");

                    float bounciness = collider->bounciness;
                    if (ImGui::DragFloat("Bounciness", &bounciness, 0.01f, 0.0f, 2.0f))
                    {
                        collider->bounciness = bounciness;
                    }
                    RecordSceneMutationFromItem("Edit Collider Bounciness");

                    bool isTrigger = collider->isTrigger;
                    if (ImGui::Checkbox("Trigger", &isTrigger))
                    {
                        const std::string beforeTriggerSnapshot = CaptureSceneSnapshot();
                        collider->isTrigger = isTrigger;
                        RecordSceneMutationImmediate("Toggle Collider Trigger", beforeTriggerSnapshot);
                    }
                }
            }

            if (auto* camera = world.TryGet<ecs::components::CameraComponent>(entity); camera != nullptr)
            {
                if (ImGui::CollapsingHeader("Camera"))
                {
                    auto position = ToFloat3(camera->position);
                    if (ImGui::DragFloat3("Camera Position", position.data(), 0.05f))
                    {
                        FromFloat3(position, camera->position);
                    }
                    RecordSceneMutationFromItem("Edit Camera Position");

                    auto rotation = ToFloat3(camera->rotationDeg);
                    if (ImGui::DragFloat3("Camera Rotation", rotation.data(), 0.5f))
                    {
                        FromFloat3(rotation, camera->rotationDeg);
                    }
                    RecordSceneMutationFromItem("Edit Camera Rotation");

                    float fovYDeg = camera->fovYDeg;
                    if (ImGui::DragFloat("FOV", &fovYDeg, 0.2f, 15.0f, 160.0f))
                    {
                        camera->fovYDeg = fovYDeg;
                    }
                    RecordSceneMutationFromItem("Edit Camera FOV");

                    bool isPrimary = camera->isPrimary;
                    if (ImGui::Checkbox("Primary", &isPrimary))
                    {
                        const std::string beforePrimarySnapshot = CaptureSceneSnapshot();
                        camera->isPrimary = isPrimary;
                        RecordSceneMutationImmediate("Toggle Camera Primary", beforePrimarySnapshot);
                    }
                }
            }

            if (!editEnabled)
            {
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }

    void SceneEditor::BuildStatisticsPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showStatistics)
        {
            return;
        }

        const auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        const auto& physicsState = core::ServiceLocator::GetPhysicsWorldState();

        if (ImGui::Begin(kStatisticsWindowName))
        {
            ImGui::Text("Mode: %s", editorState.mode == editor::RuntimeMode::Edit ? "Edit" : "Play");
            ImGui::Text("FPS: %.1f", windowState.timings.averageFps);
            ImGui::Text("Frame: %.2f ms", windowState.timings.frameMs);
            ImGui::Text("Render: %.2f ms", windowState.timings.renderMs);
            ImGui::Text("World update: %.2f ms", windowState.timings.worldUpdateMs);
            ImGui::Separator();
            ImGui::Text("Entities: %u", windowState.renderStats.totalEntities);
            ImGui::Text("Renderable: %u", windowState.renderStats.renderableEntities);
            ImGui::Text("Drawn: %u", windowState.renderStats.renderedEntities);
            ImGui::Text("Active collisions: %u", physicsState.stats.collisionPairs);
            ImGui::Text("Resource memory: %s", FormatBytes(windowState.renderStats.resourceMemoryBytes).c_str());
        }
        ImGui::End();
    }

    void SceneEditor::BuildViewportPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showViewport)
        {
            return;
        }

        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBackground;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin(kViewportWindowName, nullptr, flags))
        {
            const ImVec2 contentMin = ImGui::GetCursorScreenPos();
            const ImVec2 contentSize = ImGui::GetContentRegionAvail();
            windowState.viewport = {
                contentMin.x,
                contentMin.y,
                std::max(contentSize.x, 1.0f),
                std::max(contentSize.y, 1.0f),
            };
            const float toolbarWidth =
                std::min(std::max(windowState.viewport.width - kViewportToolbarPadding * 2.0f, 320.0f), 860.0f);
            const ImVec2 toolbarMin{
                windowState.viewport.x + kViewportToolbarPadding,
                windowState.viewport.y + kViewportToolbarPadding,
            };
            const ImVec2 toolbarMax{
                toolbarMin.x + toolbarWidth,
                toolbarMin.y + kViewportToolbarHeight,
            };
            windowState.viewportToolbar = {
                toolbarMin.x,
                toolbarMin.y,
                toolbarWidth,
                kViewportToolbarHeight,
            };
            const ImVec2 mousePosition = ImGui::GetIO().MousePos;
            const bool mouseInToolbarRegion =
                mousePosition.x >= toolbarMin.x &&
                mousePosition.x <= toolbarMax.x &&
                mousePosition.y >= toolbarMin.y &&
                mousePosition.y <= toolbarMax.y;

            ImGui::ItemSize(ImVec2(windowState.viewport.width, windowState.viewport.height), 0.0f);
            const ImRect canvasRect(
                ImVec2(windowState.viewport.x, windowState.viewport.y),
                ImVec2(windowState.viewport.x + windowState.viewport.width, windowState.viewport.y + windowState.viewport.height));
            const bool canvasHovered =
                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                canvasRect.Contains(mousePosition);
            const bool canvasLeftClicked =
                canvasHovered &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            windowState.viewportHovered = canvasHovered;
            windowState.viewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

            if (ImGui::BeginDragDropTargetCustom(canvasRect, ImGui::GetID("##viewport_canvas_target")))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMeshPayloadType))
                {
                    const std::string beforeSnapshot = services_.captureSceneSnapshot();
                    const std::string meshPath(static_cast<const char*>(payload->Data), payload->DataSize - 1u);
                    const std::string materialPath = ResolveSuggestedMaterialForMesh(meshPath);
                    SpawnRenderableEntity(windowContext.windowId, meshPath, materialPath);
                    RecordSceneMutationImmediate("Spawn Mesh From Asset Browser", beforeSnapshot);
                }

                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMaterialPayloadType))
                {
                    const std::string materialPath(static_cast<const char*>(payload->Data), payload->DataSize - 1u);
                    ecs::EntityId targetEntity = PickEntityAtScreenPosition(
                        windowContext,
                        windowState,
                        mousePosition);
                    if (targetEntity == ecs::kInvalidEntity)
                    {
                        targetEntity = editorState.selectedEntity;
                    }

                    if (targetEntity != ecs::kInvalidEntity)
                    {
                        if (auto* renderer = services_.world->TryGet<ecs::components::MeshRendererComponent>(targetEntity);
                            renderer != nullptr && renderer->materialPath != materialPath)
                        {
                            const std::string beforeSnapshot = services_.captureSceneSnapshot();
                            renderer->materialPath = materialPath;
                            editorState.selectedEntity = targetEntity;
                            RecordSceneMutationImmediate("Assign Material From Asset Browser", beforeSnapshot);
                        }
                    }
                }

                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTexturePayloadType))
                {
                    const std::string beforeSnapshot = services_.captureSceneSnapshot();
                    const std::string texturePath(static_cast<const char*>(payload->Data), payload->DataSize - 1u);
                    const std::string materialPath = EnsureTexturePreviewMaterial(texturePath);
                    if (!materialPath.empty())
                    {
                        SpawnRenderableEntity(windowContext.windowId, kCubeMeshPath, materialPath);
                        RecordSceneMutationImmediate("Spawn Textured Cube From Asset Browser", beforeSnapshot);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            bool toolbarHovered = false;

            if (editorState.mode == editor::RuntimeMode::Edit &&
                editorState.selectedEntity != ecs::kInvalidEntity &&
                windowState.renderStats.camera.available)
            {
                const bool wasUsing = gizmoWasUsing_;
                const bool gizmoChanged = gizmo_->DrawAndHandle(editor::TransformGizmo::Context{
                    *services_.world,
                    editorState.selectedEntity,
                    windowContext.windowId,
                    windowState.viewport,
                    scene::ToDirectXMatrix(windowState.renderStats.camera.view),
                    scene::ToDirectXMatrix(windowState.renderStats.camera.projection),
                    editorState.gizmoOperation,
                    editorState.gizmoSpace,
                    !mouseInToolbarRegion,
                    services_.logger,
                });

                windowState.gizmoHovered = gizmo_->IsHovered();
                windowState.gizmoActive = gizmo_->IsUsing();
                if (gizmoChanged)
                {
                    editorState.sceneDirty = true;
                }

                if (!wasUsing && gizmo_->IsUsing() && pendingGizmoMutationSnapshot_.empty() && services_.captureSceneSnapshot != nullptr)
                {
                    pendingGizmoMutationSnapshot_ = services_.captureSceneSnapshot();
                }

                if (wasUsing && !gizmo_->IsUsing())
                {
                    CommitPendingGizmoMutation();
                }
                gizmoWasUsing_ = gizmo_->IsUsing();
            }

            toolbarHovered = BuildToolbar(windowContext, windowState.viewport);

            if (canvasLeftClicked &&
                editorState.mode == editor::RuntimeMode::Edit &&
                !mouseInToolbarRegion &&
                !toolbarHovered &&
                !windowState.gizmoHovered &&
                !windowState.gizmoActive)
            {
                editorState.selectedEntity = PickEntityAtScreenPosition(
                    windowContext,
                    windowState,
                    mousePosition);
            }

            windowState.viewportAcceptsCameraNavigation =
                canvasHovered &&
                !mouseInToolbarRegion &&
                !toolbarHovered &&
                !windowState.gizmoHovered &&
                !windowState.gizmoActive &&
                !ImGui::IsAnyItemActive();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void SceneEditor::BuildMaterialEditorPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showMaterialEditor)
        {
            return;
        }

        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);

        if (ImGui::Begin(kMaterialEditorWindowName))
        {
            if (editorState.selectedEntity == ecs::kInvalidEntity)
            {
                ImGui::TextDisabled("Select an entity with MeshRenderer.");
                ImGui::End();
                return;
            }

            if (!MatchesWindowBinding(*services_.world, editorState.selectedEntity, windowContext.windowId))
            {
                ImGui::TextDisabled("Selected entity belongs to another window.");
                ImGui::End();
                return;
            }

            auto* renderer = services_.world->TryGet<ecs::components::MeshRendererComponent>(editorState.selectedEntity);
            if (renderer == nullptr || renderer->materialPath.empty())
            {
                ImGui::TextDisabled("Selected entity has no material.");
                ImGui::End();
                return;
            }

            auto materialResource = services_.resourceManager->Load<resource::MaterialAsset>(renderer->materialPath);
            if (materialResource == nullptr)
            {
                ImGui::TextDisabled("Failed to load material.");
                ImGui::End();
                return;
            }

            ImGui::TextDisabled("%s", renderer->materialPath.c_str());
            ImGui::TextWrapped("Live preview is applied to all entities using this material.");
            ImGui::TextWrapped("A dedicated preview mesh is shown in the viewport overlay.");
            ImGui::Separator();

            int previewShape = windowState.materialPreviewShape == editor::MaterialPreviewShape::Cube ? 1 : 0;
            if (ImGui::Combo("Preview Mesh", &previewShape, "Sphere\0Cube\0"))
            {
                windowState.materialPreviewShape =
                    previewShape == 1 ? editor::MaterialPreviewShape::Cube : editor::MaterialPreviewShape::Sphere;
            }
            windowState.materialPreviewEnabled = true;
            windowState.materialPreviewMaterialPath = renderer->materialPath;

            const bool editEnabled = editorState.mode == editor::RuntimeMode::Edit;
            if (!editEnabled)
            {
                ImGui::BeginDisabled();
            }

            resource::MaterialAsset beforeAsset = CloneMaterialAsset(materialResource->asset);

            auto shaderKeys = services_.resourceManager->GetKnownShaderKeys();
            if (ImGui::BeginCombo("Shader", FileNameLabel(materialResource->asset.shaderPath).c_str()))
            {
                for (const auto& shaderKey : shaderKeys)
                {
                    const bool selected = shaderKey == materialResource->asset.shaderPath;
                    if (ImGui::Selectable(FileNameLabel(shaderKey).c_str(), selected))
                    {
                        materialResource->asset.shaderPath = shaderKey;
                        services_.resourceManager->Load<resource::ShaderAsset>(shaderKey);
                    }
                }
                ImGui::EndCombo();
            }
            if ((ImGui::IsItemDeactivatedAfterEdit() || !ImGui::IsItemActive()) && !MaterialEquals(beforeAsset, materialResource->asset))
            {
                PushMaterialAssetCommand(
                    "Change Material Shader",
                    renderer->materialPath,
                    beforeAsset,
                    CloneMaterialAsset(materialResource->asset));
            }

            beforeAsset = CloneMaterialAsset(materialResource->asset);
            auto textureKeys = services_.resourceManager->GetKnownTextureKeys();
            if (ImGui::BeginCombo("Texture", FileNameLabel(materialResource->asset.texturePath).c_str()))
            {
                for (const auto& textureKey : textureKeys)
                {
                    const bool selected = textureKey == materialResource->asset.texturePath;
                    if (ImGui::Selectable(FileNameLabel(textureKey).c_str(), selected))
                    {
                        materialResource->asset.texturePath = textureKey;
                        services_.resourceManager->Load<resource::TextureAsset>(textureKey);
                    }
                }
                ImGui::EndCombo();
            }
            if ((ImGui::IsItemDeactivatedAfterEdit() || !ImGui::IsItemActive()) && !MaterialEquals(beforeAsset, materialResource->asset))
            {
                PushMaterialAssetCommand(
                    "Change Material Texture",
                    renderer->materialPath,
                    beforeAsset,
                    CloneMaterialAsset(materialResource->asset));
            }

            beforeAsset = CloneMaterialAsset(materialResource->asset);
            float tint[4]{
                materialResource->asset.tint.r,
                materialResource->asset.tint.g,
                materialResource->asset.tint.b,
                materialResource->asset.tint.a,
            };
            if (ImGui::ColorEdit4("Tint", tint))
            {
                materialResource->asset.tint = {tint[0], tint[1], tint[2], tint[3]};
            }
            if ((ImGui::IsItemDeactivatedAfterEdit() || !ImGui::IsItemActive()) && !MaterialEquals(beforeAsset, materialResource->asset))
            {
                PushMaterialAssetCommand(
                    "Change Material Tint",
                    renderer->materialPath,
                    beforeAsset,
                    CloneMaterialAsset(materialResource->asset));
            }

            ImGui::TextDisabled("Preview target: %s", PreviewMeshPath(windowState.materialPreviewShape));

            if (!editEnabled)
            {
                ImGui::EndDisabled();
            }
        }
        ImGui::End();
    }

    void SceneEditor::BuildAssetBrowserPanel(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showAssetBrowser)
        {
            return;
        }

        if (ImGui::Begin(kAssetBrowserWindowName))
        {
            if (ImGui::BeginTabBar("asset_tabs"))
            {
                const auto drawAssetList =
                    [](const char* payloadType, const std::vector<std::string>& assets)
                    {
                        for (const auto& asset : assets)
                        {
                            ImGui::Selectable(FileNameLabel(asset).c_str(), false);
                            if (ImGui::BeginDragDropSource())
                            {
                                ImGui::SetDragDropPayload(payloadType, asset.c_str(), asset.size() + 1u);
                                ImGui::TextUnformatted(asset.c_str());
                                ImGui::EndDragDropSource();
                            }
                        }
                    };

                if (ImGui::BeginTabItem("Meshes"))
                {
                    drawAssetList(kMeshPayloadType, services_.resourceManager->GetKnownMeshKeys());
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Materials"))
                {
                    drawAssetList(kMaterialPayloadType, services_.resourceManager->GetKnownMaterialKeys());
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Textures"))
                {
                    drawAssetList(kTexturePayloadType, services_.resourceManager->GetKnownTextureKeys());
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Shaders"))
                {
                    const auto shaders = services_.resourceManager->GetKnownShaderKeys();
                    for (const auto& shader : shaders)
                    {
                        ImGui::BulletText("%s", shader.c_str());
                    }
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::Separator();
            ImGui::TextDisabled("Drag meshes or textures to the viewport to create entities.");
        }
        ImGui::End();
    }

    void SceneEditor::HandleKeyboardShortcuts(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        if (!initialized_)
        {
            return;
        }

        ImGuiIO& io = ImGui::GetIO();
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && services_.saveScene && editorState.mode == editor::RuntimeMode::Edit)
        {
            if (services_.saveScene())
            {
                editorState.sceneDirty = false;
            }
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false) && services_.loadScene && editorState.mode == editor::RuntimeMode::Edit)
        {
            if (services_.loadScene())
            {
                if (history_ != nullptr)
                {
                    history_->Clear();
                }
                editorState.selectedEntity = ecs::kInvalidEntity;
                editorState.sceneDirty = false;
            }
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && history_ != nullptr && history_->CanUndo() && editorState.mode == editor::RuntimeMode::Edit)
        {
            history_->Undo();
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && history_ != nullptr && history_->CanRedo() && editorState.mode == editor::RuntimeMode::Edit)
        {
            history_->Redo();
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && editorState.mode == editor::RuntimeMode::Edit)
        {
            DeleteSelectedEntity();
        }
    }

    void SceneEditor::ValidateSelection() const
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.selectedEntity != ecs::kInvalidEntity && !services_.world->IsAlive(editorState.selectedEntity))
        {
            editorState.selectedEntity = ecs::kInvalidEntity;
        }
    }

    void SceneEditor::CreateDefaultDockLayout(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        ImGuiID dockspaceId = ImGui::GetID("MyEngineEditorDockSpace");
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID dockLeft = 0;
        ImGuiID dockRight = 0;
        ImGuiID dockBottom = 0;
        ImGuiID dockCenter = dockspaceId;

        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Left, 0.22f, &dockLeft, &dockCenter);
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.28f, &dockRight, &dockCenter);
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.28f, &dockBottom, &dockCenter);

        ImGui::DockBuilderDockWindow(kHierarchyWindowName, dockLeft);
        ImGui::DockBuilderDockWindow(kInspectorWindowName, dockRight);
        ImGui::DockBuilderDockWindow(kMaterialEditorWindowName, dockRight);
        ImGui::DockBuilderDockWindow(kAssetBrowserWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kStatisticsWindowName, dockBottom);
        ImGui::DockBuilderDockWindow(kViewportWindowName, dockCenter);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    void SceneEditor::DrawEntityNode(const core::WindowId windowId, const ecs::EntityId entity)
    {
        ecs::World& world = *services_.world;
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();

        const auto* hierarchy = world.TryGet<ecs::components::HierarchyComponent>(entity);
        const bool hasChildren = hierarchy != nullptr && !hierarchy->children.empty();

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_SpanAvailWidth |
            (editorState.selectedEntity == entity ? ImGuiTreeNodeFlags_Selected : 0) |
            (!hasChildren ? ImGuiTreeNodeFlags_Leaf : 0);

        const bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(entity)),
            flags,
            "%s",
            EntityDisplayName(world, entity).c_str());

        if (ImGui::IsItemClicked())
        {
            editorState.selectedEntity = entity;
            editorState.lastInteractedWindowId = windowId;
        }

        if (open)
        {
            if (hierarchy != nullptr)
            {
                std::vector<ecs::EntityId> children = hierarchy->children;
                std::sort(
                    children.begin(),
                    children.end(),
                    [&](const ecs::EntityId lhs, const ecs::EntityId rhs)
                    {
                        return EntityDisplayName(world, lhs) < EntityDisplayName(world, rhs);
                    });

                for (const ecs::EntityId child : children)
                {
                    if (world.IsAlive(child) && MatchesWindowBinding(world, child, windowId))
                    {
                        DrawEntityNode(windowId, child);
                    }
                }
            }

            ImGui::TreePop();
        }
    }

    ecs::EntityId SceneEditor::PickEntityAtScreenPosition(
        const SceneEditorWindowContext& windowContext,
        const editor::WindowEditorState& windowState,
        const ImVec2& mousePosition) const
    {
        if (!windowState.renderStats.camera.available)
        {
            return ecs::kInvalidEntity;
        }

        DirectX::XMVECTOR rayOrigin = DirectX::XMVectorZero();
        DirectX::XMVECTOR rayDirection = DirectX::XMVectorZero();
        if (!BuildScreenRay(
                mousePosition,
                windowState.viewport,
                scene::ToDirectXMatrix(windowState.renderStats.camera.view),
                scene::ToDirectXMatrix(windowState.renderStats.camera.projection),
                rayOrigin,
                rayDirection))
        {
            return ecs::kInvalidEntity;
        }

        ecs::World& world = *services_.world;
        std::unordered_map<ecs::EntityId, DirectX::XMFLOAT4X4> worldMatrixCache;
        std::unordered_set<ecs::EntityId> visiting;

        ecs::EntityId closestEntity = ecs::kInvalidEntity;
        float closestDistance = std::numeric_limits<float>::max();

        world.ForEach<ecs::components::TransformComponent, ecs::components::MeshRendererComponent>(
            [&](const ecs::EntityId entity, ecs::components::TransformComponent&, ecs::components::MeshRendererComponent& renderer)
            {
                if (!renderer.visible || !MatchesWindowBinding(world, entity, windowContext.windowId))
                {
                    return;
                }

                const DirectX::XMMATRIX worldMatrix = scene::ResolveWorldMatrix(
                    world,
                    entity,
                    worldMatrixCache,
                    visiting,
                    nullptr,
                    services_.logger);

                float hitDistance = 0.0f;
                if (IntersectRayWithEntity(world, entity, rayOrigin, rayDirection, worldMatrix, hitDistance) &&
                    hitDistance < closestDistance)
                {
                    closestDistance = hitDistance;
                    closestEntity = entity;
                }
            });

        return closestEntity;
    }

    void SceneEditor::SpawnRenderableEntity(const core::WindowId windowId, std::string meshPath, std::string materialPath)
    {
        ecs::World& world = *services_.world;

        ecs::components::Vec3 spawnPosition{0.0f, 0.0f, 3.0f};
        world.ForEach<ecs::components::CameraComponent, ecs::components::WindowBindingComponent>(
            [&](const ecs::EntityId, ecs::components::CameraComponent& camera, ecs::components::WindowBindingComponent& binding)
            {
                if (binding.windowId != windowId || !camera.isPrimary)
                {
                    return;
                }

                const float yawRad = DirectX::XMConvertToRadians(camera.rotationDeg.y);
                const float pitchRad = DirectX::XMConvertToRadians(camera.rotationDeg.x);
                spawnPosition = {
                    camera.position.x + std::sin(yawRad) * std::cos(pitchRad) * 5.0f,
                    camera.position.y - std::sin(pitchRad) * 5.0f,
                    camera.position.z + std::cos(yawRad) * std::cos(pitchRad) * 5.0f,
                };
            });

        const ecs::EntityId entity = world.CreateEntity();
        world.Emplace<ecs::components::TagComponent>(entity).name = std::filesystem::path(meshPath).stem().string() + "_" + std::to_string(entity);
        auto& transform = world.Emplace<ecs::components::TransformComponent>(entity);
        transform.position = spawnPosition;
        auto& renderer = world.Emplace<ecs::components::MeshRendererComponent>(entity);
        renderer.meshPath = std::move(meshPath);
        renderer.materialPath = std::move(materialPath);
        renderer.visible = true;
        world.Emplace<ecs::components::WindowBindingComponent>(entity).windowId = windowId;

        core::ServiceLocator::GetEditorRuntimeState().selectedEntity = entity;
    }

    bool SceneEditor::ApplyMaterialAsset(const std::string& materialPath, const resource::MaterialAsset& asset) const
    {
        auto material = services_.resourceManager->Load<resource::MaterialAsset>(materialPath);
        if (material == nullptr)
        {
            return false;
        }

        material->asset = asset;
        services_.resourceManager->Load<resource::ShaderAsset>(asset.shaderPath);
        services_.resourceManager->Load<resource::TextureAsset>(asset.texturePath);
        return services_.resourceManager->SaveMaterial(materialPath, material->asset);
    }

    bool SceneEditor::PushMaterialAssetCommand(
        const char* label,
        const std::string& materialPath,
        const resource::MaterialAsset& beforeAsset,
        const resource::MaterialAsset& afterAsset)
    {
        if (MaterialEquals(beforeAsset, afterAsset) || history_ == nullptr)
        {
            return false;
        }

        if (!ApplyMaterialAsset(materialPath, afterAsset))
        {
            return false;
        }

        history_->Push(std::make_unique<editor::LambdaEditorCommand>(
            label,
            [this, materialPath, beforeAsset]() { return ApplyMaterialAsset(materialPath, beforeAsset); },
            [this, materialPath, afterAsset]() { return ApplyMaterialAsset(materialPath, afterAsset); }));
        core::ServiceLocator::GetEditorRuntimeState().sceneDirty = true;
        return true;
    }

    std::string SceneEditor::ResolveSuggestedMaterialForMesh(const std::string& meshPath) const
    {
        if (services_.resourceManager == nullptr)
        {
            return kDefaultMaterialPath;
        }

        const auto materialKeys = services_.resourceManager->GetKnownMaterialKeys();
        if (materialKeys.empty())
        {
            if (auto defaultMaterial = services_.resourceManager->Load<resource::MaterialAsset>(kDefaultMaterialPath); defaultMaterial != nullptr)
            {
                return defaultMaterial->key;
            }

            return kDefaultMaterialPath;
        }

        const std::string meshId = CanonicalAssetId(std::filesystem::path(meshPath));
        if (!meshId.empty())
        {
            for (const auto& materialKey : materialKeys)
            {
                if (CanonicalAssetId(std::filesystem::path(materialKey)) == meshId)
                {
                    return materialKey;
                }
            }
        }

        const auto defaultIt = std::find(materialKeys.begin(), materialKeys.end(), std::string{kDefaultMaterialPath});
        if (defaultIt != materialKeys.end())
        {
            return *defaultIt;
        }

        return materialKeys.front();
    }

    std::string SceneEditor::EnsureTexturePreviewMaterial(const std::string& texturePath) const
    {
        if (texturePath.empty() || services_.resourceManager == nullptr)
        {
            return {};
        }

        resource::MaterialAsset asset{};
        if (auto baseMaterial = services_.resourceManager->Load<resource::MaterialAsset>(kDefaultMaterialPath); baseMaterial != nullptr)
        {
            asset = baseMaterial->asset;
        }
        else
        {
            asset.shaderPath = kDefaultShaderPath;
            asset.tint = {1.0f, 1.0f, 1.0f, 1.0f};
        }

        asset.shaderPath = asset.shaderPath.empty() ? kDefaultShaderPath : asset.shaderPath;
        asset.texturePath = texturePath;
        asset.tint = {1.0f, 1.0f, 1.0f, 1.0f};

        const std::filesystem::path materialPath =
            std::filesystem::path("assets/materials/generated") /
            (SanitizeStem(std::filesystem::path(texturePath)) + ".material.json");

        if (!services_.resourceManager->SaveMaterial(materialPath, asset))
        {
            return {};
        }

        return materialPath.generic_string();
    }

    std::string SceneEditor::CaptureSceneSnapshot() const
    {
        return services_.captureSceneSnapshot != nullptr ? services_.captureSceneSnapshot() : std::string{};
    }

    void SceneEditor::RecordSceneMutationFromItem(const char* label)
    {
        if (services_.captureSceneSnapshot == nullptr || services_.restoreSceneSnapshot == nullptr)
        {
            pendingSceneMutationSnapshot_.clear();
            return;
        }

        if (ImGui::IsItemActivated() && pendingSceneMutationSnapshot_.empty())
        {
            pendingSceneMutationSnapshot_ = CaptureSceneSnapshot();
        }

        if (!ImGui::IsItemDeactivatedAfterEdit())
        {
            return;
        }

        if (pendingSceneMutationSnapshot_.empty())
        {
            return;
        }

        std::string snapshotBefore = std::move(pendingSceneMutationSnapshot_);
        pendingSceneMutationSnapshot_.clear();

        const std::string snapshotAfter = services_.captureSceneSnapshot();
        if (snapshotBefore == snapshotAfter)
        {
            return;
        }

        const auto restore = services_.restoreSceneSnapshot;
        history_->Push(std::make_unique<editor::LambdaEditorCommand>(
            label,
            [restore, snapshotBefore]() { return restore(snapshotBefore); },
            [restore, snapshotAfter]() { return restore(snapshotAfter); }));
        core::ServiceLocator::GetEditorRuntimeState().sceneDirty = true;
    }

    void SceneEditor::RecordSceneMutationImmediate(const char* label, const std::string& beforeSnapshot)
    {
        if (services_.captureSceneSnapshot == nullptr || services_.restoreSceneSnapshot == nullptr)
        {
            return;
        }

        const std::string afterSnapshot = services_.captureSceneSnapshot();
        if (beforeSnapshot == afterSnapshot)
        {
            return;
        }

        const auto restore = services_.restoreSceneSnapshot;
        history_->Push(std::make_unique<editor::LambdaEditorCommand>(
            label,
            [restore, beforeSnapshot]() { return restore(beforeSnapshot); },
            [restore, afterSnapshot]() { return restore(afterSnapshot); }));
        core::ServiceLocator::GetEditorRuntimeState().sceneDirty = true;
    }

    void SceneEditor::CommitPendingGizmoMutation()
    {
        if (pendingGizmoMutationSnapshot_.empty() || services_.captureSceneSnapshot == nullptr || services_.restoreSceneSnapshot == nullptr)
        {
            pendingGizmoMutationSnapshot_.clear();
            return;
        }

        const std::string afterSnapshot = services_.captureSceneSnapshot();
        const std::string beforeSnapshot = std::exchange(pendingGizmoMutationSnapshot_, {});
        if (beforeSnapshot == afterSnapshot)
        {
            return;
        }

        const auto restore = services_.restoreSceneSnapshot;
        history_->Push(std::make_unique<editor::LambdaEditorCommand>(
            "Manipulate Transform Gizmo",
            [restore, beforeSnapshot]() { return restore(beforeSnapshot); },
            [restore, afterSnapshot]() { return restore(afterSnapshot); }));
        core::ServiceLocator::GetEditorRuntimeState().sceneDirty = true;
    }

    void SceneEditor::DeleteSelectedEntity()
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (editorState.selectedEntity == ecs::kInvalidEntity || editorState.mode != editor::RuntimeMode::Edit)
        {
            return;
        }

        const std::string beforeSnapshot = services_.captureSceneSnapshot != nullptr ? services_.captureSceneSnapshot() : std::string();
        if (services_.world->DestroyEntity(editorState.selectedEntity))
        {
            editorState.selectedEntity = ecs::kInvalidEntity;
            RecordSceneMutationImmediate("Delete Entity", beforeSnapshot);
        }
    }
}