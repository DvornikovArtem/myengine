// FaceIdService.h
// Face ID without the screens: owns the camera, the models, the profiles and the settings, and runs the recognition (login) or the recording of a profile (enrol) on a thread
// of its own. A screen asks View() for what to draw and calls the commands. No frame is written anywhere: the pictures live in memory until the camera stops.
// (docs/editor/faceid.md)

#pragma once

#include <myengine/faceid/FaceCamera.h>
#include <myengine/faceid/FaceDatabase.h>
#include <myengine/faceid/FaceMatcher.h>
#include <myengine/faceid/FaceModels.h>
#include <myengine/faceid/FaceProfileStore.h>
#include <myengine/faceid/FaceSigner.h>
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
        bool forceLocal = false;             // the publisher's machine only: work with the local profiles although a shared database is there (to try the Local mode)

        static std::filesystem::path DefaultFile();
        static FaceIdSettings Load(const std::filesystem::path& file);
        bool Save(const std::filesystem::path& file) const;
    };

    // Whose faces let a person in: the shared database signed by the publisher (it is there, so it decides), or the profiles of this computer (there is none)
    enum class FaceSource
    {
        Local,
        Shared
    };

    // What the service found about the shared database (for the screens)
    struct DatabaseInfo
    {
        std::filesystem::path path;      // where it is, or where it was named to be
        bool named = false;              // --faceid-db / MYENGINE_FACEID_DB
        bool found = false;              // the file is there
        bool valid = false;              // and the signature fits: its faces are in use
        std::string error;               // the words for the screen when it is found (or named) and not valid
        std::size_t faces = 0;
        std::string created;
        bool hasPassword = false;
        bool signedByBuiltInKey = false; // false: the key of this machine (the publisher before the key is built into the engine)
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
            std::filesystem::path databaseFile;        // named by --faceid-db; empty: looked for (docs/editor/faceid.md)
            std::filesystem::path executableDirectory; // empty: the folder of the exe
            std::filesystem::path signingKeyFile;      // empty: <directory>/signing.key
            bool overrideTrustedKey = false;           // the tests: the key that databases are checked with instead of the built-in one
            std::string trustedKeyHex;
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

        // ---- the shared database (docs/editor/faceid.md, "The shared database")
        FaceSource Source() const { return source_; }
        const DatabaseInfo& Database() const { return databaseInfo_; }
        // Shared mode and the database did not pass: nobody gets in by face or by password until it is repaired (the reason is in Database().error)
        bool SharedBlocked() const { return source_ == FaceSource::Shared && !databaseInfo_.valid; }
        // The faces that let a person in now: the database or the local profiles
        const std::vector<FaceProfile>& ActiveProfiles() const { return source_ == FaceSource::Shared ? sharedProfiles_ : store_.Profiles(); }
        // Is there a password to type (Shared: the one in the database, if the publisher put it there; Local: the first profile's)
        bool PasswordAvailable() const;
        // Looks for the database again and checks it (after the publisher saved one, or a change of the mode)
        void ReloadDatabase();

        // ---- the publisher: the machine that has the private key (and, when a key is built into the engine, the same one)
        FaceSigner& Signer() { return signer_; }
        const std::string& SignerError() const { return signerError_; }
        bool IsPublisher() const;
        // The public key of this machine against the one built into the engine, in words ("" without a key)
        std::string PublisherKeyNote() const;
        // The faces being prepared for the next signing: the database that is in use, then what the publisher adds and removes
        const FaceDatabase& Draft() const { return draft_; }
        bool DraftDirty() const { return draftDirty_; }
        bool DraftAddFace(const std::string& name, std::vector<FaceEmbedding> embeddings, std::string* error = nullptr);
        bool DraftRemoveFace(std::size_t index);
        void DraftDiscard();
        // Signs the draft with the key of this machine and writes it. `includePassword`: the password of the publisher goes in as a hash (a new one when `password` is not
        // empty, else the one that is in the draft already). Afterwards the service uses that file at once.
        bool PublisherSave(const std::filesystem::path& file, bool includePassword, const std::string& password, std::string* error = nullptr);
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
        std::filesystem::path Directory() const;
        std::vector<std::vector<std::uint8_t>> TrustedKeys() const;
        bool BuiltInKeyIsEmpty() const;

        Options options_;
        FaceIdSettings settings_;
        FaceProfileStore store_;
        FaceSigner signer_;
        std::string signerError_;
        FaceSource source_ = FaceSource::Local;
        DatabaseInfo databaseInfo_;
        std::vector<FaceProfile> sharedProfiles_;
        PasswordHash sharedPassword_;
        FaceDatabase draft_;
        bool draftDirty_ = false;
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
