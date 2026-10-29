#include <myengine/faceid/FaceIdService.h>

#include <myengine/faceid/FaceAlign.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace myengine::faceid
{
    namespace
    {
        constexpr int kEnrolTarget = 8;
        // the poses of the eight faces: 0 straight, -1 the nose to the left of the camera picture (the person turns to their right), +1 the other way
        constexpr int kEnrolPoses[kEnrolTarget] = {0, 0, -1, -1, 1, 1, 0, 0};
        constexpr float kMinFaceWidth = 90.0f;       // login: a face narrower than this (in a 640 x 480 picture) is "come closer"
        constexpr float kMinEnrolFaceWidth = 120.0f;
        constexpr float kMinSharpness = 30.0f;       // enrol: the variance of the Laplacian of the aligned face
        constexpr float kTurnedYaw = 0.20f;
        constexpr float kStraightYaw = 0.12f;

        std::filesystem::path DirectoryOf(const FaceIdService::Options& options)
        {
            return options.directory.empty() ? FaceProfileStore::DefaultFile().parent_path() : options.directory;
        }

        const FaceDetection* Largest(const std::vector<FaceDetection>& faces)
        {
            const FaceDetection* best = nullptr;
            for (const FaceDetection& face : faces)
            {
                if (best == nullptr || face.w * face.h > best->w * best->h)
                {
                    best = &face;
                }
            }
            return best;
        }
    }

    std::filesystem::path FaceIdSettings::DefaultFile() { return FaceProfileStore::DefaultFile().parent_path() / "settings.json"; }

    FaceIdSettings FaceIdSettings::Load(const std::filesystem::path& file)
    {
        FaceIdSettings settings;
        std::ifstream stream(file);
        if (!stream)
        {
            return settings;
        }
        const nlohmann::json root = nlohmann::json::parse(stream, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            return settings;
        }
        const auto read = [&](const char* key, auto& value)
        {
            const auto it = root.find(key);
            if (it != root.end())
            {
                try
                {
                    value = it->get<std::decay_t<decltype(value)>>();
                }
                catch (const nlohmann::json::exception&)
                {
                    // a value of the wrong kind keeps the default
                }
            }
        };
        read("enabled", settings.enabled);
        read("games", settings.games);
        read("liveness", settings.liveness);
        read("camera", settings.camera);
        read("threshold", settings.threshold);
        settings.threshold = std::clamp(settings.threshold, 0.2f, 0.9f);
        return settings;
    }

    bool FaceIdSettings::Save(const std::filesystem::path& file) const
    {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        nlohmann::json root;
        root["enabled"] = enabled;
        root["games"] = games;
        root["liveness"] = liveness;
        root["camera"] = camera;
        root["threshold"] = threshold;
        std::ofstream stream(file, std::ios::trunc);
        stream << root.dump(2) << "\n";
        return static_cast<bool>(stream);
    }

    FaceIdService::FaceIdService() : FaceIdService(Options{}) {}

    FaceIdService::FaceIdService(Options options)
        : options_(std::move(options)),
          store_(DirectoryOf(options_) / "profiles.bin", options_.protector != nullptr ? options_.protector : MakeDpapiProtector())
    {
        view_.target = kEnrolTarget;
        thread_ = std::thread([this]() { Loop(); });
    }

    FaceIdService::~FaceIdService()
    {
        quit_.store(true);
        wake_.notify_all();
        if (thread_.joinable())
        {
            thread_.join();
        }
        camera_.Stop();
    }

    double FaceIdService::NowSeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool FaceIdService::Initialize()
    {
        settings_ = FaceIdSettings::Load(DirectoryOf(options_) / "settings.json");
        decider_.SetThreshold(settings_.threshold);
        profilesError_.clear();
        store_.Load(&profilesError_);
        available_ = false;
        if (!FaceModels::Available())
        {
            std::string error;
            models_.Load({}, error);
            unavailableReason_ = error;
            return false;
        }
        const std::filesystem::path models = options_.modelsDirectory.empty() ? FaceModels::DefaultDirectory() : options_.modelsDirectory;
        std::string error;
        if (!models_.Load(models, error))
        {
            unavailableReason_ = error;
            return false;
        }
        unavailableReason_.clear();
        available_ = true;
        return true;
    }

    bool FaceIdService::SaveSettings() const { return settings_.Save(DirectoryOf(options_) / "settings.json"); }

    void FaceIdService::SetLoginPhase(const LoginPhase phase, const std::string& text)
    {
        view_.phase = phase;
        view_.text = text;
        phaseSince_ = NowSeconds();
    }

    void FaceIdService::BeginLogin()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode_ = FaceIdMode::Login;
            view_ = FaceIdView{};
            view_.mode = FaceIdMode::Login;
            view_.target = kEnrolTarget;
            decider_ = RecognitionDecider(5, settings_.threshold);
            view_.streakNeeded = decider_.FramesNeeded();
            challenge_.Reset();
            acceptedProfile_ = -1;
            noFaceFrames_ = 0;
            knownSerial_ = 0;
            SetLoginPhase(LoginPhase::Starting, "opening the camera ...");
        }
        camera_.Start(settings_.camera);
        wake_.notify_all();
    }

    void FaceIdService::BeginEnroll()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode_ = FaceIdMode::Enroll;
            view_ = FaceIdView{};
            view_.mode = FaceIdMode::Enroll;
            view_.target = kEnrolTarget;
            enrolled_.clear();
            stableFrames_ = 0;
            lastCapture_ = 0.0;
            knownSerial_ = 0;
            view_.text = "opening the camera ...";
        }
        camera_.Start(settings_.camera);
        wake_.notify_all();
    }

    void FaceIdService::Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode_ = FaceIdMode::Idle;
            view_.mode = FaceIdMode::Idle;
            view_.frame = {};
            view_.hasFrame = false;
        }
        camera_.Stop();
    }

    void FaceIdService::RestartAttempt()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        attemptReset_ = true;
    }

    FaceIdView FaceIdService::View(const bool wantFrame, const std::uint64_t knownSerial)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        FaceIdView copy = view_;
        copy.frame = {};
        copy.hasFrame = false;
        if (wantFrame && view_.hasFrame && view_.frameSerial != knownSerial)
        {
            copy.frame = view_.frame;
            copy.hasFrame = true;
        }
        copy.cameraStatus = camera_.Status();
        copy.cameraFailed = camera_.GetState() == FaceCamera::State::Failed;
        if (copy.mode == FaceIdMode::Login && copy.cameraFailed && copy.phase != LoginPhase::Granted)
        {
            copy.phase = LoginPhase::NoCamera;
        }
        if (limiter_.Locked(NowSeconds()))
        {
            copy.phase = LoginPhase::Locked;
            copy.lockRemaining = limiter_.RemainingSeconds(NowSeconds());
        }
        return copy;
    }

    bool FaceIdService::TakeEnrolled(std::vector<FaceEmbedding>& embeddings)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!view_.enrolReady)
        {
            return false;
        }
        embeddings = enrolled_;
        enrolled_.clear();
        view_.enrolReady = false;
        view_.captured = 0;
        return true;
    }

    bool FaceIdService::LoginByPassword(const std::string& password)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = NowSeconds();
        if (limiter_.Locked(now))
        {
            return false;
        }
        if (store_.HasPassword() && store_.CheckPassword(password))
        {
            limiter_.Success();
            view_.phase = LoginPhase::Granted;
            view_.grantedName = "password";
            return true;
        }
        limiter_.Failure(now);
        return false;
    }

    void FaceIdService::Loop()
    {
        while (!quit_.load())
        {
            FaceIdMode mode = FaceIdMode::Idle;
            bool reset = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(15));
                mode = mode_;
                reset = attemptReset_;
                attemptReset_ = false;
                if (reset && mode == FaceIdMode::Login)
                {
                    decider_.Reset();
                    challenge_.Reset();
                    acceptedProfile_ = -1;
                    noFaceFrames_ = 0;
                    if (!limiter_.Locked(NowSeconds()))
                    {
                        SetLoginPhase(LoginPhase::LookingForFace, "look at the camera");
                    }
                }
            }
            if (mode == FaceIdMode::Idle || !available_)
            {
                continue;
            }
            FaceImage frame;
            if (!camera_.Latest(frame, knownSerial_))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                continue;
            }
            const double now = NowSeconds();
            if (mode == FaceIdMode::Login)
            {
                ProcessLogin(frame, now);
            }
            else
            {
                ProcessEnroll(frame, now);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            view_.frame = std::move(frame);
            view_.hasFrame = true;
            view_.frameSerial = knownSerial_;
        }
    }

    void FaceIdService::ProcessLogin(const FaceImage& frame, const double now)
    {
        LoginPhase phase;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            phase = view_.phase;
            if (limiter_.Locked(now))
            {
                view_.phase = LoginPhase::Locked;
                view_.hasFace = false;
                return;
            }
            if (phase == LoginPhase::Locked)
            {
                decider_.Reset();
                SetLoginPhase(LoginPhase::LookingForFace, "look at the camera");
                phase = LoginPhase::LookingForFace;
            }
            if (phase == LoginPhase::Denied)
            {
                if (now - phaseSince_ >= 2.5)
                {
                    decider_.Reset();
                    challenge_.Reset();
                    acceptedProfile_ = -1;
                    SetLoginPhase(LoginPhase::LookingForFace, "look at the camera");
                }
                return;
            }
            if (phase == LoginPhase::Granted || phase == LoginPhase::Starting)
            {
                if (phase == LoginPhase::Starting)
                {
                    SetLoginPhase(LoginPhase::LookingForFace, "look at the camera");
                    phase = LoginPhase::LookingForFace;
                }
                else
                {
                    return;
                }
            }
        }

        const std::vector<FaceDetection> faces = models_.Detect(frame);
        const FaceDetection* best = Largest(faces);
        FaceEmbedding embedding{};
        bool embedded = false;
        if (best != nullptr && best->w >= kMinFaceWidth && phase != LoginPhase::Challenge)
        {
            embedded = models_.Embed(frame, *best, embedding);
        }
        else if (best != nullptr && best->w >= kMinFaceWidth && phase == LoginPhase::Challenge && challenge_.GetState() == LivenessChallenge::State::Passed)
        {
            embedded = models_.Embed(frame, *best, embedding);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto fail = [&](const std::string& text)
        {
            limiter_.Failure(now);
            decider_.Reset();
            challenge_.Reset();
            acceptedProfile_ = -1;
            if (limiter_.Locked(now))
            {
                SetLoginPhase(LoginPhase::Locked, "too many attempts");
            }
            else
            {
                SetLoginPhase(LoginPhase::Denied, text);
            }
        };
        if (best == nullptr)
        {
            view_.hasFace = false;
            if (++noFaceFrames_ >= 10 && view_.phase != LoginPhase::LookingForFace)
            {
                decider_.Reset();
                challenge_.Reset();
                acceptedProfile_ = -1;
                SetLoginPhase(LoginPhase::LookingForFace, "look at the camera");
            }
            return;
        }
        noFaceFrames_ = 0;
        view_.hasFace = true;
        view_.face = *best;
        if (best->w < kMinFaceWidth)
        {
            view_.text = "come closer";
            return;
        }

        if (view_.phase == LoginPhase::Challenge)
        {
            if (challenge_.GetState() == LivenessChallenge::State::Waiting)
            {
                challenge_.Feed(best->Yaw(), now);
                view_.challengeProgress = challenge_.Progress();
                if (challenge_.GetState() == LivenessChallenge::State::Failed)
                {
                    fail("the check of the live face failed");
                }
                return;
            }
            if (challenge_.GetState() == LivenessChallenge::State::Passed)
            {
                // after the turn the same person must still be there: three more frames of the same profile
                if (!embedded)
                {
                    return;
                }
                const MatchResult match = BestMatch(store_.Profiles(), embedding);
                view_.lastScore = match.score;
                if (match.profile == acceptedProfile_ && match.score >= settings_.threshold * 0.9f)
                {
                    if (++afterChallengeFrames_ >= 3)
                    {
                        limiter_.Success();
                        view_.grantedName = store_.Profiles()[static_cast<std::size_t>(acceptedProfile_)].name;
                        SetLoginPhase(LoginPhase::Granted, "hello, " + view_.grantedName);
                        mode_ = FaceIdMode::Idle;
                    }
                }
                else if (++mismatchFrames_ > 8)
                {
                    fail("the face changed during the check");
                }
            }
            return;
        }

        if (!embedded)
        {
            return;
        }
        const MatchResult match = BestMatch(store_.Profiles(), embedding);
        view_.lastScore = match.score;
        view_.phase = LoginPhase::Recognising;
        view_.text = "recognising ...";
        const RecognitionDecider::State state = decider_.Feed(match);
        view_.streak = decider_.Streak();
        if (state == RecognitionDecider::State::Accepted)
        {
            acceptedProfile_ = decider_.AcceptedProfile();
            if (settings_.liveness)
            {
                const auto direction = (random_() & 1u) != 0 ? LivenessChallenge::Direction::ImageLeft : LivenessChallenge::Direction::ImageRight;
                challenge_.Start(direction, now);
                afterChallengeFrames_ = 0;
                mismatchFrames_ = 0;
                noFaceFrames_ = 0;
                view_.challengeDirection = direction;
                view_.challengeProgress = 0.0f;
                SetLoginPhase(LoginPhase::Challenge, "turn your head");
            }
            else
            {
                limiter_.Success();
                view_.grantedName = store_.Profiles()[static_cast<std::size_t>(acceptedProfile_)].name;
                SetLoginPhase(LoginPhase::Granted, "hello, " + view_.grantedName);
                mode_ = FaceIdMode::Idle;
            }
        }
        else if (state == RecognitionDecider::State::Rejected)
        {
            fail("access denied");
        }
    }

    void FaceIdService::ProcessEnroll(const FaceImage& frame, const double now)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (view_.enrolReady)
            {
                return;
            }
        }
        const std::vector<FaceDetection> faces = models_.Detect(frame);
        const FaceDetection* best = Largest(faces);
        int pose = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pose = kEnrolPoses[std::clamp(view_.captured, 0, kEnrolTarget - 1)];
        }
        const auto setPrompt = [this](const std::string& text, const FaceDetection* face)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.text = text;
            view_.hasFace = face != nullptr;
            if (face != nullptr)
            {
                view_.face = *face;
            }
            stableFrames_ = 0;
        };
        if (best == nullptr)
        {
            setPrompt("look at the camera", nullptr);
            return;
        }
        for (const FaceDetection& other : faces)
        {
            if (&other != best && other.w * other.h > 0.35f * best->w * best->h)
            {
                setPrompt("only one person must be in the picture", best);
                return;
            }
        }
        if (best->w < kMinEnrolFaceWidth)
        {
            setPrompt("come closer", best);
            return;
        }
        const float yaw = best->Yaw();
        const bool poseOk = pose == 0 ? std::fabs(yaw) < kStraightYaw : (pose < 0 ? yaw < -kTurnedYaw : yaw > kTurnedYaw);
        if (!poseOk)
        {
            setPrompt(pose == 0 ? "look straight at the camera" : (pose < 0 ? "turn your head slowly to your right" : "turn your head slowly to your left"), best);
            return;
        }
        FaceImage aligned;
        if (!AlignFace(frame, best->landmarks, aligned) || Sharpness(aligned) < kMinSharpness)
        {
            setPrompt("hold still, the picture is blurred", best);
            return;
        }
        bool capture = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            view_.hasFace = true;
            view_.face = *best;
            view_.text = "hold ...";
            ++stableFrames_;
            capture = stableFrames_ >= 3 && now - lastCapture_ >= 0.6;
        }
        if (!capture)
        {
            return;
        }
        FaceEmbedding embedding{};
        if (!models_.Embed(frame, *best, embedding))
        {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        enrolled_.push_back(embedding);
        view_.captured = static_cast<int>(enrolled_.size());
        lastCapture_ = now;
        stableFrames_ = 0;
        if (view_.captured >= kEnrolTarget)
        {
            view_.enrolReady = true;
            view_.text = "done";
            mode_ = FaceIdMode::Idle;
        }
    }
}
