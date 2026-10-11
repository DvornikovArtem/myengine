#pragma once

// Internal to the scene editor implementation (engine/src/ui/editor); not a public header.

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
#include <myengine/editor/ThumbnailService.h>
#include <myengine/editor/TransformGizmo.h>
#include <myengine/ecs/World.h>
#include <myengine/ecs/components/CameraComponent.h>
#include <myengine/ecs/components/CameraControllerComponent.h>
#include <myengine/ecs/components/ColliderComponent.h>
#include <myengine/ecs/components/HierarchyComponent.h>
#include <myengine/ecs/components/MeshRendererComponent.h>
#include <myengine/ecs/components/RigidbodyComponent.h>
#include <myengine/ecs/components/ScriptComponent.h>
#include <myengine/ecs/components/TagComponent.h>
#include <myengine/ecs/components/TransformComponent.h>
#include <myengine/ecs/components/WindowBindingComponent.h>
#include <myengine/physics/PhysicsWorldState.h>
#include <myengine/resource/ResourceManager.h>
#include <myengine/scene/TransformUtils.h>
#include <myengine/ui/ContentBrowser.h>
#include <myengine/ui/AssistantPanel.h>
#include <myengine/ui/SceneEditor.h>
#include <myengine/ui/PrefabInspector.h>
#include <myengine/ui/ScriptInspector.h>

#include "EditorIcons.h"
#include "EditorStyle.h"
#include "EditorWidgets.h"

namespace myengine::ui::detail
{
    // Window titles: icon + name, with a stable ID after ### so a saved layout survives title changes
    inline constexpr char kHierarchyWindowName[] = ICON_LIST_TREE " Outliner###Outliner";
    inline constexpr char kInspectorWindowName[] = ICON_SLIDERS_HORIZONTAL " Details###Details";
    inline constexpr char kStatisticsWindowName[] = ICON_ACTIVITY " Statistics###Statistics";
    inline constexpr char kViewportWindowName[] = ICON_MONITOR " Viewport###Viewport";
    inline constexpr char kMaterialEditorWindowName[] = ICON_PALETTE " Material Editor###MaterialEditor";
    inline constexpr char kAssetBrowserWindowName[] = ICON_FOLDER " Content Browser###ContentBrowser";
    inline constexpr char kPrefabsWindowName[] = ICON_PACKAGE " Prefabs###Prefabs";
    inline constexpr char kScriptConsoleWindowName[] = ICON_SCROLL_TEXT " Output Log###OutputLog";
    inline constexpr char kAssistantWindowName[] = ICON_SPARKLES " Assistant###Assistant";
    inline constexpr char kMeshPayloadType[] = "MYENGINE_ASSET_MESH";
    inline constexpr char kMaterialPayloadType[] = "MYENGINE_ASSET_MATERIAL";
    inline constexpr char kTexturePayloadType[] = "MYENGINE_ASSET_TEXTURE";
    // The assistant accepts these from the Content Browser (the viewport does not): the data is the asset key
    inline constexpr char kPrefabPayloadType[] = "MYENGINE_ASSET_PREFAB";
    inline constexpr char kScriptPayloadType[] = "MYENGINE_ASSET_SCRIPT";
    inline constexpr char kFilePayloadType[] = "MYENGINE_ASSET_FILE";
    inline constexpr char kDefaultMaterialPath[] = "assets/materials/default.material.json";
    inline constexpr char kDefaultShaderPath[] = "assets/shaders/textured_lit.shader.json";
    inline constexpr char kCubeMeshPath[] = "assets/models/crate.obj";
    inline constexpr char kSphereMeshPath[] = "assets/models/sphere.obj";
    inline constexpr float kViewportToolbarPadding = 12.0f;
    inline constexpr float kPlayFrameThickness = 3.0f;
    // The overlay toolbar moved to the main toolbar (U1); the viewport overlay is empty until U2 adds view controls
    inline constexpr float kViewportToolbarHeight = 0.0f;
    inline constexpr float kDefaultRenderableRadius = 0.8660254f;

    inline bool MatchesWindowBinding(ecs::World& world, const ecs::EntityId entity, const core::WindowId windowId)
    {
        const auto* binding = world.TryGet<ecs::components::WindowBindingComponent>(entity);
        return binding == nullptr || binding->windowId == 0 || binding->windowId == windowId;
    }

