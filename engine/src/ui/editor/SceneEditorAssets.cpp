#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        // "crate.obj" -> "crate", "default.material.json" -> "default"
        std::string AssetName(const std::string& path)
        {
            if (path.empty())
            {
                return "None";
            }

            std::filesystem::path name = std::filesystem::path(path).filename();
            while (name.has_extension())
            {
                name = name.stem();
            }
            return name.string();
        }

        // The icon shown in the list of an asset picker, with a stable ID after ## (two files may share a name)
        std::string PickerItem(const char* icon, const std::string& label, const std::string& key)
        {
            return std::string(icon) + "  " + label + "##" + key;
        }

        // A dim explanatory line with an info icon, wrapped at the panel width
        void DrawInfoLine(const char* text)
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float available = ImGui::GetContentRegionAvail().x;
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            PushFontRole(FontRole::Secondary);
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize();
            const float wrapWidth = std::max(available - 24.0f, 40.0f);
            const ImVec2 textSize = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
            PopFontRole();

            DrawIcon(drawList, IconSize::Row14, ICON_INFO, ImVec2(min.x + 7.0f, min.y + 9.0f), style::kTextDim);
            drawList->AddText(font, size, ImVec2(min.x + 22.0f, min.y + 2.0f), style::kTextDim, text, nullptr, wrapWidth);
            ImGui::Dummy(ImVec2(available, std::max(textSize.y + 6.0f, 22.0f)));
        }
    }

    void SceneEditor::BuildAssetBrowserPanel(const SceneEditorWindowContext& windowContext)
    {
        (void)windowContext;

        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showAssetBrowser || contentBrowser_ == nullptr)
        {
            return;
        }

        // Edge to edge: the browser lays out its own tools row, wells and footer
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = ImGui::Begin(kAssetBrowserWindowName, &editorState.showAssetBrowser);
        ImGui::PopStyleVar();
        if (visible)
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
            // One entry point for "open this asset". Today it does what the kinds always did; the asset viewers
            // (textures, meshes, prefabs) are added here. Anything it does not handle goes back to the browser.
            hooks.onOpenAsset = [this, &hooks](const std::string& assetPath, const ContentKind kind)
            {
                switch (kind)
                {
                    case ContentKind::Scene:
                        hooks.openScene(assetPath);
                        return true;
                    case ContentKind::Prefab:
                    {
                        std::string name = std::filesystem::path(assetPath).filename().string();
                        const std::string suffix = ".prefab.json";
                        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                        {
                            name.resize(name.size() - suffix.size());
                        }
                        hooks.openPrefab(name);
                        return true;
                    }
                    case ContentKind::Material:
                        hooks.openMaterial(assetPath);
                        return true;
                    case ContentKind::Folder:
                    case ContentKind::Script:
                    case ContentKind::Mesh:
                    case ContentKind::Texture:
                    case ContentKind::Shader:
                    case ContentKind::Other:
                        break;
                }
                return false;
            };
            hooks.thumbnail = [this](const ContentEntry& entry) { return MakeContentThumbnail(entry); };

            contentBrowser_->Draw(hooks);
        }
        ImGui::End();

        DrawOpenScenePrompt();
    }

    ContentThumbnail SceneEditor::MakeContentThumbnail(const ContentEntry& entry) const
    {
        ContentThumbnail thumbnail;
        if (services_.resourceManager == nullptr)
        {
            return thumbnail;
        }

        // A texture is its own picture. A material shows its base colour map with the tint until the
        // render-to-texture thumbnails (sphere with the material) come; meshes and the rest keep the type icon.
        std::string texturePath;
        core::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
        if (entry.kind == ContentKind::Texture)
        {
            texturePath = entry.path;
        }
        else if (entry.kind == ContentKind::Material)
        {
            if (const auto material = services_.resourceManager->Load<resource::MaterialAsset>(entry.path); material != nullptr)
            {
                texturePath = material->asset.texturePath;
                tint = material->asset.tint;
            }
        }
        if (texturePath.empty())
        {
            return thumbnail;
        }

        // Textures load in the background: the first Load starts it and returns a placeholder
        const auto texture = services_.resourceManager->Load<resource::TextureAsset>(texturePath);
        if (services_.resourceManager->IsLoadPending(texturePath))
        {
            thumbnail.pending = true;
            return thumbnail;
        }
        if (texture == nullptr || !texture->asset.gpuHandle.IsValid() ||
            texture->asset.gpuHandle.value == services_.resourceManager->GetFallbackTextureHandle().value)
        {
            return thumbnail; // the file could not be read: the type icon stays
        }

        // UiManager turns this id back into the render::TextureHandle when it draws the image
        const auto channel = [](const float value)
        {
            return static_cast<std::uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        thumbnail.textureId = static_cast<std::uint64_t>(texture->asset.gpuHandle.value);
        thumbnail.width = texture->asset.data.width;
        thumbnail.height = texture->asset.data.height;
        thumbnail.tint = IM_COL32(channel(tint.r), channel(tint.g), channel(tint.b), channel(tint.a));

        // Transparent pixels get a checkerboard under the picture (RGBA8, alpha is every 4th byte)
        const auto& pixels = texture->asset.data.pixelsRgba8;
        for (std::size_t index = 3; index < pixels.size(); index += 4)
        {
            if (pixels[index] != 255u)
            {
                thumbnail.hasAlpha = true;
                break;
            }
        }
        return thumbnail;
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
        const bool forProject = projectUi_ != nullptr && !projectUi_->pendingOpenProject.empty();
        if (pendingOpenScenePath_.empty() && !forProject)
        {
            return;
        }

        constexpr char kPromptName[] = "Unsaved changes##open_scene";
        if (!ImGui::IsPopupOpen(kPromptName))
        {
            ImGui::OpenPopup(kPromptName);
        }

        constexpr float kDialogWidth = 460.0f;
        if (!BeginDialog(kPromptName, "Unsaved changes", ICON_TRIANGLE_ALERT, kDialogWidth))
        {
            return;
        }

        // The header icon is dim by default; a warning is amber
        {
            const ImVec2 position = ImGui::GetWindowPos();
            DrawIcon(
                ImGui::GetWindowDrawList(),
                IconSize::Row14,
                ICON_TRIANGLE_ALERT,
                ImVec2(position.x + 16.0f + 7.5f, position.y + 19.0f),
                style::kWarning);
        }

        const auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        const std::string mapName = std::filesystem::u8path(editorState.mapPath).stem().u8string();
        const std::string target = forProject
            ? std::filesystem::u8path(projectUi_->pendingOpenProject).stem().u8string()
            : std::filesystem::u8path(pendingOpenScenePath_).stem().u8string();

        const std::string question = "Save changes to " + (mapName.empty() ? std::string("the map") : mapName) +
                                     (forProject ? " before switching to " : " before opening ") + target + "?";
        ImGui::PushTextWrapPos(kDialogWidth - 16.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextStrong));
        ImGui::TextUnformatted(question.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        PushFontRole(FontRole::Tiny);
        ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
        ImGui::TextUnformatted(forProject
                ? "If you don't save, your changes will be lost. The editor restarts."
                : "If you don't save, your changes will be lost.");
        ImGui::PopStyleColor();
        PopFontRole();
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        // The answer is applied once: the pending request is cleared first, then the action runs
        const auto proceed = [&]()
        {
            const std::string scenePath = std::exchange(pendingOpenScenePath_, {});
            const std::string projectFile = forProject ? std::exchange(projectUi_->pendingOpenProject, {}) : std::string();
            ImGui::CloseCurrentPopup();
            if (forProject)
            {
                // The map was saved or is being dropped on purpose: nothing is written on exit
                if (services_.restartWithProject)
                {
                    services_.restartWithProject(projectFile, true);
                }
            }
            else
            {
                OpenSceneNow(scenePath);
            }
        };

        constexpr float kPrimaryWidth = 124.0f;
        constexpr float kDontSaveWidth = 96.0f;
        constexpr float kCancelWidth = 84.0f;
        DialogFooter();
        DialogAlignRight(kPrimaryWidth + kDontSaveWidth + kCancelWidth + 16.0f);
        if (PrimaryButton(forProject ? "Save and Switch" : "Save and Open", nullptr, true, kPrimaryWidth))
        {
            const bool saved = services_.saveScene && services_.saveScene();
            if (saved)
            {
                core::ServiceLocator::GetEditorRuntimeState().sceneDirty = false;
                proceed();
            }
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (Button("Don't Save", nullptr, true, kDontSaveWidth))
        {
            proceed();
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (Button("Cancel", nullptr, true, kCancelWidth))
        {
            pendingOpenScenePath_.clear();
            if (projectUi_ != nullptr)
            {
                projectUi_->pendingOpenProject.clear();
            }
            ImGui::CloseCurrentPopup();
        }

        EndDialog();
    }

    void SceneEditor::BuildMaterialEditorPanel(const SceneEditorWindowContext& windowContext)
    {
        auto& editorState = core::ServiceLocator::GetEditorRuntimeState();
        if (!editorState.showMaterialEditor)
        {
            return;
        }

        auto& windowState = editorState.GetOrCreateWindowState(windowContext.windowId);

        if (ImGui::Begin(kMaterialEditorWindowName, &editorState.showMaterialEditor))
        {
            // Another selection drops a material that was opened from the Content Browser
            if (!pinnedMaterialPath_.empty() && editorState.selectedEntity != pinnedMaterialEntity_)
            {
                pinnedMaterialPath_.clear();
            }

            const bool pinned = !pinnedMaterialPath_.empty();
            std::string materialPath = pinnedMaterialPath_;
            if (!pinned)
            {
                const char* emptyText = nullptr;
                if (editorState.selectedEntity == ecs::kInvalidEntity)
                {
                    emptyText = "Select an entity with a Mesh Renderer, or double-click a material in the Content Browser.";
                }
                else if (!MatchesWindowBinding(*services_.world, editorState.selectedEntity, windowContext.windowId))
                {
                    emptyText = "The selected entity belongs to another window.";
                }
                else
                {
                    const auto* renderer = services_.world->TryGet<ecs::components::MeshRendererComponent>(editorState.selectedEntity);
                    if (renderer == nullptr || renderer->materialPath.empty())
                    {
                        emptyText = "Select an entity with a Mesh Renderer, or double-click a material in the Content Browser.";
                    }
                    else
                    {
                        materialPath = renderer->materialPath;
                    }
                }

                if (emptyText != nullptr)
                {
                    EmptyState(ICON_PALETTE, "No material", emptyText);
                    ImGui::End();
                    return;
                }
            }

            auto materialResource = services_.resourceManager->Load<resource::MaterialAsset>(materialPath);
            if (materialResource == nullptr)
            {
                EmptyState(ICON_PALETTE, "No material", "Failed to load the material (see the log).");
                ImGui::End();
                return;
            }

            const bool editEnabled = editorState.mode == editor::RuntimeMode::Edit;
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // ---- Header: a palette square, the name and the path; "Selection" returns from a pinned material
            bool backToSelection = false;
            {
                const ImVec2 start = ImGui::GetCursorScreenPos();
                const float available = ImGui::GetContentRegionAvail().x;
                const float squareSize = 28.0f;
                drawList->AddRectFilled(start, ImVec2(start.x + squareSize, start.y + squareSize), style::kRecessed, style::kRounding);
                DrawIcon(
                    drawList,
                    IconSize::Row14,
                    ICON_PALETTE,
                    ImVec2(start.x + squareSize * 0.5f, start.y + squareSize * 0.5f),
                    style::kTypeMaterial);

                const float buttonWidth = 104.0f;
                const float textRight = start.x + available - (pinned ? buttonWidth + 8.0f : 0.0f);

                const float textX = start.x + squareSize + 8.0f;
                drawList->PushClipRect(ImVec2(textX, start.y - 2.0f), ImVec2(textRight, start.y + squareSize + 2.0f), true);
                PushFontRole(FontRole::Strong); // the name is SemiBold 14
                drawList->AddText(ImVec2(textX, start.y - 1.0f), style::kTextStrong, AssetName(materialPath).c_str());
                PopFontRole();
                PushFontRole(FontRole::Secondary);
                drawList->AddText(ImVec2(textX, start.y + 15.0f), style::kTextDim, materialPath.c_str());
                PopFontRole();
                drawList->PopClipRect();

                if (pinned)
                {
                    ImGui::SetCursorScreenPos(ImVec2(start.x + available - buttonWidth, start.y + (squareSize - style::kFrameHeight) * 0.5f));
                    backToSelection = Button("Selection", ICON_ARROW_LEFT, true, buttonWidth);
                    Tooltip("Back to the selected entity's material");
                }

                ImGui::SetCursorScreenPos(start);
                ImGui::Dummy(ImVec2(available, squareSize + 8.0f));
            }

            if (!editEnabled)
            {
                Banner(BannerKind::Play, "Playing â edit the material after Stop.");
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
            }

            DrawInfoLine("Edits apply live to every entity using this material.");

            // ---- Material: shader, texture, tint
            if (BeginCategory("Material"))
            {
                ImGui::BeginDisabled(!editEnabled);
                if (BeginPropertyGrid("##material_asset"))
                {
                    resource::MaterialAsset beforeAsset = CloneMaterialAsset(materialResource->asset);
                    const auto commitMaterialChange = [&](const char* label)
                    {
                        if ((ImGui::IsItemDeactivatedAfterEdit() || !ImGui::IsItemActive()) &&
                            !MaterialEquals(beforeAsset, materialResource->asset))
                        {
                            PushMaterialAssetCommand(label, materialPath, beforeAsset, CloneMaterialAsset(materialResource->asset));
                            beforeAsset = CloneMaterialAsset(materialResource->asset);
                        }
                    };

                    PropertyLabel("Shader");
                    {
                        const auto shaderKeys = services_.resourceManager->GetKnownShaderKeys();
                        if (BeginAssetPicker("##shader", AssetName(materialResource->asset.shaderPath).c_str(), ICON_CODE_XML, style::kTypeShader, materialResource->asset.shaderPath.c_str()))
                        {
                            for (const auto& shaderKey : shaderKeys)
                            {
                                const bool selected = ResourcePathsEqual(*services_.resourceManager, shaderKey, materialResource->asset.shaderPath);
                                if (ImGui::Selectable(PickerItem(ICON_CODE_XML, AssetName(shaderKey), shaderKey).c_str(), selected))
                                {
                                    materialResource->asset.shaderPath = shaderKey;
                                    services_.resourceManager->Load<resource::ShaderAsset>(shaderKey);
                                }
                                if (selected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            EndAssetPicker();
                        }
                        commitMaterialChange("Change Material Shader");
                    }

                    PropertyLabel("Texture");
                    {
                        const auto textureKeys = services_.resourceManager->GetKnownTextureKeys();
                        if (BeginAssetPicker("##texture", AssetName(materialResource->asset.texturePath).c_str(), ICON_IMAGE, style::kTypeTexture, materialResource->asset.texturePath.c_str()))
                        {
                            for (const auto& textureKey : textureKeys)
                            {
                                const bool selected = ResourcePathsEqual(*services_.resourceManager, textureKey, materialResource->asset.texturePath);
                                if (ImGui::Selectable(PickerItem(ICON_IMAGE, AssetName(textureKey), textureKey).c_str(), selected))
                                {
                                    materialResource->asset.texturePath = textureKey;
                                    services_.resourceManager->Load<resource::TextureAsset>(textureKey);
                                }
                                if (selected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            EndAssetPicker();
                        }
                        commitMaterialChange("Change Material Texture");
                    }

                    PropertyLabel("Tint");
                    {
                        float tint[4]{
                            materialResource->asset.tint.r,
                            materialResource->asset.tint.g,
                            materialResource->asset.tint.b,
                            materialResource->asset.tint.a,
                        };

                        // A 40 x 24 swatch (the picker opens on click) and the four 0-255 values in one row
                        if (ImGui::ColorButton("##tint_swatch", ImVec4(tint[0], tint[1], tint[2], tint[3]),
                                ImGuiColorEditFlags_AlphaPreview, ImVec2(40.0f, style::kFrameHeight)))
                        {
                            ImGui::OpenPopup("##tint_picker");
                        }
                        if (ImGui::BeginPopup("##tint_picker"))
                        {
                            if (ImGui::ColorPicker4("##tint_picker_4", tint, ImGuiColorEditFlags_AlphaBar))
                            {
                                materialResource->asset.tint = {tint[0], tint[1], tint[2], tint[3]};
                            }
                            commitMaterialChange("Change Material Tint");
                            ImGui::EndPopup();
                        }

                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        int channels[4]{
                            static_cast<int>(std::lround(tint[0] * 255.0f)),
                            static_cast<int>(std::lround(tint[1] * 255.0f)),
                            static_cast<int>(std::lround(tint[2] * 255.0f)),
                            static_cast<int>(std::lround(tint[3] * 255.0f)),
                        };
                        if (ImGui::DragInt4("##tint_channels", channels, 0.5f, 0, 255))
                        {
                            materialResource->asset.tint = {
                                static_cast<float>(std::clamp(channels[0], 0, 255)) / 255.0f,
                                static_cast<float>(std::clamp(channels[1], 0, 255)) / 255.0f,
                                static_cast<float>(std::clamp(channels[2], 0, 255)) / 255.0f,
                                static_cast<float>(std::clamp(channels[3], 0, 255)) / 255.0f,
                            };
                        }
                        FocusOutline();
                        commitMaterialChange("Change Material Tint");
                    }
                    EndPropertyGrid();
                }
                ImGui::EndDisabled();
                EndCategory();
            }

            // ---- Preview: the mesh the viewport shows while this tab is open
            if (BeginCategory("Preview"))
            {
                ImGui::Indent(20.0f);
                static const char* const kShapes[] = {"Sphere", "Cube"};
                const int current = windowState.materialPreviewShape == editor::MaterialPreviewShape::Cube ? 1 : 0;
                const int next = Segmented("##preview_shape", kShapes, 2, current);
                if (next != current)
                {
                    windowState.materialPreviewShape =
                        next == 1 ? editor::MaterialPreviewShape::Cube : editor::MaterialPreviewShape::Sphere;
                }
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                PushFontRole(FontRole::Tiny);
                ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
                ImGui::TextUnformatted("Shown in the viewport while this tab is open");
                ImGui::PopStyleColor();
                PopFontRole();
                ImGui::Unindent(20.0f);
                EndCategory();
            }

            windowState.materialPreviewEnabled = true;
            windowState.materialPreviewMaterialPath = materialPath;

            if (backToSelection)
            {
                pinnedMaterialPath_.clear();
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
