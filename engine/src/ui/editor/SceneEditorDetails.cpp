#include <cstdio>

#include "SceneEditorInternal.h"
#include "SceneEditorEntityKind.h"

namespace myengine::ui
{
    using namespace detail;

    namespace
    {
        // "crate.obj" -> "crate", "default.material.json" -> "default": the picker shows the asset name
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

        // Dim 13 px line at the indent of a nested group
        void DrawGroupCaption(const char* icon, const ImU32 iconColor, const std::string& text, const float indent)
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float available = ImGui::GetContentRegionAvail().x;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const float rowHeight = 24.0f;
            const float centerY = min.y + rowHeight * 0.5f;
            DrawIcon(drawList, IconSize::Row14, icon, ImVec2(min.x + indent + 7.0f, centerY), iconColor);

            PushFontRole(FontRole::Secondary);
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize();
            PopFontRole();
            drawList->AddText(font, size, ImVec2(min.x + indent + 22.0f, std::floor(centerY - size * 0.5f)), style::kTextDim, text.c_str());
            ImGui::Dummy(ImVec2(available, rowHeight));
        }

        // A dim explanatory line with an info icon, wrapped at the panel width
        void DrawInfoLine(const char* text, const float indent)
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const float available = ImGui::GetContentRegionAvail().x;
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            PushFontRole(FontRole::Secondary);
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize();
            const float wrapWidth = std::max(available - indent - 24.0f, 40.0f);
            const ImVec2 textSize = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
            PopFontRole();

