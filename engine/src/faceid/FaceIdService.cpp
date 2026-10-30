#include <myengine/faceid/FaceIdService.h>

#include <myengine/faceid/FaceAlign.h>
#include <myengine/faceid/FacePhotos.h>

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
        // after photos the camera adds the turned faces first (a photo is mostly straight), then straight ones
        constexpr int kAfterPhotosPoses[kEnrolTarget - 1] = {-1, 1, 0, 0, 0, 0, 0};
        constexpr float kMinEnrolFaceWidth = 120.0f;
        constexpr float kMinSharpness = 30.0f;       // enrol: the variance of the Laplacian of the aligned face
        constexpr float kTurnedYaw = 0.20f;
        constexpr float kStraightYaw = 0.12f;

        std::filesystem::path DirectoryOf(const FaceIdService::Options& options)
        {
            return options.directory.empty() ? FaceProfileStore::DefaultFile().parent_path() : options.directory;
        }

        int EnrolPose(const int index, const int target)
        {
            return target == kEnrolTarget ? kEnrolPoses[std::clamp(index, 0, kEnrolTarget - 1)] : kAfterPhotosPoses[std::clamp(index, 0, kEnrolTarget - 2)];
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
        read("forceLocal", settings.forceLocal);
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
        root["forceLocal"] = forceLocal;
        std::ofstream stream(file, std::ios::trunc);
        stream << root.dump(2) << "\n";
        return static_cast<bool>(stream);
    }

    FaceIdService::FaceIdService() : FaceIdService(Options{}) {}

    FaceIdService::FaceIdService(Options options)
        : options_(std::move(options)),
          store_(DirectoryOf(options_) / "profiles.bin", options_.protector != nullptr ? options_.protector : MakeDpapiProtector()),
          signer_(options_.signingKeyFile.empty() ? DirectoryOf(options_) / "signing.key" : options_.signingKeyFile,
                  options_.protector != nullptr ? options_.protector : MakeDpapiProtector())
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

    std::filesystem::path FaceIdService::Directory() const { return DirectoryOf(options_); }

    bool FaceIdService::Initialize()
    {
        settings_ = FaceIdSettings::Load(DirectoryOf(options_) / "settings.json");
        decider_.SetThreshold(settings_.threshold);
        profilesError_.clear();
        signerError_.clear();
        signer_.Load(&signerError_);
        ReloadDatabase();
        if (source_ == FaceSource::Local)
        {
            store_.Load(&profilesError_);   // the profiles of this computer matter only when no shared database decides
        }
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

    std::vector<std::vector<std::uint8_t>> FaceIdService::TrustedKeys() const
    {
        std::vector<std::vector<std::uint8_t>> keys;
        std::vector<std::uint8_t> builtIn;
        if (options_.overrideTrustedKey)
        {
            if (!FromHex(options_.trustedKeyHex, builtIn) || builtIn.size() != kPublicKeySize)
            {
                builtIn.clear();
            }
        }
        else
        {
            builtIn = BuiltInPublicKey();
        }
        if (!builtIn.empty())
        {
            keys.push_back(builtIn);
        }
        else if (signer_.HasKey())
        {
            // no key is built into this engine yet: the machine of the publisher trusts its own key, so the work can go on before the engine is built again
            keys.push_back(signer_.PublicKey());
        }
        return keys;
    }

    // true when no key is built into the engine (the test's own key counts as built in)
    bool FaceIdService::BuiltInKeyIsEmpty() const
    {
        if (!options_.overrideTrustedKey)
        {
            return BuiltInPublicKey().empty();
        }
        std::vector<std::uint8_t> key;
        return !FromHex(options_.trustedKeyHex, key) || key.size() != kPublicKeySize;
    }

    bool FaceIdService::IsPublisher() const
    {
        if (!signer_.HasKey())
        {
            return false;
        }
        const std::vector<std::vector<std::uint8_t>> keys = TrustedKeys();
        return !keys.empty() && keys.front() == signer_.PublicKey();
    }

    std::string FaceIdService::PublisherKeyNote() const
    {
        if (!signer_.HasKey())
        {
            return {};
        }
        const std::vector<std::uint8_t> builtIn = options_.overrideTrustedKey ? std::vector<std::uint8_t>() : BuiltInPublicKey();
        if (options_.overrideTrustedKey)
        {
            std::vector<std::uint8_t> key;
            if (FromHex(options_.trustedKeyHex, key) && key == signer_.PublicKey())
            {
                return "This key is the one built into the engine.";
            }
            return options_.trustedKeyHex.empty() ? "No key is built into the engine yet." : "This key is NOT the one built into the engine.";
        }
        if (builtIn.empty())
        {
            return "No key is built into the engine yet: copy the public key and put it into FaceIdTrustedKey.h (tools\\faceid\\set_trusted_key.ps1), then build again. Until then only this computer accepts the database.";
        }
        if (builtIn == signer_.PublicKey())
        {
            return "This key is the one built into the engine.";
        }
        return "This key is NOT the one built into the engine: databases signed with it are rejected everywhere, also here.";
    }

    bool FaceIdService::PasswordAvailable() const
    {
        if (source_ == FaceSource::Shared)
        {
            return databaseInfo_.valid && databaseInfo_.hasPassword;
        }
        return store_.HasPassword();
    }

    void FaceIdService::ReloadDatabase()
    {
        databaseInfo_ = DatabaseInfo{};
        sharedProfiles_.clear();
        sharedPassword_ = PasswordHash{};
        const std::filesystem::path exeDirectory = options_.executableDirectory.empty() ? ExecutableDirectory() : options_.executableDirectory;
        const DatabaseLocation location = FindDatabase(options_.databaseFile, exeDirectory);
        draft_ = FaceDatabase{};
        draftDirty_ = false;
        if (location.path.empty())
        {
            source_ = FaceSource::Local;
            return;
        }
        source_ = FaceSource::Shared;
        databaseInfo_.path = location.path;
        databaseInfo_.named = location.named;
        databaseInfo_.found = location.exists;
        if (!location.exists)
        {
            databaseInfo_.error = "The face database that was named is not there: " + location.path.string();
        }
        else
        {
            const std::vector<std::vector<std::uint8_t>> keys = TrustedKeys();
            const DatabaseCheck check = LoadDatabase(location.path, keys);
            if (check.status == DatabaseStatus::Ok)
            {
                databaseInfo_.valid = true;
                databaseInfo_.faces = check.database.faces.size();
                databaseInfo_.created = check.database.created;
                databaseInfo_.hasPassword = check.database.password.Valid();
                databaseInfo_.signedByBuiltInKey = !keys.empty() && !BuiltInKeyIsEmpty();
                sharedProfiles_ = check.database.Profiles();
                sharedPassword_ = check.database.password;
                draft_ = check.database;
            }
            else
            {
                databaseInfo_.error = check.message;
            }
        }
        if (settings_.forceLocal && IsPublisher())
        {
            source_ = FaceSource::Local;   // the publisher tries the Local mode: the database stays where it is
        }
    }

    bool FaceIdService::DraftAddFace(const std::string& name, std::vector<FaceEmbedding> embeddings, std::string* error)
    {
        const auto fail = [&](const std::string& text)
        {
            if (error != nullptr)
            {
                *error = text;
            }
            return false;
        };
        if (draft_.faces.size() >= FaceDatabase::kMaxFaces)
        {
            return fail("the database holds " + std::to_string(FaceDatabase::kMaxFaces) + " faces at most");
        }
        if (name.empty() || name.size() > FaceProfileStore::kMaxNameBytes)
        {
            return fail("the name must be 1 to 32 bytes");
        }
        for (const SharedFace& face : draft_.faces)
        {
            if (face.name == name)
            {
                return fail("there is a face with this name");
            }
        }
        if (embeddings.size() < FaceProfileStore::kMinEmbeddings || embeddings.size() > FaceProfileStore::kMaxEmbeddings)
        {
            return fail("a face needs 5 to 10 pictures");
        }
        for (FaceEmbedding& embedding : embeddings)
        {
            Normalize(embedding);
        }
        draft_.faces.push_back({name, TodayText(), std::move(embeddings)});
        draftDirty_ = true;
        return true;
    }

    bool FaceIdService::DraftRemoveFace(const std::size_t index)
    {
        if (index >= draft_.faces.size())
        {
            return false;
        }
        draft_.faces.erase(draft_.faces.begin() + static_cast<std::ptrdiff_t>(index));
        draftDirty_ = true;
        return true;
    }

    void FaceIdService::DraftDiscard()
    {
        draft_ = FaceDatabase{};
        draftDirty_ = false;
        if (databaseInfo_.valid)
        {
            const std::vector<std::vector<std::uint8_t>> keys = TrustedKeys();
            const DatabaseCheck check = LoadDatabase(databaseInfo_.path, keys);
            if (check.status == DatabaseStatus::Ok)
            {
                draft_ = check.database;
            }
        }
    }

    bool FaceIdService::PublisherSave(const std::filesystem::path& file, const bool includePassword, const std::string& password, std::string* error)
    {
        const auto fail = [&](const std::string& text)
        {
            if (error != nullptr)
            {
                *error = text;
            }
            return false;
        };
        if (!IsPublisher())
        {
            return fail("only the computer with the publisher key signs the database");
        }
        if (file.empty())
        {
            return fail("type the file to write");
        }
        FaceDatabase database = draft_;
        database.created = TodayText();
        if (!includePassword)
        {
            database.password = PasswordHash{};
        }
        else if (!password.empty())
        {
            if (password.size() < 6)
            {
                return fail("the password needs 6 characters or more");
            }
            if (!HashPassword(password, database.password))
            {
                return fail("the password could not be hashed");
            }
        }
        else if (!database.password.Valid())
        {
            return fail("type the password to put in the database, or switch the option off");
        }
        std::vector<std::uint8_t> bytes;
        if (!SignDatabase(database, signer_, bytes, error))
        {
            return false;
        }
        if (!SaveDatabaseFile(file, bytes, error))
        {
            return false;
        }
        // the service uses the file that was just written: named, when the usual places do not lead to it
        const DatabaseLocation usual = FindDatabase({}, options_.executableDirectory.empty() ? ExecutableDirectory() : options_.executableDirectory);
        std::error_code ec;
        const bool sameFile = !usual.path.empty() && usual.exists && std::filesystem::equivalent(usual.path, file, ec);
        options_.databaseFile = sameFile ? std::filesystem::path() : file;
        ReloadDatabase();
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
            view_.target = std::max(1, CameraFacesAfter(photos_.size()));
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
        embeddings.clear();
        for (const PhotoFace& photo : photos_)
        {
            embeddings.push_back(photo.embedding);
        }
        photos_.clear();
        embeddings.insert(embeddings.end(), enrolled_.begin(), enrolled_.end());
        enrolled_.clear();
        view_.enrolReady = false;
        view_.captured = 0;
        return true;
    }

    int FaceIdService::CameraFacesAfter(const std::size_t photos)
    {
        if (photos == 0)
        {
            return kEnrolTarget;
        }
        if (photos >= FaceProfileStore::kMaxEmbeddings)
        {
            return 0;
        }
        return std::clamp(kEnrolTarget - static_cast<int>(photos), 1, static_cast<int>(FaceProfileStore::kMaxEmbeddings - photos));
    }

    std::vector<PhotoReport> FaceIdService::AddPhotos(const std::vector<std::filesystem::path>& files)
    {
        std::vector<PhotoReport> reports(files.size());
        bool idle = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            idle = mode_ == FaceIdMode::Idle;
        }

        // 1. each photo on its own: a picture with one good face
        struct Candidate
        {
            std::size_t report = 0;
            PhotoResult result;
        };
        std::vector<Candidate> candidates;
        std::size_t room = FaceProfileStore::kMaxEmbeddings > photos_.size() ? FaceProfileStore::kMaxEmbeddings - photos_.size() : 0;
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            PhotoReport& report = reports[i];
            report.file = files[i].filename().u8string();
            if (!available_ || !idle)
            {
                report.message = "Face ID is busy or not available";
                continue;
            }
            if (room == 0)
            {
                report.message = "not read: " + std::to_string(FaceProfileStore::kMaxEmbeddings) + " photos are the most for one face";
                continue;
            }
            FaceImage picture;
            if (!LoadPhoto(files[i], picture, report.message))
            {
                continue;
            }
            PhotoResult result = ExaminePhoto(models_, picture);
            if (!result.accepted)
            {
                report.message = result.reason;
                continue;
            }
            // the same picture twice
            const std::string* twin = nullptr;
            for (const PhotoFace& other : photos_)
            {
                if (Cosine(result.embedding, other.embedding) > kPhotoDuplicate)
                {
                    twin = &other.file;
                }
            }
            for (const Candidate& other : candidates)
            {
                if (twin == nullptr && Cosine(result.embedding, other.result.embedding) > kPhotoDuplicate)
                {
                    twin = &reports[other.report].file;
                }
            }
            if (twin != nullptr)
            {
                report.message = "the same picture as " + *twin;
                continue;
            }
            candidates.push_back({i, std::move(result)});
            --room;
        }

        // 2. the photos of one face are of one person: a photo joins when it looks like another one (a chain of "looks like" counts); the people that were chosen before form the
        // first group, and without them the biggest group of this batch is the person (the first of equals). A stranger among the photos is a slip of the file, not a deception.
        const std::size_t before = photos_.size();
        std::vector<int> group(before + candidates.size());
        for (std::size_t i = 0; i < group.size(); ++i)
        {
            group[i] = static_cast<int>(i);
        }
        const auto embeddingAt = [&](const std::size_t index) -> const FaceEmbedding&
        { return index < before ? photos_[index].embedding : candidates[index - before].result.embedding; };
        const auto find = [&](int node)
        {
            while (group[static_cast<std::size_t>(node)] != node)
            {
                node = group[static_cast<std::size_t>(node)] = group[static_cast<std::size_t>(group[static_cast<std::size_t>(node)])];
            }
            return node;
        };
        for (std::size_t i = 1; i < before; ++i)
        {
            group[static_cast<std::size_t>(find(static_cast<int>(i)))] = find(0);
        }
        for (std::size_t i = 0; i < group.size(); ++i)
        {
            for (std::size_t j = i + 1; j < group.size(); ++j)
            {
                if (Cosine(embeddingAt(i), embeddingAt(j)) >= kPhotoSamePerson)
                {
                    group[static_cast<std::size_t>(find(static_cast<int>(j)))] = find(static_cast<int>(i));
                }
            }
        }
        int chosen = -1;
        if (before > 0)
        {
            chosen = find(0);
        }
        else
        {
            std::size_t biggest = 0;
            for (std::size_t i = 0; i < group.size(); ++i)
            {
                const int root = find(static_cast<int>(i));
                std::size_t count = 0;
                for (std::size_t j = 0; j < group.size(); ++j)
                {
                    if (find(static_cast<int>(j)) == root)
                    {
                        ++count;
                    }
                }
                if (count > biggest)
                {
                    biggest = count;
                    chosen = root;
                }
            }
        }
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            Candidate& candidate = candidates[i];
            PhotoReport& report = reports[candidate.report];
            if (find(static_cast<int>(before + i)) != chosen)
            {
                report.message = "this does not look like the person in the other photos";
                continue;
            }
            photos_.push_back({report.file, candidate.result.embedding, candidate.result.mirrored});
            report.accepted = true;
            report.message = "face found (" + std::to_string(static_cast<int>(std::lround(candidate.result.faceWidth))) + " px wide)";
        }
        return reports;
    }

    bool FaceIdService::TakePhotoFaces(std::vector<FaceEmbedding>& embeddings, std::string* error)
    {
        if (photos_.empty())
        {
            if (error != nullptr)
            {
                *error = "no photo with a face was added";
            }
            return false;
        }
        embeddings.clear();
        for (const PhotoFace& photo : photos_)
        {
            embeddings.push_back(photo.embedding);
        }
        for (std::size_t i = 0; embeddings.size() < FaceProfileStore::kMinEmbeddings && i < photos_.size(); ++i)
        {
            embeddings.push_back(photos_[i].mirrored);
        }
        photos_.clear();
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
        const bool right = source_ == FaceSource::Shared ? (databaseInfo_.valid && databaseInfo_.hasPassword && VerifyPassword(password, sharedPassword_))
                                                         : (store_.HasPassword() && store_.CheckPassword(password));
        if (right)
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
                const MatchResult match = BestMatch(ActiveProfiles(), embedding);
                view_.lastScore = match.score;
                if (match.profile == acceptedProfile_ && match.score >= settings_.threshold * 0.9f)
                {
                    if (++afterChallengeFrames_ >= 3)
                    {
                        limiter_.Success();
                        view_.grantedName = ActiveProfiles()[static_cast<std::size_t>(acceptedProfile_)].name;
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
        const MatchResult match = BestMatch(ActiveProfiles(), embedding);
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
                view_.grantedName = ActiveProfiles()[static_cast<std::size_t>(acceptedProfile_)].name;
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
            pose = EnrolPose(view_.captured, view_.target);
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
        if (view_.captured >= view_.target)
        {
            view_.enrolReady = true;
            view_.text = "done";
            mode_ = FaceIdMode::Idle;
        }
    }
}
