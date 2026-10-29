// FaceIdService.h
// Face ID without the screens: owns the camera, the models, the profiles and the settings, and runs the recognition (login) or the recording of a profile (enrol) on a thread
// of its own. A screen asks View() for what to draw and calls the commands. No frame is written anywhere: the pictures live in memory until the camera stops.
// (docs/editor/faceid.md)

#pragma once

#include <myengine/faceid/FaceCamera.h>
#include <myengine/faceid/FaceMatcher.h>
#include <myengine/faceid/FaceModels.h>
#include <myengine/faceid/FaceProfileStore.h>
#include <myengine/faceid/FaceTypes.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace myengine::faceid
{
    struct FaceIdSettings
    {
        bool enabled = true;                 // the login screen of the editor
        bool games = false;                  // the games (Rocket Arena, FlyDoom) ask for Face ID too
        bool liveness = true;                // "turn your head"
        std::string camera;                  // the name of the camera, empty: the first one
        float threshold = kDefaultThreshold; // the cosine similarity that counts as the same person

        static std::filesystem::path DefaultFile();
        static FaceIdSettings Load(const std::filesystem::path& file);
        bool Save(const std::filesystem::path& file) const;
    };

    enum class FaceIdMode
    {
        Idle,
        Login,
        Enroll
    };

    enum class LoginPhase
    {
        Starting,         // the camera is opening
        NoCamera,         // it did not open (the text is in cameraStatus)
        LookingForFace,
        Recognising,
        Challenge,        // "turn your head"
        Granted,
        Denied,           // a stranger or a failed check; after a moment the next attempt begins
        Locked            // three failures: wait
    };

    // What a screen draws, a copy taken under the lock
    struct FaceIdView
    {
        FaceIdMode mode = FaceIdMode::Idle;
        std::uint64_t frameSerial = 0;
        bool hasFrame = false;
        FaceImage frame;                 // the newest picture (only when `wantFrame` was asked and it is newer)
        bool hasFace = false;
        FaceDetection face;              // in the pixels of the frame
        std::string cameraStatus;
        bool cameraFailed = false;

        LoginPhase phase = LoginPhase::Starting;
        std::string text;                // the line under the picture
        std::string grantedName;
        double lockRemaining = 0.0;
        float challengeProgress = 0.0f;
        LivenessChallenge::Direction challengeDirection = LivenessChallenge::Direction::ImageLeft;
        int streak = 0;
        int streakNeeded = 5;

        int captured = 0;                // enrol
        int target = 8;
        bool enrolReady = false;         // all faces are taken: TakeEnrolled gives the profile
        float lastScore = 0.0f;          // the similarity of the last frame (shown in the debug line only)
    };

    class FaceIdService
    {
    public:
        struct Options
        {
            std::filesystem::path directory;          // the folder of the profiles and the settings; empty: %LOCALAPPDATA%\myengine\faceid
            std::filesystem::path modelsDirectory;    // empty: FaceModels::DefaultDirectory()
            std::shared_ptr<IDataProtector> protector; // empty: DPAPI
        };

        FaceIdService();
        explicit FaceIdService(Options options);
        ~FaceIdService();
        FaceIdService(const FaceIdService&) = delete;
        FaceIdService& operator=(const FaceIdService&) = delete;

        // Loads the settings and the profiles and the models. false: Face ID cannot work (no ONNX Runtime, no models); `UnavailableReason` says why. A damaged file of the
        // profiles is a message in `ProfilesError`, not a failure: the screen offers to start again.
        bool Initialize();
        bool Available() const { return available_; }
        const std::string& UnavailableReason() const { return unavailableReason_; }
        const std::string& ProfilesError() const { return profilesError_; }

        FaceIdSettings& Settings() { return settings_; }
        bool SaveSettings() const;
        FaceProfileStore& Store() { return store_; }
        float Threshold() const { return settings_.threshold; }

        // ---- the commands (from the thread of the UI)
        void BeginLogin();                            // the camera on, the recognition running
        void BeginEnroll();                           // the camera on, the recording of one profile (8 faces)
        void Stop();                                  // the camera off, the thread idle
        void RestartAttempt();                        // after Denied: again (the limiter decides whether it is Locked)

        // ---- the state
        FaceIdView View(bool wantFrame, std::uint64_t knownSerial);
        // The embeddings of an enrolment that has finished (the profile is not yet in the store: the screen asks the name and the password, then Store().Add)
        bool TakeEnrolled(std::vector<FaceEmbedding>& embeddings);

        // The login by the password (the camera did not work, or nobody wants it)
        bool LoginByPassword(const std::string& password);

        static double NowSeconds();

    private:
        void Loop();
        void ProcessLogin(const FaceImage& frame, double now);
        void ProcessEnroll(const FaceImage& frame, double now);
        void SetLoginPhase(LoginPhase phase, const std::string& text);

        Options options_;
        FaceIdSettings settings_;
        FaceProfileStore store_;
        FaceModels models_;
        FaceCamera camera_;
        bool available_ = false;
        std::string unavailableReason_;
        std::string profilesError_;

        std::thread thread_;
        std::atomic<bool> quit_{false};
        std::mutex mutex_;
        std::condition_variable wake_;
        FaceIdMode mode_ = FaceIdMode::Idle;        // guarded by mutex_
        bool attemptReset_ = false;

        // the state of the worker (guarded by mutex_ where the screen reads it)
        FaceIdView view_;
        std::vector<FaceEmbedding> enrolled_;
        RecognitionDecider decider_;
        AttemptLimiter limiter_;
        LivenessChallenge challenge_;
        int acceptedProfile_ = -1;
        int afterChallengeFrames_ = 0;
    int mismatchFrames_ = 0;
        double phaseSince_ = 0.0;
        double lastCapture_ = 0.0;
        int stableFrames_ = 0;
        int noFaceFrames_ = 0;
        std::mt19937 random_{std::random_device{}()};
        std::uint64_t knownSerial_ = 0;
    };
}
