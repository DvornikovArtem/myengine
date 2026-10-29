#include <myengine/faceid/FaceIdGate.h>

#include <imgui/imgui.h>

#include <filesystem>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace myengine::faceid
{
    namespace
    {
        constexpr ImVec4 kTextDim(0.62f, 0.66f, 0.74f, 1.0f);
        constexpr ImVec4 kGood(0.35f, 0.85f, 0.50f, 1.0f);
        constexpr ImVec4 kBad(0.95f, 0.38f, 0.38f, 1.0f);
        constexpr ImVec4 kAccent(0.30f, 0.62f, 1.0f, 1.0f);

        ImU32 Color(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

        bool InputPassword(const char* label, char* buffer, const std::size_t size, const bool enterReturns = false)
        {
            ImGuiInputTextFlags flags = ImGuiInputTextFlags_Password;
            if (enterReturns)
            {
                flags |= ImGuiInputTextFlags_EnterReturnsTrue;
            }
            return ImGui::InputText(label, buffer, size, flags);
        }

        void WipeBuffer(char* buffer, const std::size_t size) { std::memset(buffer, 0, size); }

        std::string Trimmed(const char* text)
        {
            std::string value(text);
            const auto begin = value.find_first_not_of(" \t");
            if (begin == std::string::npos)
            {
                return {};
            }
            return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
        }

        std::filesystem::path PathOf(const char* text) { return std::filesystem::u8path(Trimmed(text)); }

        void SetText(char* buffer, const std::size_t size, const std::string& text)
        {
            std::snprintf(buffer, size, "%s", text.c_str());
        }

        const char* Capital(const LoginPhase phase)
        {
            switch (phase)
            {
            case LoginPhase::Granted: return "Welcome";
            case LoginPhase::Denied: return "Access denied";
            case LoginPhase::Locked: return "Locked";
            default: return "Face ID";
            }
        }
    }

    FaceIdGate::FaceIdGate(FaceIdService& service) : service_(service) {}

    void FaceIdGate::Begin()
    {
        screen_ = Screen::None;
        passwordMode_ = false;
        if (!service_.Available() || !service_.Settings().enabled)
        {
            return;
        }
        if (service_.Source() == FaceSource::Shared)
        {
            // the shared database decides: no offer to record a face here, and a database that did not pass closes the door
            fromSettings_ = false;
            if (service_.SharedBlocked())
            {
                screen_ = Screen::Rejected;
                return;
            }
            screen_ = Screen::Login;
            service_.BeginLogin();
            return;
        }
        if (service_.Store().Profiles().empty())
        {
            screen_ = Screen::FirstRun;
            fromSettings_ = false;
            return;
        }
        screen_ = Screen::Login;
        service_.BeginLogin();
    }

    void FaceIdGate::StartEnrolFlow(const bool shared)
    {
        enrolShared_ = shared;
        consentChecked_ = false;
        detailsError_.clear();
        WipeBuffer(nameBuffer_, sizeof(nameBuffer_));
        WipeBuffer(newPasswordBuffer_, sizeof(newPasswordBuffer_));
        WipeBuffer(repeatPasswordBuffer_, sizeof(repeatPasswordBuffer_));
        screen_ = Screen::Consent;
    }

    void FaceIdGate::LeaveEnrol()
    {
        service_.Stop();
        WipeBuffer(newPasswordBuffer_, sizeof(newPasswordBuffer_));
        WipeBuffer(repeatPasswordBuffer_, sizeof(repeatPasswordBuffer_));
        if (fromSettings_ || service_.ActiveProfiles().empty())
        {
            screen_ = Screen::None;
        }
        else
        {
            screen_ = Screen::Login;
            service_.BeginLogin();
        }
        fromSettings_ = false;
    }

    void FaceIdGate::BeginPanel(const FaceIdUiContext& context, const char* title, const float width)
    {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(context.width, context.height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.09f, 1.0f));
        ImGui::Begin("##faceid_gate", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoCollapse);
        ImGui::SetWindowFocus();
        const float left = std::max(24.0f, (context.width - width) * 0.5f);
        ImGui::SetCursorPos(ImVec2(left, std::max(24.0f, context.height * 0.08f)));
        ImGui::BeginGroup();
        ImGui::TextColored(kAccent, "MYENGINE");
        ImGui::Text("%s", title);
        ImGui::Separator();
        ImGui::PushItemWidth(width);
    }

    void FaceIdGate::EndPanel()
    {
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    void FaceIdGate::DrawPreview(const FaceIdUiContext& context, const FaceIdView& view, const float width)
    {
        if (view.hasFrame && context.uploadPreview)
        {
            frameWidth_ = view.frame.width;
            frameHeight_ = view.frame.height;
            texture_ = context.uploadPreview(view.frame, true);
            textureSerial_ = view.frameSerial;
        }
        const float height = width * static_cast<float>(frameHeight_) / static_cast<float>(std::max(frameWidth_, 1));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        if (texture_ != 0)
        {
            drawList->AddImage(static_cast<ImTextureID>(texture_), origin, ImVec2(origin.x + width, origin.y + height));
        }
        else
        {
            drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), Color(ImVec4(0.09f, 0.10f, 0.14f, 1.0f)));
        }
        if (view.hasFace)
        {
            // the picture is mirrored, so are the box and the points
            const float scale = width / static_cast<float>(std::max(frameWidth_, 1));
            const float x1 = origin.x + (static_cast<float>(frameWidth_) - (view.face.x + view.face.w)) * scale;
            const float x2 = origin.x + (static_cast<float>(frameWidth_) - view.face.x) * scale;
            const float y1 = origin.y + view.face.y * scale;
            const float y2 = origin.y + (view.face.y + view.face.h) * scale;
            const bool good = view.phase == LoginPhase::Granted || (view.mode == FaceIdMode::Login && view.phase == LoginPhase::Recognising);
            const bool bad = view.phase == LoginPhase::Denied;
            const ImVec4 color = bad ? kBad : (good ? kGood : kAccent);
            drawList->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), Color(color), 6.0f, 0, 2.5f);
            for (int n = 0; n < 5; ++n)
            {
                drawList->AddCircleFilled(ImVec2(origin.x + (static_cast<float>(frameWidth_) - view.face.landmarks[2 * n]) * scale, origin.y + view.face.landmarks[2 * n + 1] * scale), 2.5f, Color(color));
            }
        }
        ImGui::Dummy(ImVec2(width, height));
    }

    bool FaceIdGate::Draw(const FaceIdUiContext& context)
    {
        switch (screen_)
        {
        case Screen::None: return false;
        case Screen::FirstRun: DrawFirstRun(context); break;
        case Screen::Consent: DrawConsent(context); break;
        case Screen::Details: DrawDetails(context); break;
        case Screen::Capturing: DrawCapturing(context); break;
        case Screen::Saved: DrawSaved(context); break;
        case Screen::Login: DrawLogin(context); break;
        case Screen::Rejected: DrawRejected(context); break;
        }
        return screen_ != Screen::None;
    }

    void FaceIdGate::DrawFirstRun(const FaceIdUiContext& context)
    {
        BeginPanel(context, "Sign in with your face", 560.0f);
        ImGui::Spacing();
        ImGui::TextWrapped("Face ID lets you into the editor by looking at the camera of this computer. It is a demo, not a lock: it keeps out a stranger who just sits down at the keyboard, "
                           "not a determined one. Up to three people can be recorded. Nothing leaves this computer.");
        ImGui::Spacing();
        if (ImGui::Button("Set up Face ID", ImVec2(180.0f, 34.0f)))
        {
            fromSettings_ = false;
            StartEnrolFlow();
        }
        ImGui::SameLine();
        if (ImGui::Button("Not now", ImVec2(120.0f, 34.0f)))
        {
            service_.Settings().enabled = false;   // it can be switched on again in Window > Face ID
            service_.SaveSettings();
            screen_ = Screen::None;
        }
        ImGui::TextColored(kTextDim, "Not now switches the login off. Window > Face ID switches it on again.");
        EndPanel();
    }

    void FaceIdGate::DrawConsent(const FaceIdUiContext& context)
    {
        BeginPanel(context, enrolShared_ ? "Add a face to the shared database: what is stored" : "Add a face: what is stored", 640.0f);
        ImGui::Spacing();
        if (enrolShared_)
        {
            ImGui::TextWrapped("The editor will look at the person through the camera and keep a short list of numbers (an embedding) for eight pictures of the face. No picture is saved. "
                               "The numbers go into the shared face database: the file allowed_faces.fdb that you sign and give to everybody who gets this engine.");
            ImGui::TextWrapped("The file is protected against a change, NOT against reading: whoever has it has these numbers. They cannot be turned back into a picture, but they are a "
                               "biometric template. Record only a person who has agreed, and delete the face from the database when they ask.");
            ImGui::Spacing();
            ImGui::Checkbox("The person has agreed to be recorded in a database that is given to other people", &consentChecked_);
        }
        else
        {
            ImGui::TextWrapped("The editor will look at you through the camera and keep a short list of numbers (an embedding) for eight pictures of your face. No picture is saved: the "
                               "numbers are stored only on this computer, in %%LOCALAPPDATA%%\\myengine\\faceid, encrypted with your Windows account (DPAPI), and are never sent anywhere.");
            ImGui::TextWrapped("You can delete a face at any time in Window > Face ID: the data is erased from the file at once.");
            ImGui::Spacing();
            ImGui::Checkbox("I agree: this is my face or the person has agreed to be recorded", &consentChecked_);
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!consentChecked_);
        if (ImGui::Button("Continue", ImVec2(140.0f, 34.0f)))
        {
            detailsError_.clear();
            screen_ = Screen::Details;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 34.0f)))
        {
            LeaveEnrol();
        }
        EndPanel();
    }

    void FaceIdGate::DrawDetails(const FaceIdUiContext& context)
    {
        const bool firstProfile = !enrolShared_ && !service_.Store().HasPassword();
        BeginPanel(context, "Add a face: who is this", 520.0f);
        ImGui::Spacing();
        ImGui::Text("Name (shown at the login):");
        ImGui::InputText("##name", nameBuffer_, sizeof(nameBuffer_));
        if (firstProfile)
        {
            ImGui::Spacing();
            ImGui::TextWrapped("The password is for the days when the camera does not work or does not know you. It is kept only as a hash.");
            ImGui::Text("Password (6 characters or more):");
            InputPassword("##password", newPasswordBuffer_, sizeof(newPasswordBuffer_));
            ImGui::Text("Once more:");
            InputPassword("##repeat", repeatPasswordBuffer_, sizeof(repeatPasswordBuffer_));
        }
        if (!detailsError_.empty())
        {
            ImGui::TextColored(kBad, "%s", detailsError_.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("Start the camera", ImVec2(180.0f, 34.0f)))
        {
            const std::string name = Trimmed(nameBuffer_);
            if (name.empty() || name.size() > FaceProfileStore::kMaxNameBytes)
            {
                detailsError_ = "Type a name (up to 32 characters).";
            }
            else if (enrolShared_ ? std::any_of(service_.Draft().faces.begin(), service_.Draft().faces.end(), [&](const SharedFace& f) { return f.name == name; })
                                  : std::any_of(service_.Store().Profiles().begin(), service_.Store().Profiles().end(), [&](const FaceProfile& p) { return p.name == name; }))
            {
                detailsError_ = "There is a face with this name already.";
            }
            else if (firstProfile && std::strlen(newPasswordBuffer_) < 6)
            {
                detailsError_ = "The password needs 6 characters or more.";
            }
            else if (firstProfile && std::strcmp(newPasswordBuffer_, repeatPasswordBuffer_) != 0)
            {
                detailsError_ = "The two passwords are not the same.";
            }
            else
            {
                detailsError_.clear();
                service_.BeginEnroll();
                screen_ = Screen::Capturing;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 34.0f)))
        {
            LeaveEnrol();
        }
        EndPanel();
    }

    void FaceIdGate::DrawCapturing(const FaceIdUiContext& context)
    {
        const FaceIdView view = service_.View(true, textureSerial_);
        std::vector<FaceEmbedding> embeddings;
        if (view.enrolReady && service_.TakeEnrolled(embeddings))
        {
            std::string error;
            if (enrolShared_)
            {
                savedOk_ = service_.DraftAddFace(Trimmed(nameBuffer_), std::move(embeddings), &error);
                savedText_ = savedOk_ ? "Added to the draft of the shared database. It is in the file only after \"Sign and save the database\" (Window > Face ID > Publisher)."
                                      : "Not added: " + error;
            }
            else
            {
                FaceProfile profile;
                profile.name = Trimmed(nameBuffer_);
                profile.embeddings = std::move(embeddings);
                savedOk_ = service_.Store().Add(std::move(profile), &error, newPasswordBuffer_);
                savedText_ = savedOk_ ? "Saved. From now on the editor knows you by your face." : "Not saved: " + error;
            }
            service_.Stop();
            WipeBuffer(newPasswordBuffer_, sizeof(newPasswordBuffer_));
            WipeBuffer(repeatPasswordBuffer_, sizeof(repeatPasswordBuffer_));
            screen_ = Screen::Saved;
            return;
        }
        BeginPanel(context, "Add a face: look at the camera", 640.0f);
        DrawPreview(context, view, 640.0f);
        if (view.cameraFailed)
        {
            ImGui::TextColored(kBad, "%s", view.cameraStatus.c_str());
        }
        else
        {
            ImGui::Text("%s", view.text.c_str());
        }
        ImGui::ProgressBar(static_cast<float>(view.captured) / static_cast<float>(std::max(view.target, 1)), ImVec2(640.0f, 18.0f));
        ImGui::TextColored(kTextDim, "%d of %d faces. Straight twice, then slowly to one side and to the other.", view.captured, view.target);
        if (ImGui::Button("Cancel", ImVec2(120.0f, 34.0f)))
        {
            LeaveEnrol();
        }
        EndPanel();
    }

    void FaceIdGate::DrawSaved(const FaceIdUiContext& context)
    {
        BeginPanel(context, savedOk_ ? "Face recorded" : "Face not recorded", 520.0f);
        ImGui::Spacing();
        ImGui::TextColored(savedOk_ ? kGood : kBad, "%s", savedText_.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Continue", ImVec2(140.0f, 34.0f)))
        {
            if (savedOk_ && !fromSettings_)
            {
                // the first profile was made by the offer at the start: the editor opens (the person is the one who just looked at the camera)
                screen_ = Screen::None;
            }
            else
            {
                LeaveEnrol();
            }
            fromSettings_ = false;
        }
        EndPanel();
    }

    void FaceIdGate::DrawRejected(const FaceIdUiContext& context)
    {
        BeginPanel(context, "Face database rejected", 640.0f);
        ImGui::Spacing();
        ImGui::TextColored(kBad, "%s", service_.Database().error.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("The editor lets in only the faces of a database that its publisher signed. This one did not pass the check, so no face and no password is accepted until "
                           "the right allowed_faces.fdb is in place.");
        if (!service_.Database().path.empty())
        {
            ImGui::TextColored(kTextDim, "%s", service_.Database().path.string().c_str());
        }
        if (service_.IsPublisher())
        {
            ImGui::Spacing();
            ImGui::TextWrapped("This computer has the publisher key: you can open the editor to repair the database (Window > Face ID > Publisher).");
            if (ImGui::Button("I am the publisher: open the editor", ImVec2(300.0f, 34.0f)))
            {
                screen_ = Screen::None;
                OpenSettings();
            }
        }
        EndPanel();
    }

    void FaceIdGate::DrawLogin(const FaceIdUiContext& context)
    {
        const FaceIdView view = service_.View(true, textureSerial_);
        if (view.phase == LoginPhase::Granted)
        {
            if (grantedAt_ == 0.0)
            {
                grantedAt_ = context.time;
            }
            if (context.time - grantedAt_ > 0.9)
            {
                grantedAt_ = 0.0;
                service_.Stop();
                WipeBuffer(passwordBuffer_, sizeof(passwordBuffer_));
                screen_ = Screen::None;
                return;
            }
        }
        BeginPanel(context, Capital(view.phase), 640.0f);
        if (!passwordMode_)
        {
            DrawPreview(context, view, 640.0f);
            std::string line = view.text;
            ImVec4 color = kTextDim;
            switch (view.phase)
            {
            case LoginPhase::NoCamera:
                line = view.cameraStatus.empty() ? "the camera is not available" : view.cameraStatus;
                color = kBad;
                break;
            case LoginPhase::Challenge:
            {
                // the picture is mirrored: the way the nose goes on the screen is the way the person turns their head
                const bool toRight = view.challengeDirection == LivenessChallenge::Direction::ImageLeft;
                line = std::string("turn your head slowly to your ") + (toRight ? "right" : "left") + " and back";
                color = kAccent;
                break;
            }
            case LoginPhase::Granted:
                line = "Hello, " + view.grantedName;
                color = kGood;
                break;
            case LoginPhase::Denied:
                color = kBad;
                break;
            case LoginPhase::Locked:
            {
                char buffer[96];
                std::snprintf(buffer, sizeof(buffer), "three attempts failed: wait %d s", static_cast<int>(std::ceil(view.lockRemaining)));
                line = buffer;
                color = kBad;
                break;
            }
            default: break;
            }
            ImGui::TextColored(color, "%s", line.c_str());
            if (view.phase == LoginPhase::Challenge)
            {
                ImGui::ProgressBar(view.challengeProgress, ImVec2(640.0f, 12.0f), "");
            }
            else if (view.phase == LoginPhase::Recognising)
            {
                ImGui::ProgressBar(static_cast<float>(view.streak) / static_cast<float>(std::max(view.streakNeeded, 1)), ImVec2(640.0f, 12.0f), "");
            }
            else
            {
                ImGui::Dummy(ImVec2(640.0f, 12.0f));
            }
            if (service_.PasswordAvailable())
            {
                if (ImGui::Button("Use the password", ImVec2(180.0f, 32.0f)) || view.phase == LoginPhase::NoCamera)
                {
                    passwordMode_ = view.phase != LoginPhase::Granted;
                    passwordError_.clear();
                }
            }
            else if (view.phase == LoginPhase::NoCamera)
            {
                ImGui::TextColored(kBad, "There is no password in this face database: the camera is the only way in.");
            }
        }
        if (passwordMode_)
        {
            const bool noCamera = view.phase == LoginPhase::NoCamera;
            if (noCamera)
            {
                ImGui::TextColored(kBad, "%s", view.cameraStatus.empty() ? "the camera is not available" : view.cameraStatus.c_str());
            }
            ImGui::Spacing();
            ImGui::Text("Password:");
            const bool locked = view.phase == LoginPhase::Locked;
            ImGui::BeginDisabled(locked);
            ImGui::SetKeyboardFocusHere();
            const bool entered = InputPassword("##loginpw", passwordBuffer_, sizeof(passwordBuffer_), true);
            if ((entered || ImGui::Button("Enter", ImVec2(120.0f, 32.0f))) && !locked)
            {
                if (service_.LoginByPassword(passwordBuffer_))
                {
                    passwordError_.clear();
                }
                else
                {
                    passwordError_ = "Wrong password.";
                }
                WipeBuffer(passwordBuffer_, sizeof(passwordBuffer_));
            }
            ImGui::EndDisabled();
            if (locked)
            {
                ImGui::TextColored(kBad, "three attempts failed: wait %d s", static_cast<int>(std::ceil(view.lockRemaining)));
            }
            else if (!passwordError_.empty())
            {
                ImGui::TextColored(kBad, "%s", passwordError_.c_str());
            }
            if (!noCamera && ImGui::Button("Back to the camera", ImVec2(180.0f, 32.0f)))
            {
                passwordMode_ = false;
                WipeBuffer(passwordBuffer_, sizeof(passwordBuffer_));
            }
        }
        if (view.phase == LoginPhase::Denied)
        {
            ImGui::TextColored(kTextDim, "The next attempt starts in a moment.");
        }
        EndPanel();
    }

    void FaceIdGate::DrawSettings(const FaceIdUiContext& context)
    {
        (void)context;
        if (!settingsOpen_)
        {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(560.0f, 640.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Face ID", &settingsOpen_))
        {
            ImGui::End();
            return;
        }
        FaceIdSettings& settings = service_.Settings();
        if (!service_.Available())
        {
            ImGui::TextColored(kBad, "Face ID is not available in this build:");
            ImGui::TextWrapped("%s", service_.UnavailableReason().c_str());
            ImGui::End();
            return;
        }
        if (!service_.ProfilesError().empty())
        {
            ImGui::TextColored(kBad, "%s", service_.ProfilesError().c_str());
            if (ImGui::Button("Erase the damaged file and start again"))
            {
                service_.Store().EraseAll();
                service_.Initialize();
            }
            ImGui::Separator();
        }
        bool changed = false;
        const bool shared = service_.Source() == FaceSource::Shared;
        if (shared)
        {
            ImGui::TextColored(kAccent, "Mode: Shared database");
            ImGui::TextWrapped("Only the faces of the database that its publisher signed get in. Faces cannot be recorded on this computer.");
        }
        else
        {
            ImGui::TextColored(kAccent, "Mode: Local");
            ImGui::TextWrapped("The faces of this computer, encrypted with your Windows account. A shared database is used instead when allowed_faces.fdb is found (--faceid-db, "
                               "faceid\\allowed_faces.fdb next to the exe, assets\\faceid\\allowed_faces.fdb).");
        }
        ImGui::Separator();
        changed |= ImGui::Checkbox("Ask for Face ID when the editor starts", &settings.enabled);
        changed |= ImGui::Checkbox("Ask to turn the head (a photo of a face does not turn)", &settings.liveness);
        changed |= ImGui::Checkbox("Ask in the games too (Rocket Arena, FlyDoom)", &settings.games);
        ImGui::TextColored(kTextDim, "The games read this setting; the editor login works already.");
        if (service_.IsPublisher())
        {
            if (ImGui::Checkbox("Use the local faces instead of the shared database (publisher only)", &settings.forceLocal))
            {
                service_.SaveSettings();
                service_.ReloadDatabase();
            }
        }
        if (settingsRefreshCameras_)
        {
            cameras_ = FaceCamera::List();
            settingsRefreshCameras_ = false;
        }
        const char* current = settings.camera.empty() ? (cameras_.empty() ? "(no camera)" : cameras_.front().name.c_str()) : settings.camera.c_str();
        if (ImGui::BeginCombo("Camera", current))
        {
            for (const CameraInfo& camera : cameras_)
            {
                if (ImGui::Selectable(camera.name.c_str(), camera.name == settings.camera))
                {
                    settings.camera = camera.name;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Refresh"))
        {
            settingsRefreshCameras_ = true;
        }
        changed |= ImGui::SliderFloat("Threshold", &settings.threshold, 0.25f, 0.70f, "%.3f");
        ImGui::TextColored(kTextDim, "Cosine similarity of SFace: %.3f is the value of OpenCV. Higher is stricter.", static_cast<double>(kDefaultThreshold));
        if (changed)
        {
            service_.SaveSettings();
        }
        ImGui::Separator();
        if (shared)
        {
            DrawSettingsShared();
        }
        else
        {
            DrawSettingsLocal();
        }
        ImGui::Separator();
        DrawPublisher();
        ImGui::End();
    }

    void FaceIdGate::DrawSettingsShared()
    {
        const DatabaseInfo& info = service_.Database();
        ImGui::TextColored(kTextDim, "%s", info.path.string().c_str());
        if (!info.valid)
        {
            ImGui::TextColored(kBad, "%s", info.error.c_str());
            return;
        }
        ImGui::Text("Signed %s, %d faces, password: %s", info.created.c_str(), static_cast<int>(info.faces), info.hasPassword ? "yes" : "no");
        ImGui::TextColored(kTextDim, info.signedByBuiltInKey ? "Signed with the key that is built into the engine." : "Checked with the key of this computer (no key is built into the engine yet).");
        if (service_.IsPublisher())
        {
            ImGui::TextColored(kTextDim, "The faces are edited in the Publisher section below.");
            return;
        }
        for (const SharedFace& face : service_.Draft().faces)
        {
            ImGui::Text("%s  (%d pictures, recorded %s)", face.name.c_str(), static_cast<int>(face.embeddings.size()), face.date.c_str());
        }
    }

    void FaceIdGate::DrawSettingsLocal()
    {
        ImGui::Text("Faces (%d of %d)", static_cast<int>(service_.Store().Profiles().size()), static_cast<int>(FaceProfileStore::kMaxProfiles));
        const auto& profiles = service_.Store().Profiles();
        for (std::size_t i = 0; i < profiles.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s  (%d faces)", profiles[i].name.c_str(), static_cast<int>(profiles[i].embeddings.size()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete"))
            {
                pendingDelete_ = static_cast<int>(i);
                ImGui::OpenPopup("Delete the face?");
            }
            ImGui::PopID();
        }
        if (ImGui::BeginPopupModal("Delete the face?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const std::string name = pendingDelete_ >= 0 && pendingDelete_ < static_cast<int>(profiles.size()) ? profiles[static_cast<std::size_t>(pendingDelete_)].name : std::string();
            ImGui::Text("The data of \"%s\" is erased from this computer.", name.c_str());
            if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f)))
            {
                std::string error;
                if (!service_.Store().Remove(static_cast<std::size_t>(std::max(pendingDelete_, 0)), &error))
                {
                    settingsMessage_ = error;
                }
                pendingDelete_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep", ImVec2(120.0f, 0.0f)))
            {
                pendingDelete_ = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::BeginDisabled(service_.Store().Full());
        if (ImGui::Button("Add a face ..."))
        {
            fromSettings_ = true;
            StartEnrolFlow();
        }
        ImGui::EndDisabled();
        if (service_.Store().Full())
        {
            ImGui::SameLine();
            ImGui::TextColored(kTextDim, "three faces are the most: delete one first");
        }
        if (!profiles.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button("Erase all Face ID data"))
            {
                ImGui::OpenPopup("Erase everything?");
            }
        }
        if (ImGui::BeginPopupModal("Erase everything?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("All faces and the password are erased from this computer.");
            if (ImGui::Button("Erase", ImVec2(120.0f, 0.0f)))
            {
                service_.Store().EraseAll();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep", ImVec2(120.0f, 0.0f)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (!profiles.empty())
        {
            ImGui::Separator();
            ImGui::Text("Change the password:");
            InputPassword("##newpw", settingsPassword_, sizeof(settingsPassword_));
            ImGui::SameLine();
            if (ImGui::Button("Set"))
            {
                std::string error;
                settingsMessage_ = service_.Store().ChangePassword(settingsPassword_, &error) ? "Password changed." : error;
                WipeBuffer(settingsPassword_, sizeof(settingsPassword_));
            }
        }
        if (!settingsMessage_.empty())
        {
            ImGui::TextColored(kTextDim, "%s", settingsMessage_.c_str());
        }
    }

    void FaceIdGate::DrawPublisher()
    {
        if (!ImGui::CollapsingHeader("Publisher (shared database)", service_.IsPublisher() ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            return;
        }
        FaceSigner& signer = service_.Signer();
        const auto say = [&](const bool good, const std::string& text)
        {
            publisherMessageGood_ = good;
            publisherMessage_ = text;
        };
        if (!service_.SignerError().empty())
        {
            ImGui::TextColored(kBad, "%s", service_.SignerError().c_str());
        }
        if (!signer.HasKey())
        {
            ImGui::TextWrapped("The publisher records the faces that every copy of the engine lets in and signs them. The key is made once, stays on this computer (encrypted with "
                               "your Windows account) and never goes to the repository, a package or the log.");
            if (ImGui::Button("Create the publisher key"))
            {
                std::string error;
                if (signer.Generate(&error))
                {
                    service_.ReloadDatabase();
                    say(true, "The key is made. Copy the public key and put it into the engine, then make a backup of the key.");
                }
                else
                {
                    say(false, error);
                }
            }
            ImGui::Spacing();
            ImGui::Text("Or take the key from a backup:");
            ImGui::InputTextWithHint("##importpath", "the backup file", importPath_, sizeof(importPath_));
            InputPassword("##importpassword", importPassword_, sizeof(importPassword_));
            ImGui::SameLine();
            if (ImGui::Button("Import the key"))
            {
                std::string error;
                if (signer.Import(PathOf(importPath_), importPassword_, &error))
                {
                    service_.ReloadDatabase();
                    say(true, "The key is imported.");
                }
                else
                {
                    say(false, error);
                }
                WipeBuffer(importPassword_, sizeof(importPassword_));
            }
        }
        else
        {
            const std::string hex = ToHex(signer.PublicKey());
            ImGui::Text("Public key (ECDSA P-256):");
            char hexBuffer[kPublicKeySize * 2 + 8];
            SetText(hexBuffer, sizeof(hexBuffer), hex);
            ImGui::InputText("##publickey", hexBuffer, sizeof(hexBuffer), ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button("Copy the public key"))
            {
                ImGui::SetClipboardText(hex.c_str());
                say(true, "Copied. Put it into the engine: tools\\faceid\\set_trusted_key.ps1 <the key>, then build again.");
            }
            ImGui::TextWrapped("%s", service_.PublisherKeyNote().c_str());
            ImGui::Spacing();
            ImGui::Text("Backup of the private key (a file under a password):");
            ImGui::InputTextWithHint("##exportpath", "a new file, outside the repository", exportPath_, sizeof(exportPath_));
            InputPassword("##exportpassword", exportPassword_, sizeof(exportPassword_));
            ImGui::SameLine();
            if (ImGui::Button("Export the key"))
            {
                std::string error;
                if (signer.Export(PathOf(exportPath_), exportPassword_, &error))
                {
                    say(true, "The backup is written. Keep the file and the password apart from the repository.");
                }
                else
                {
                    say(false, error);
                }
                WipeBuffer(exportPassword_, sizeof(exportPassword_));
            }
        }
        if (service_.IsPublisher())
        {
            ImGui::Separator();
            const FaceDatabase& draft = service_.Draft();
            ImGui::Text("Faces in the shared database (%d of %d)%s", static_cast<int>(draft.faces.size()), static_cast<int>(FaceDatabase::kMaxFaces),
                        service_.DraftDirty() ? "  - not signed yet" : "");
            for (std::size_t i = 0; i < draft.faces.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("%s  (%d pictures, recorded %s)", draft.faces[i].name.c_str(), static_cast<int>(draft.faces[i].embeddings.size()), draft.faces[i].date.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete"))
                {
                    pendingDraftDelete_ = static_cast<int>(i);
                    ImGui::OpenPopup("Delete the face from the database?");
                }
                ImGui::PopID();
            }
            if (ImGui::BeginPopupModal("Delete the face from the database?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                const std::string name = pendingDraftDelete_ >= 0 && pendingDraftDelete_ < static_cast<int>(draft.faces.size())
                                             ? draft.faces[static_cast<std::size_t>(pendingDraftDelete_)].name
                                             : std::string();
                ImGui::Text("\"%s\" leaves the draft. Copies of the engine that already have the old file keep it until you give them the new one.", name.c_str());
                if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f)))
                {
                    service_.DraftRemoveFace(static_cast<std::size_t>(std::max(pendingDraftDelete_, 0)));
                    pendingDraftDelete_ = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Keep", ImVec2(120.0f, 0.0f)))
                {
                    pendingDraftDelete_ = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::BeginDisabled(draft.faces.size() >= FaceDatabase::kMaxFaces);
            if (ImGui::Button("Record a face into the shared database ..."))
            {
                fromSettings_ = true;
                StartEnrolFlow(true);
            }
            ImGui::EndDisabled();
            ImGui::Spacing();
            ImGui::Checkbox("Put my password in the database (a hash; everybody gets the same fallback password)", &publisherIncludePassword_);
            if (publisherIncludePassword_)
            {
                InputPassword("##publisherpassword", publisherPassword_, sizeof(publisherPassword_));
                ImGui::TextColored(kTextDim, draft.password.Valid() ? "Leave it empty to keep the password that is in the database already." : "6 characters or more.");
            }
            if (databasePath_[0] == '\0')
            {
                const std::filesystem::path suggested =
                    service_.Database().path.empty() ? ExecutableDirectory() / "faceid" / kDatabaseFileName : service_.Database().path;
                SetText(databasePath_, sizeof(databasePath_), suggested.u8string());
            }
            ImGui::Text("File to write:");
            ImGui::InputText("##databasepath", databasePath_, sizeof(databasePath_));
            ImGui::BeginDisabled(draft.faces.empty());
            if (ImGui::Button("Sign and save the database"))
            {
                std::string error;
                if (service_.PublisherSave(PathOf(databasePath_), publisherIncludePassword_, publisherPassword_, &error))
                {
                    say(true, "Signed and saved. This copy uses it now. Give the file to the others: next to the exe in faceid\\, in a package, or assets\\faceid\\ of the repository.");
                }
                else
                {
                    say(false, error);
                }
                WipeBuffer(publisherPassword_, sizeof(publisherPassword_));
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!service_.DraftDirty());
            if (ImGui::Button("Discard the changes"))
            {
                service_.DraftDiscard();
                say(true, "The draft is the signed database again.");
            }
            ImGui::EndDisabled();
        }
        if (!publisherMessage_.empty())
        {
            ImGui::TextColored(publisherMessageGood_ ? kGood : kBad, "%s", publisherMessage_.c_str());
        }
    }
}
