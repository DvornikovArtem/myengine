#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    void SceneEditor::BuildAssetBrowserPanel(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showAssetBrowser || contentBrowser_ == nullptr)
        {
            return;
        }

        if (ImGui::Begin(kAssetBrowserWindowName))
        {
            ContentBrowserHooks hooks;
            hooks.meshPayloadType = kMeshPayloadType;
            hooks.materialPayloadType = kMaterialPayloadType;
            hooks.texturePayloadType = kTexturePayloadType;
            hooks.openScene = [this](const std::string& scenePath) { RequestOpenScene(scenePath); };
            hooks.openPrefab = [this](const std::string& prefabName)
            {
                if (services_.prefabLibrary == nullptr || prefabInspector_ == nullptr)
                {
                    return;
                }

                core::ServiceLocator::GetEditorRuntimeState().showPrefabs = true;
                prefabInspector_->Select(*services_.prefabLibrary, prefabName);
                ImGui::SetWindowFocus(kPrefabsWindowName);
            };
            hooks.openMaterial = [this](const std::string& materialPath)
            {
                auto& state = core::ServiceLocator::GetEditorRuntimeState();
                state.showMaterialEditor = true;
                pinnedMaterialPath_ = materialPath;
                pinnedMaterialEntity_ = state.selectedEntity;
                ImGui::SetWindowFocus(kMaterialEditorWindowName);
            };

            contentBrowser_->Draw(hooks);
        }
        ImGui::End();

        DrawOpenScenePrompt();
    }

    void SceneEditor::RequestOpenScene(const std::string& scenePath)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!services_.openScene || editorState.mode != editor::RuntimeMode::Edit)
        {
            return;
        }

        if (editorState.sceneDirty)
        {
            pendingOpenScenePath_ = scenePath; // DrawOpenScenePrompt asks what to do with the changes
            return;
        }

        OpenSceneNow(scenePath);
    }

    void SceneEditor::OpenSceneNow(const std::string& scenePath)
    {
        if (!services_.openScene || !services_.openScene(scenePath))
        {
            if (services_.logger != nullptr)
            {
                services_.logger->Warning("Content Browser: failed to open the scene " + scenePath);
            }
            return;
        }

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        editorState.selectedEntity = ecs::kInvalidEntity;
        editorState.sceneDirty = false;
        pinnedMaterialPath_.clear();
        pendingSceneMutationSnapshot_.clear();
        pendingGizmoMutationSnapshot_.clear();
        if (history_ != nullptr)
        {
            history_->Clear();
        }
    }

    void SceneEditor::DrawOpenScenePrompt()
    {
        if (pendingOpenScenePath_.empty())
        {
            return;
        }

        constexpr char kPromptName[] = "Unsaved changes##open_scene";
        if (!ImGui::IsPopupOpen(kPromptName))
        {
            ImGui::OpenPopup(kPromptName);
        }

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kPromptName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        ImGui::Text("The current scene has unsaved changes.");
        ImGui::TextDisabled("Open %s?", pendingOpenScenePath_.c_str());
        ImGui::Spacing();

        const std::string target = pendingOpenScenePath_;
        if (ImGui::Button("Save and open"))
        {
            const bool saved = services_.saveScene && services_.saveScene();
            if (saved)
            {
                core::ServiceLocator::GetEditorRuntimeState().sceneDirty = false;
                pendingOpenScenePath_.clear();
                ImGui::CloseCurrentPopup();
                OpenSceneNow(target);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Open without saving"))
        {
            pendingOpenScenePath_.clear();
            ImGui::CloseCurrentPopup();
            OpenSceneNow(target);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            pendingOpenScenePath_.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
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
            // Another selection drops a material that was opened from the Content Browser
            if (!pinnedMaterialPath_.empty() && editorState.selectedEntity != pinnedMaterialEntity_)
            {
                pinnedMaterialPath_.clear();
            }

            std::string materialPath = pinnedMaterialPath_;
            if (!materialPath.empty())
            {
                ImGui::TextDisabled("Opened from the Content Browser");
                ImGui::SameLine();
                if (ImGui::SmallButton("Back to selection"))
                {
                    pinnedMaterialPath_.clear();
                    ImGui::End();
                    return;
                }
            }
            else
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
                materialPath = renderer->materialPath;
            }

            auto materialResource = services_.resourceManager->Load<resource::MaterialAsset>(materialPath);
            if (materialResource == nullptr)
            {
                ImGui::TextDisabled("Failed to load material.");
                ImGui::End();
                return;
            }

            ImGui::TextDisabled("%s", materialPath.c_str());
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
            windowState.materialPreviewMaterialPath = materialPath;

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
                    materialPath,
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
                    materialPath,
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
                    materialPath,
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
