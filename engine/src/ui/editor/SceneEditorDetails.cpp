#include "SceneEditorInternal.h"

namespace myengine::ui
{
    using namespace detail;

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
                            const bool selected = ResourcePathsEqual(*services_.resourceManager, meshKey, renderer->meshPath);
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
                            const bool selected = ResourcePathsEqual(*services_.resourceManager, materialKey, renderer->materialPath);
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
                                const bool selected = ResourcePathsEqual(*services_.resourceManager, textureKey, materialResource->asset.texturePath);
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

            // Script behaviours: fields declared in the script class (speed: float = 3.0).
            // Edit: values are stored in the scene and used on the next Play. Play: current values, read-only
            if (auto* script = world.TryGet<ecs::components::ScriptComponent>(entity); script != nullptr)
            {
                if (ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (!editEnabled)
                    {
                        ImGui::EndDisabled(); // the status line stays readable in Play
                    }

                    ui::ScriptFieldsUndo undo;
                    undo.recordFromItem = [this](const char* label) { RecordSceneMutationFromItem(label); };
                    undo.captureBefore = [this]() { return CaptureSceneSnapshot(); };
                    undo.recordImmediate = [this](const char* label, const std::string& before) { RecordSceneMutationImmediate(label, before); };

                    for (std::size_t index = 0; index < script->scripts.size(); ++index)
                    {
                        auto& entry = script->scripts[index];
                        ImGui::PushID(static_cast<int>(index));

                        if (index > 0)
                        {
                            ImGui::Separator();
                        }
                        ImGui::Text("%s.%s", entry.module.c_str(), entry.className.c_str());

                        const std::string status = editEnabled || !services_.scriptStatus ? std::string() : services_.scriptStatus(entity, index);
                        ImGui::SameLine();
                        if (editEnabled)
                        {
                            ImGui::TextDisabled("(applied on Play)");
                        }
                        else if (status == "Faulted")
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Faulted - see the log");
                        }
                        else
                        {
                            ImGui::TextDisabled("%s", status.empty() ? "not created" : status.c_str());
                        }

                        const auto fields = services_.describeScriptFields
                            ? services_.describeScriptFields(entry.module, entry.className)
                            : std::vector<scripting::ScriptFieldInfo>{};
                        if (fields.empty())
                        {
                            ImGui::TextDisabled("No editable fields, or the script failed to load (see the log).");
                            ImGui::TextDisabled("A field is a class attribute with a type: speed: float = 3.0");
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

                        ImGui::PopID();
                    }

                    if (!editEnabled)
                    {
                        ImGui::BeginDisabled();
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
}
