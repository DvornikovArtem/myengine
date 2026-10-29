// FaceIdGate.h
// The screens of Face ID in the editor (ImGui): the login that covers the editor until the face (or the password) is known, the recording of a profile (consent, name,
// eight faces) and the settings window (docs/editor/faceid.md). The gate is a thin layer over FaceIdService: it keeps no picture and no embedding.

#pragma once

#include <myengine/faceid/FaceIdService.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace myengine::faceid
{
    struct FaceIdUiContext
    {
        float width = 0.0f;       // the size of the window in the pixels of ImGui
        float height = 0.0f;
        double time = 0.0;
        // Makes a texture of the picture (mirrored when asked) and returns its id for ImGui::Image; the previous one of this window is released by the caller
        std::function<std::uint64_t(const FaceImage& image, bool mirror)> uploadPreview;
    };

    class FaceIdGate
    {
    public:
        explicit FaceIdGate(FaceIdService& service);

        // Decides what to show at the start of the editor: nothing (off, unavailable), the first-run offer or the login
        void Begin();
        // true while the gate covers the editor (login, the offer, the recording); the editor is not built then
        bool Draw(const FaceIdUiContext& context);
        // The settings window, drawn over the editor while it is open
        void DrawSettings(const FaceIdUiContext& context);
        void OpenSettings() { settingsOpen_ = true; settingsRefreshCameras_ = true; }
        bool Blocking() const { return screen_ != Screen::None; }

    private:
        enum class Screen
        {
            None,
            FirstRun,
            Consent,
            Details,
            Capturing,
            Saved,
            Login,
            Rejected    // Shared mode and the database did not pass the check: nobody gets in by face or password (the publisher can repair it)
        };

        void StartEnrolFlow(bool shared = false);
        void LeaveEnrol();
        void DrawLogin(const FaceIdUiContext& context);
        void DrawFirstRun(const FaceIdUiContext& context);
        void DrawConsent(const FaceIdUiContext& context);
        void DrawDetails(const FaceIdUiContext& context);
        void DrawCapturing(const FaceIdUiContext& context);
        void DrawSaved(const FaceIdUiContext& context);
        void DrawRejected(const FaceIdUiContext& context);
        void DrawSettingsLocal();
        void DrawSettingsShared();
        void DrawPublisher();
        void DrawPreview(const FaceIdUiContext& context, const FaceIdView& view, float width);
        void BeginPanel(const FaceIdUiContext& context, const char* title, float width);
        void EndPanel();

        FaceIdService& service_;
        Screen screen_ = Screen::None;
        bool fromSettings_ = false;           // the recording started from the settings window: it returns to the editor, not to the login

        // the picture
        std::uint64_t texture_ = 0;
        std::uint64_t textureSerial_ = 0;
        int frameWidth_ = 640;
        int frameHeight_ = 480;

        // the login by the password
        bool passwordMode_ = false;
        char passwordBuffer_[64] = {};
        std::string passwordError_;
        double grantedAt_ = 0.0;

        // the recording
        char nameBuffer_[40] = {};
        char newPasswordBuffer_[64] = {};
        char repeatPasswordBuffer_[64] = {};
        bool consentChecked_ = false;
        bool enrolShared_ = false;            // the face goes into the draft of the shared database (the publisher), not into the local profiles
        std::string detailsError_;
        std::string savedText_;
        bool savedOk_ = false;

        // the settings window
        bool settingsOpen_ = false;
        bool settingsRefreshCameras_ = true;
        std::vector<CameraInfo> cameras_;
        int pendingDelete_ = -1;
        bool pendingEraseAll_ = false;
        char settingsPassword_[64] = {};
        std::string settingsMessage_;

        // the publisher section of the settings window
        char databasePath_[520] = {};
        char exportPath_[520] = {};
        char exportPassword_[64] = {};
        char importPath_[520] = {};
        char importPassword_[64] = {};
        char publisherPassword_[64] = {};
        bool publisherIncludePassword_ = false;
        int pendingDraftDelete_ = -1;
        std::string publisherMessage_;
        bool publisherMessageGood_ = false;
    };
}
