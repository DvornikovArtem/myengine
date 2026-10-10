#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

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
                    const bool selected = ResourcePathsEqual(*services_.resourceManager, shaderKey, materialResource->asset.shaderPath);
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
                    const bool selected = ResourcePathsEqual(*services_.resourceManager, textureKey, materialResource->asset.texturePath);
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
}