            DrawIcon(drawList, IconSize::Row14, ICON_INFO, ImVec2(min.x + indent + 7.0f, min.y + 9.0f), style::kTextDim);
            drawList->AddText(font, size, ImVec2(min.x + indent + 22.0f, min.y + 2.0f), style::kTextDim, text, nullptr, wrapWidth);
            ImGui::Dummy(ImVec2(available, std::max(textSize.y + 6.0f, 22.0f)));
        }
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
                EmptyState(
                    ICON_SLIDERS_HORIZONTAL,
                    "Nothing selected",
                    "Select an entity in the Outliner or click it in the Viewport.");
                ImGui::End();
                return;
            }

            const bool editEnabled = editorState.mode == editor::RuntimeMode::Edit;
            const EntityKindInfo kind = ClassifyEntity(world, entity);
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // ---- Header: a type icon, the name field and "Entity N · Type"
            {
                const ImVec2 start = ImGui::GetCursorScreenPos();
                const float squareSize = 28.0f;
                drawList->AddRectFilled(start, ImVec2(start.x + squareSize, start.y + squareSize), style::kRecessed, style::kRounding);
                DrawIcon(
                    drawList,
                    IconSize::Row14,
                    kind.icon,
                    ImVec2(start.x + squareSize * 0.5f, start.y + squareSize * 0.5f),
                    kind.color);

                ImGui::SetCursorScreenPos(ImVec2(start.x + squareSize + 8.0f, start.y + (squareSize - style::kFrameHeight) * 0.5f));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (auto* tag = world.TryGet<ecs::components::TagComponent>(entity); tag != nullptr)
                {
                    ImGui::BeginDisabled(!editEnabled);
                    ImGui::InputText("##entity_name", &tag->name);
                    FocusOutline();
                    RecordSceneMutationFromItem("Rename Entity");
                    ImGui::EndDisabled();
                }
                else
                {
                    ImGui::BeginDisabled();
                    std::string placeholder = "Entity " + std::to_string(entity);
                    ImGui::InputText("##entity_name", &placeholder);
                    ImGui::EndDisabled();
                }

                const std::string subtitle = "Entity " + std::to_string(entity) + " \xC2\xB7 " + kind.name;
                PushFontRole(FontRole::Secondary);
                ImFont* font = ImGui::GetFont();
                const float size = ImGui::GetFontSize();
                PopFontRole();
                drawList->AddText(
                    font,
                    size,
                    ImVec2(start.x + squareSize + 8.0f + 10.0f, start.y + squareSize + 6.0f),
                    style::kTextDim,
                    subtitle.c_str());

                ImGui::SetCursorScreenPos(start);
                ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, squareSize + 6.0f + size + 8.0f));
            }

            if (!editEnabled)
            {
                Banner(BannerKind::Play, "Playing \xE2\x80\x94 live values. Edit them after Stop.");
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
            }

            // ---- Transform
            if (auto* transform = world.TryGet<ecs::components::TransformComponent>(entity); transform != nullptr)
            {
                if (BeginCategory("Transform"))
                {
                    ImGui::BeginDisabled(!editEnabled);
                    if (BeginPropertyGrid("##transform"))
                    {
                        PropertyLabel("Position");
                        auto position = ToFloat3(transform->position);
                        if (DragVector3("##position", position.data(), 0.05f, "%.3f",
                                [this](int) { RecordSceneMutationFromItem("Edit Transform Position"); }))
                        {
                            FromFloat3(position, transform->position);
                        }

                        PropertyLabel("Rotation");
                        auto rotation = ToFloat3(transform->rotationDeg);
                        if (DragVector3("##rotation", rotation.data(), 0.5f, "%.3f",
                                [this](int) { RecordSceneMutationFromItem("Edit Transform Rotation"); }))
                        {
                            FromFloat3(rotation, transform->rotationDeg);
                        }

                        PropertyLabel("Scale");
                        auto scale = ToFloat3(transform->scale);
                        if (DragVector3("##scale", scale.data(), 0.02f, "%.3f",
                                [this](int) { RecordSceneMutationFromItem("Edit Transform Scale"); }))
                        {
                            for (float& component : scale)
                            {
                                component = std::clamp(component, 0.01f, 200.0f);
                            }
                            FromFloat3(scale, transform->scale);
                        }
                        EndPropertyGrid();
                    }
                    ImGui::EndDisabled();
                    EndCategory();
                }
            }

            // ---- Mesh Renderer
            if (auto* renderer = world.TryGet<ecs::components::MeshRendererComponent>(entity); renderer != nullptr)
            {
                if (BeginCategory("Mesh Renderer"))
                {
                    ImGui::BeginDisabled(!editEnabled);
                    if (BeginPropertyGrid("##mesh_renderer"))
                    {
                        PropertyLabel("Mesh");
                        {
                            const auto meshKeys = services_.resourceManager->GetKnownMeshKeys();
                            if (BeginAssetPicker("##mesh", AssetName(renderer->meshPath).c_str(), ICON_BOX, style::kTypeMesh, renderer->meshPath.c_str()))
                            {
                                for (const auto& meshKey : meshKeys)
                                {
                                    const bool selected = ResourcePathsEqual(*services_.resourceManager, meshKey, renderer->meshPath);
                                    if (ImGui::Selectable(PickerItem(ICON_BOX, AssetName(meshKey), meshKey).c_str(), selected))
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
                                EndAssetPicker();
                            }
                        }

                        PropertyLabel("Material");
                        {
                            const auto materialKeys = services_.resourceManager->GetKnownMaterialKeys();
                            if (BeginAssetPicker("##material", AssetName(renderer->materialPath).c_str(), ICON_PALETTE, style::kTypeMaterial, renderer->materialPath.c_str()))
                            {
                                for (const auto& materialKey : materialKeys)
                                {
                                    const bool selected = ResourcePathsEqual(*services_.resourceManager, materialKey, renderer->materialPath);
                                    if (ImGui::Selectable(PickerItem(ICON_PALETTE, AssetName(materialKey), materialKey).c_str(), selected))
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
                                EndAssetPicker();
                            }

                            // Third column: the same action as the old "Open Material Editor" button
                            ImGui::TableSetColumnIndex(2);
                            if (IconButton("##open_material_editor", ICON_SQUARE_PEN, "Open in Material Editor", false, true, 0, 22.0f, nullptr, IconSize::Row14))
                            {
                                editorState.showMaterialEditor = true;
                                ImGui::SetWindowFocus(kMaterialEditorWindowName);
                            }
                        }

                        PropertyLabel("Visible");
                        {
                            bool visible = renderer->visible;
                            if (Checkbox("##visible", &visible))
                            {
                                const std::string beforeVisibilitySnapshot = CaptureSceneSnapshot();
                                renderer->visible = visible;
                                RecordSceneMutationImmediate("Toggle Renderer Visibility", beforeVisibilitySnapshot);
                            }
                        }
                        EndPropertyGrid();
                    }

                    if (auto materialResource = services_.resourceManager->Load<resource::MaterialAsset>(renderer->materialPath);
                        materialResource != nullptr)
                    {
                        DrawGroupCaption(
                            ICON_PALETTE,
                            style::kTypeMaterial,
                            "Material asset \xC2\xB7 " + std::filesystem::path(renderer->materialPath).filename().string(),
                            20.0f);

                        if (BeginPropertyGrid("##material_asset", 32.0f))
                        {
                            resource::MaterialAsset beforeAsset = CloneMaterialAsset(materialResource->asset);
                            const auto commitMaterialChange = [&](const char* label)
                            {
                                if ((ImGui::IsItemDeactivatedAfterEdit() || !ImGui::IsItemActive()) &&
                                    !MaterialEquals(beforeAsset, materialResource->asset))
                                {
                                    PushMaterialAssetCommand(
                                        label,
                                        renderer->materialPath,
                                        beforeAsset,
                                        CloneMaterialAsset(materialResource->asset));
                                    beforeAsset = CloneMaterialAsset(materialResource->asset);
                                }
                            };

                            PropertyLabel("Texture");
                            {
                                auto textureKeys = services_.resourceManager->GetKnownTextureKeys();
                                const std::string textureLabel = materialResource->asset.texturePath.empty()
                                    ? std::string("None")
                                    : std::filesystem::path(materialResource->asset.texturePath).filename().string();
                                if (BeginAssetPicker("##texture", textureLabel.c_str(), ICON_IMAGE, style::kTypeTexture, materialResource->asset.texturePath.c_str()))
                                {
                                    for (const auto& textureKey : textureKeys)
                                    {
                                        const bool selected = ResourcePathsEqual(*services_.resourceManager, textureKey, materialResource->asset.texturePath);
                                        if (ImGui::Selectable(
                                                PickerItem(ICON_IMAGE, std::filesystem::path(textureKey).filename().string(), textureKey).c_str(),
                                                selected))
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
                                commitMaterialChange("Change Renderer Texture");
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
                                    commitMaterialChange("Change Renderer Tint");
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
                                commitMaterialChange("Change Renderer Tint");
                            }
                            EndPropertyGrid();
                        }
                        DrawInfoLine("Shared asset: changes apply to every entity using it.", 32.0f);
                    }
                    ImGui::EndDisabled();
                    EndCategory();
                }
            }

            // ---- Rigidbody
            if (auto* rigidbody = world.TryGet<ecs::components::RigidbodyComponent>(entity); rigidbody != nullptr)
            {
                if (BeginCategory("Rigidbody"))
                {
                    ImGui::BeginDisabled(!editEnabled);
                    if (BeginPropertyGrid("##rigidbody"))
                    {
                        PropertyLabel("Use Gravity");
                        bool useGravity = rigidbody->useGravity;
                        if (Checkbox("##use_gravity", &useGravity))
                        {
                            const std::string beforeGravitySnapshot = CaptureSceneSnapshot();
                            rigidbody->useGravity = useGravity;
                            RecordSceneMutationImmediate("Toggle Rigidbody Gravity", beforeGravitySnapshot);
                        }

                        PropertyLabel("Is Kinematic");
                        bool isKinematic = rigidbody->isKinematic;
                        if (Checkbox("##is_kinematic", &isKinematic))
                        {
                            const std::string beforeKinematicSnapshot = CaptureSceneSnapshot();
                            rigidbody->isKinematic = isKinematic;
                            RecordSceneMutationImmediate("Toggle Rigidbody Kinematic", beforeKinematicSnapshot);
                        }

                        // Read only: three numbers, no field
                        PropertyLabel("Velocity", true);
                        {
                            char text[96];
                            std::snprintf(
                                text,
                                sizeof(text),
                                "%.3f   %.3f   %.3f",
                                rigidbody->velocity.x,
                                rigidbody->velocity.y,
                                rigidbody->velocity.z);
                            PushFontRole(FontRole::Secondary);
                            ImGui::AlignTextToFramePadding();
                            ImGui::PushStyleColor(ImGuiCol_Text, style::ToVec4(style::kTextDim));
                            ImGui::TextUnformatted(text);
                            ImGui::PopStyleColor();
                            PopFontRole();
                        }
                        EndPropertyGrid();
                    }
                    ImGui::EndDisabled();
                    EndCategory();
                }
            }

            // ---- Collider
            if (auto* collider = world.TryGet<ecs::components::ColliderComponent>(entity); collider != nullptr)
            {
                const char* typeName = collider->type == ecs::components::ColliderType::Sphere ? "Sphere" : "Box";
                if (BeginCategory("Collider", true, typeName))
                {
                    ImGui::BeginDisabled(!editEnabled);
                    if (BeginPropertyGrid("##collider"))
                    {
                        PropertyLabel("Type");
                        const int type = collider->type == ecs::components::ColliderType::Sphere ? 1 : 0;
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (BeginCombo("##type", type == 1 ? "Sphere" : "Box"))
                        {
                            static constexpr const char* kTypeNames[] = {"Box", "Sphere"};
                            for (int index = 0; index < 2; ++index)
                            {
                                if (ImGui::Selectable(kTypeNames[index], index == type) && index != type)
                                {
                                    const std::string beforeTypeSnapshot = CaptureSceneSnapshot();
                                    collider->type = index == 1 ? ecs::components::ColliderType::Sphere : ecs::components::ColliderType::Box;
                                    RecordSceneMutationImmediate("Change Collider Type", beforeTypeSnapshot);
                                }
                            }
                            EndCombo();
                        }

                        if (collider->type == ecs::components::ColliderType::Box)
                        {
                            PropertyLabel("Half Extents");
                            auto halfExtents = ToFloat3(collider->halfExtents);
                            if (DragVector3("##half_extents", halfExtents.data(), 0.02f, "%.3f",
                                    [this](int) { RecordSceneMutationFromItem("Edit Collider Extents"); }))
                            {
                                for (float& component : halfExtents)
                                {
                                    component = std::clamp(component, 0.01f, 100.0f);
                                }
                                FromFloat3(halfExtents, collider->halfExtents);
                            }
                        }
                        else
                        {
                            PropertyLabel("Radius");
                            float radius = collider->radius;
                            if (ImGui::DragFloat("##radius", &radius, 0.02f, 0.01f, 100.0f, "%.3f"))
                            {
                                collider->radius = radius;
                            }
                            FocusOutline();
                            RecordSceneMutationFromItem("Edit Collider Radius");
                        }

                        PropertyLabel("Offset");
                        auto offset = ToFloat3(collider->offset);
                        if (DragVector3("##offset", offset.data(), 0.02f, "%.3f",
                                [this](int) { RecordSceneMutationFromItem("Edit Collider Offset"); }))
                        {
                            FromFloat3(offset, collider->offset);
                        }

                        PropertyLabel("Friction");
                        float friction = collider->friction;
                        if (ImGui::DragFloat("##friction", &friction, 0.01f, 0.0f, 2.0f, "%.3f"))
                        {
                            collider->friction = friction;
                        }
                        FocusOutline();
                        RecordSceneMutationFromItem("Edit Collider Friction");

                        PropertyLabel("Bounciness");
                        float bounciness = collider->bounciness;
                        if (ImGui::DragFloat("##bounciness", &bounciness, 0.01f, 0.0f, 2.0f, "%.3f"))
                        {
                            collider->bounciness = bounciness;
                        }
                        FocusOutline();
                        RecordSceneMutationFromItem("Edit Collider Bounciness");

                        PropertyLabel("Trigger");
                        bool isTrigger = collider->isTrigger;
                        if (Checkbox("##trigger", &isTrigger))
                        {
                            const std::string beforeTriggerSnapshot = CaptureSceneSnapshot();
                            collider->isTrigger = isTrigger;
                            RecordSceneMutationImmediate("Toggle Collider Trigger", beforeTriggerSnapshot);
                        }
                        EndPropertyGrid();
                    }
                    ImGui::EndDisabled();
                    EndCategory();
                }
            }

            // ---- Camera
            if (auto* camera = world.TryGet<ecs::components::CameraComponent>(entity); camera != nullptr)
            {
                if (BeginCategory("Camera"))
                {
                    ImGui::BeginDisabled(!editEnabled);
                    if (BeginPropertyGrid("##camera"))
                    {
                        PropertyLabel("Position");
                        auto position = ToFloat3(camera->position);
                        if (DragVector3("##camera_position", position.data(), 0.05f, "%.3f",
                                [this](int) { RecordSceneMutationFromItem("Edit Camera Position"); }))
                        {
                            FromFloat3(position, camera->position);
                        }

                        PropertyLabel("Rotation");
                        auto rotation = ToFloat3(camera->rotationDeg);
                        if (DragVector3("##camera_rotation", rotation.data(), 0.5f, "%.3f",
                                [this](int) { RecordSceneMutationFromItem("Edit Camera Rotation"); }))
                        {
                            FromFloat3(rotation, camera->rotationDeg);
                        }

                        PropertyLabel("FOV");
                        float fovYDeg = camera->fovYDeg;
                        if (ImGui::DragFloat("##fov", &fovYDeg, 0.2f, 15.0f, 160.0f, "%.1f"))
                        {
                            camera->fovYDeg = fovYDeg;
                        }
                        FocusOutline();
                        RecordSceneMutationFromItem("Edit Camera FOV");

                        PropertyLabel("Primary");
                        bool isPrimary = camera->isPrimary;
                        if (Checkbox("##primary", &isPrimary))
                        {
                            const std::string beforePrimarySnapshot = CaptureSceneSnapshot();
                            camera->isPrimary = isPrimary;
                            RecordSceneMutationImmediate("Toggle Camera Primary", beforePrimarySnapshot);
                        }
                        EndPropertyGrid();
                    }
                    ImGui::EndDisabled();
                    EndCategory();
                }
            }

            // ---- Scripts: fields declared in the script class (speed: float = 3.0).
            // Edit: values are stored in the scene and used on the next Play. Play: current values, read-only
            if (auto* script = world.TryGet<ecs::components::ScriptComponent>(entity); script != nullptr)
            {
                if (BeginCategory("Scripts"))
                {
                    ui::ScriptFieldsUndo undo;
                    undo.recordFromItem = [this](const char* label) { RecordSceneMutationFromItem(label); };
                    undo.captureBefore = [this]() { return CaptureSceneSnapshot(); };
                    undo.recordImmediate = [this](const char* label, const std::string& before) { RecordSceneMutationImmediate(label, before); };

                    for (std::size_t index = 0; index < script->scripts.size(); ++index)
                    {
                        auto& entry = script->scripts[index];
                        ImGui::PushID(static_cast<int>(index));

                        ui::ScriptHeaderChip chip = ui::ScriptHeaderChip::AppliedOnPlay;
                        if (!editEnabled)
                        {
                            const std::string status = services_.scriptStatus ? services_.scriptStatus(entity, index) : std::string();
                            if (status == "Faulted")
                            {
                                chip = ui::ScriptHeaderChip::Faulted;
                            }
                            else if (status == "Active")
                            {
                                chip = ui::ScriptHeaderChip::Active;
                            }
                            else
                            {
                                chip = ui::ScriptHeaderChip::NotCreated;
                            }
                        }

                        if (ui::DrawScriptHeader(entry.module, entry.className, chip))
                        {
                            // "Open log": bring the log window forward (the errors are listed there)
                            editorState.showScriptConsole = true;
                            ImGui::SetWindowFocus(kScriptConsoleWindowName);
                        }

                        const auto fields = services_.describeScriptFields
                            ? services_.describeScriptFields(entry.module, entry.className)
                            : std::vector<scripting::ScriptFieldInfo>{};
                        if (fields.empty())
                        {
                            DrawInfoLine("No editable fields, or the script failed to load (see the log).", 20.0f);
                            DrawInfoLine("A field is a class attribute with a type: speed: float = 3.0", 20.0f);
                        }
                        else if (editEnabled)
                        {
                            ui::DrawScriptFields(fields, entry.props, undo);
                        }
                        else
                        {
                            // Play: the live object's values; editing is off, changes would be lost on Stop anyway
                            const auto live = services_.liveScriptFields ? services_.liveScriptFields(entity, index) : nlohmann::json::object();
                            ui::DrawScriptFieldValues(fields, live);
                        }

                        ImGui::Dummy(ImVec2(0.0f, 4.0f));
                        ImGui::PopID();
                    }
                    EndCategory();
                }
            }
        }
        ImGui::End();
    }
}