    inline std::string EntityLabel(ecs::World& world, const ecs::EntityId entity)
    {
        const auto* tag = world.TryGet<ecs::components::TagComponent>(entity);
        const std::string name = tag != nullptr && !tag->name.empty()
            ? tag->name
            : "Entity";
        return name + "##entity_" + std::to_string(entity);
    }

    inline std::string EntityDisplayName(ecs::World& world, const ecs::EntityId entity)
    {
        const auto* tag = world.TryGet<ecs::components::TagComponent>(entity);
        if (tag != nullptr && !tag->name.empty())
        {
            return tag->name;
        }

        return "Entity_" + std::to_string(entity);
    }

    inline std::string FileNameLabel(const std::string& path)
    {
        if (path.empty())
        {
            return "<none>";
        }

        return std::filesystem::path(path).filename().string();
    }

    inline bool ResourcePathsEqual(
        const resource::ResourceManager& resourceManager,
        const std::string& lhs,
        const std::string& rhs)
    {
        if (lhs.empty() || rhs.empty())
        {
            return lhs == rhs;
        }

        return resourceManager.ResolvePath(lhs) == resourceManager.ResolvePath(rhs);
    }

    // The options of an asset picker: thumbnails, the meta line ("Mesh · 960 tris") and the path comparison
    inline AssetPickerOptions MakePickerOptions(
        const PickerKind kind,
        resource::ResourceManager& resourceManager,
        editor::ThumbnailService* thumbnails)
    {
        AssetPickerOptions options;
        options.kind = kind;
        options.thumbnails = thumbnails;
        options.samePath = [&resourceManager](const std::string& lhs, const std::string& rhs)
        {
            return ResourcePathsEqual(resourceManager, lhs, rhs);
        };
        options.meta = [&resourceManager, kind](const std::string& key) -> std::string
        {
            if (kind == PickerKind::Mesh)
            {
                const auto mesh = resourceManager.Load<resource::MeshAsset>(key);
                if (mesh != nullptr && !resourceManager.IsLoadPending(key) && !mesh->asset.data.indices.empty())
                {
                    return "Mesh \xC2\xB7 " + std::to_string(mesh->asset.data.indices.size() / 3) + " tris";
                }
                return "Mesh";
            }
            if (kind == PickerKind::Texture)
            {
                const auto texture = resourceManager.Load<resource::TextureAsset>(key);
                if (texture != nullptr && !resourceManager.IsLoadPending(key) && texture->asset.data.width > 0)
                {
                    return "Texture \xC2\xB7 " + std::to_string(texture->asset.data.width) + "\xC3\x97" +
                        std::to_string(texture->asset.data.height);
                }
                return "Texture";
            }
            return kind == PickerKind::Material ? "Material" : "Shader";
        };
        return options;
    }

    // "crate.obj" -> "crate", "default.material.json" -> "default"
    inline std::string AssetNameWithoutExtensions(const std::string& fileName)
    {
        std::filesystem::path name = std::filesystem::path(fileName).filename();
        while (name.has_extension())
        {
            name = name.stem();
        }
        return name.string();
    }

    inline std::string FormatBytes(const std::uint64_t bytes)
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

    inline std::string SanitizeStem(const std::filesystem::path& path)
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

    inline std::string CanonicalAssetId(std::filesystem::path path)
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

    inline const char* PreviewMeshPath(const editor::MaterialPreviewShape shape)
    {
        return shape == editor::MaterialPreviewShape::Cube ? kCubeMeshPath : kSphereMeshPath;
    }

    inline bool MaterialEquals(const resource::MaterialAsset& lhs, const resource::MaterialAsset& rhs)
    {
        return lhs.shaderPath == rhs.shaderPath &&
            lhs.texturePath == rhs.texturePath &&
            lhs.tint.r == rhs.tint.r &&
            lhs.tint.g == rhs.tint.g &&
            lhs.tint.b == rhs.tint.b &&
            lhs.tint.a == rhs.tint.a;
    }

    inline void DrawSceneVisibilityMask(const editor::ViewportRect& viewportRect)
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

    inline std::vector<ecs::EntityId> CollectVisibleEntities(ecs::World& world, const core::WindowId windowId)
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

    inline std::array<float, 3> ToFloat3(const ecs::components::Vec3& value)
    {
        return {value.x, value.y, value.z};
    }

    inline void FromFloat3(const std::array<float, 3>& value, ecs::components::Vec3& target)
    {
        target.x = value[0];
        target.y = value[1];
        target.z = value[2];
    }

    inline resource::MaterialAsset CloneMaterialAsset(const resource::MaterialAsset& asset)
    {
        return asset;
    }

    inline DirectX::BoundingSphere BuildPickBounds(ecs::World& world, const ecs::EntityId entity, const DirectX::XMMATRIX& worldMatrix)
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

    inline bool IntersectRayWithLocalAabb(
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

    inline bool IntersectRayWithEntity(
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

    inline bool BuildScreenRay(
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

namespace myengine::ui
{
    // The orbit camera and the shape of a material preview (Material Editor and Material Viewer)
    struct MaterialPreviewView
    {
        float yaw = 0.61f;
        float pitch = 0.44f;
        float distance = 3.0f;
        bool cube = false;
        bool wireframe = false;
    };

    // Draws a live preview of the material at the cursor, `size` big; orbit with the left button, wheel zooms.
    // `openViewer` (optional) adds an "open in the viewer" button next to Reset camera and is set when it is pressed.
    // `drawOverlay` false: the caller draws its own controls (the Material Viewer).
    // Defined in SceneEditorAssets.cpp.
    void DrawMaterialPreviewView(
        editor::ThumbnailService* thumbnails,
        const char* viewId,
        const std::string& materialPath,
        const ImVec2& size,
        float rounding,
        MaterialPreviewView& view,
        bool* openViewer,
        bool drawOverlay = true);

    enum class AssetViewerType : std::uint8_t
    {
        Texture,
        Mesh,
        Material,
        Prefab,
    };

    // One open viewer tab (SceneEditorViewers.cpp); state that survives a hot reload of the file lives here
    struct AssetViewerTab
    {
        AssetViewerType type = AssetViewerType::Texture;
        std::string path;  // asset key
        std::string name;  // shown in the header
        std::string title; // the window title with a stable ID
        bool open = true;
        bool docked = false;
        bool focusRequested = false;

        // 3D (mesh, prefab) camera
        float yaw = 0.55f;
        float pitch = 0.32f;
        float distance = 3.0f;
        float target[3]{0.0f, 0.0f, 0.0f};
        bool needsFrame = true;
        bool wireframe = false;
        bool showGrid = true;
        bool showBounds = false;
        std::uint32_t infoHandle = 0; // the GPU handle that localBounds / hasAlpha were computed for
        editor::BoundsBox localBounds;
        std::string previewMaterial; // mesh viewer: chosen preview material; empty: the suggested one

        // Texture
        bool fit = true;
        float zoom = 1.0f;
        float pan[2]{0.0f, 0.0f};
        bool channelR = true;
        bool channelG = true;
        bool channelB = true;
        bool channelA = true;
        bool checker = true; // a checkerboard under the transparent pixels
        bool nearest = true;
        bool nearestChosen = false; // the user picked the filter: do not choose it from the size any more
        bool hasAlpha = false;
        render::TextureHandle maskedTexture; // a copy with the masked channels, when not all are on
        std::uint32_t maskedKey = ~0u;

        // Material
        MaterialPreviewView materialView;

        // Prefab
        std::vector<editor::PrefabEntityInfo> prefabEntities;
        bool prefabRead = false;
        double prefabCheckTime = 0.0;
        std::filesystem::file_time_type prefabWriteTime{};
        int selectedEntity = -1;
    };

    // State of the Project Browser and the map dialogs (SceneEditorProject.cpp)
    struct ProjectUiState
    {
        enum class MapDialog
        {
            None,
            NewMap,
            OpenMap,
            SaveMapAs,
        };

        MapDialog mapDialog = MapDialog::None;
        bool openMapDialogRequested = false;
        std::string mapName;
        std::string message;
        bool confirmOverwrite = false;
        std::vector<std::string> mapChoices; // project paths of the maps that Open Map lists

        bool browserWasOpen = false;
        std::vector<std::string> recentProjects; // project files, newest first
        std::string newProjectName = "NewProject";
        std::string newProjectFolder;
        std::string browserMessage;

        std::string pendingOpenProject; // waits for the answer of the unsaved changes prompt
    };
}
