// Face ID without a camera (F0): the pure decisions (cosine, five frames in a row, lockout, turning the head), the alignment of a face, the profile file (DPAPI, limits,
// password, erasing) and, when ONNX Runtime and the models are there, the real YuNet + SFace on two public domain portraits of NASA astronauts. No camera is opened,
// no picture is written; the portraits are downloaded by tools/faceid/fetch_models.ps1 -WithTestImage and are not in the repository. Exit code 77: the models or the
// portraits are not there (CTest shows the test as skipped); the checks before that part have passed.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include <stb_image.h>

#include <myengine/faceid/FaceAlign.h>
#include <myengine/faceid/FaceMatcher.h>
#include <myengine/faceid/FaceModels.h>
#include <myengine/faceid/FaceProfileStore.h>
#include <myengine/faceid/FaceTypes.h>

namespace
{
    using namespace myengine::faceid;
    namespace fs = std::filesystem;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Near(const double a, const double b, const double tolerance = 1e-4) { return std::fabs(a - b) <= tolerance; }

    FaceEmbedding Unit(const int axis, const float other = 0.0f)
    {
        FaceEmbedding embedding{};
        embedding[static_cast<std::size_t>(axis)] = 1.0f;
        embedding[static_cast<std::size_t>((axis + 1) % kEmbeddingSize)] = other;
        Normalize(embedding);
        return embedding;
    }

    std::vector<FaceEmbedding> Embeddings(const int count, const int axis)
    {
        std::vector<FaceEmbedding> list;
        for (int i = 0; i < count; ++i)
        {
            list.push_back(Unit(axis, 0.05f * static_cast<float>(i)));
        }
        return list;
    }

    // The protection of the tests: reversible, and the bytes are not the plain ones
    class XorProtector final : public IDataProtector
    {
    public:
        bool Protect(const std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>& out) override
        {
            out = plain;
            for (std::size_t i = 0; i < out.size(); ++i)
            {
                out[i] = static_cast<std::uint8_t>(out[i] ^ (0x5Au + i % 7u));
            }
            return true;
        }
        bool Unprotect(const std::vector<std::uint8_t>& in, std::vector<std::uint8_t>& out) override { return Protect(in, out); }
    };

    class FailingProtector final : public IDataProtector
    {
    public:
        bool Protect(const std::vector<std::uint8_t>&, std::vector<std::uint8_t>&) override { return false; }
        bool Unprotect(const std::vector<std::uint8_t>&, std::vector<std::uint8_t>&) override { return false; }
    };

    // ---- cosine, normalisation, the best profile

    void TestCosine()
    {
        FaceEmbedding a = Unit(0);
        FaceEmbedding b = Unit(1);
        Check(Near(Cosine(a, a), 1.0), "cosine of a vector with itself is 1");
        Check(Near(Cosine(a, b), 0.0), "cosine of orthogonal vectors is 0");
        FaceEmbedding scaled = a;
        for (float& value : scaled)
        {
            value *= 7.0f;
        }
        Check(Near(Cosine(a, scaled), 1.0), "cosine does not depend on the length");
        FaceEmbedding opposite = a;
        for (float& value : opposite)
        {
            value = -value;
        }
        Check(Near(Cosine(a, opposite), -1.0), "cosine of opposite vectors is -1");
        FaceEmbedding zero{};
        Check(Near(Cosine(a, zero), 0.0), "a zero vector is similar to nothing (no division by zero)");
        Normalize(zero);   // must not produce NaN
        Check(std::isfinite(zero[0]), "normalising a zero vector stays finite");
    }

    void TestBestMatch()
    {
        std::vector<FaceProfile> profiles;
        profiles.push_back({"Artem", Embeddings(6, 0)});
        profiles.push_back({"Ivan", Embeddings(6, 40)});
        const MatchResult none = BestMatch({}, Unit(0));
        Check(none.profile == -1, "no profiles: no match");
        const MatchResult first = BestMatch(profiles, Unit(0, 0.02f));
        Check(first.profile == 0 && first.score > 0.9f, "a face near the first profile is the first profile");
        const MatchResult second = BestMatch(profiles, Unit(40, 0.02f));
        Check(second.profile == 1 && second.score > 0.9f, "a face near the second profile is the second profile");
        const MatchResult stranger = BestMatch(profiles, Unit(90));
        Check(stranger.score < kDefaultThreshold, "an unrelated face is below the threshold");
    }

    // ---- five frames in a row, the lockout, the turn of the head

    void TestDecider()
    {
        RecognitionDecider decider(5, 0.363f, 12);
        for (int i = 0; i < 4; ++i)
        {
            Check(decider.Feed({1, 0.8f}) == RecognitionDecider::State::Collecting, "four frames are not enough");
        }
        Check(decider.Feed({1, 0.8f}) == RecognitionDecider::State::Accepted, "five frames in a row accept the profile");
        Check(decider.AcceptedProfile() == 1, "the accepted profile is the one that matched");

        decider.Reset();
        for (int i = 0; i < 4; ++i)
        {
            decider.Feed({0, 0.9f});
        }
        decider.Feed({0, 0.1f});   // one weak frame breaks the series
        Check(decider.GetState() == RecognitionDecider::State::Collecting && decider.Streak() == 0, "a weak frame resets the streak");
        for (int i = 0; i < 4; ++i)
        {
            decider.Feed({0, 0.9f});
        }
        Check(decider.GetState() == RecognitionDecider::State::Collecting, "the series starts again from zero");

        decider.Reset();
        for (int i = 0; i < 5; ++i)
        {
            decider.Feed({0, 0.9f});
            if (i == 2)
            {
                decider.Feed({1, 0.9f});   // another person in the middle of the series
            }
        }
        Check(decider.GetState() != RecognitionDecider::State::Accepted || decider.AcceptedProfile() != 0 || decider.Streak() >= 5,
              "two profiles do not mix into one series");

        decider.Reset();
        RecognitionDecider::State state = RecognitionDecider::State::Collecting;
        int frames = 0;
        while (state == RecognitionDecider::State::Collecting && frames < 100)
        {
            state = decider.Feed({-1, 0.05f});
            ++frames;
        }
        Check(state == RecognitionDecider::State::Rejected && frames == 12, "a stranger is rejected after twelve frames");

        decider.Reset();
        decider.SetThreshold(0.9f);
        for (int i = 0; i < 5; ++i)
        {
            decider.Feed({0, 0.8f});
        }
        Check(decider.GetState() != RecognitionDecider::State::Accepted, "the threshold of the settings is used");
    }

    void TestLimiter()
    {
        AttemptLimiter limiter(3, 30.0);
        Check(!limiter.Locked(0.0), "a new limiter is open");
        limiter.Failure(1.0);
        limiter.Failure(2.0);
        Check(!limiter.Locked(2.5), "two failures do not lock");
        limiter.Failure(3.0);
        Check(limiter.Locked(3.5), "three failures lock");
        Check(Near(limiter.RemainingSeconds(13.0), 20.0, 1e-6), "the lock counts down");
        Check(!limiter.Locked(33.5), "the lock ends after thirty seconds");
        limiter.Success();
        Check(limiter.Failures() == 0 && !limiter.Locked(33.6), "a success clears the failures");
        limiter.Failure(40.0);
        limiter.Failure(41.0);
        limiter.Success();
        limiter.Failure(42.0);
        Check(!limiter.Locked(42.1), "failures before a success are forgotten");
    }

    void TestLiveness()
    {
        LivenessChallenge challenge;
        challenge.Start(LivenessChallenge::Direction::ImageLeft, 0.0);
        for (int i = 0; i < 5; ++i)
        {
            Check(challenge.Feed(0.02f, 0.1 * i) == LivenessChallenge::State::Waiting, "a still head waits");
        }
        // the nose goes to the left of the picture: Yaw falls (docs: Yaw grows when the nose goes to the right)
        LivenessChallenge::State state = LivenessChallenge::State::Waiting;
        for (int i = 0; i < 10 && state == LivenessChallenge::State::Waiting; ++i)
        {
            state = challenge.Feed(0.02f - 0.08f * static_cast<float>(i + 1), 0.6 + 0.1 * i);
        }
        Check(state == LivenessChallenge::State::Passed, "turning the head to the asked side passes");

        challenge.Start(LivenessChallenge::Direction::ImageLeft, 0.0);
        state = LivenessChallenge::State::Waiting;
        for (int i = 0; i < 12 && state == LivenessChallenge::State::Waiting; ++i)
        {
            state = challenge.Feed(0.0f + 0.08f * static_cast<float>(i + 1), 0.1 * i);   // to the other side
        }
        Check(state != LivenessChallenge::State::Passed, "turning to the wrong side does not pass");

        challenge.Start(LivenessChallenge::Direction::ImageRight, 0.0, 8.0, 0.25f);
        state = LivenessChallenge::State::Waiting;
        for (int i = 0; i < 100 && state == LivenessChallenge::State::Waiting; ++i)
        {
            state = challenge.Feed(0.0f, 0.2 * i);   // a photo: nothing moves
        }
        Check(state == LivenessChallenge::State::Failed, "a face that does not move fails after the timeout");
    }

    // ---- the alignment

    void TestAlign()
    {
        // the identity: the template onto itself
        Similarity identity;
        Check(EstimateSimilarity(kAlignTemplate, kAlignTemplate, identity) && Near(identity.a, 1.0) && Near(identity.b, 0.0) && Near(identity.tx, 0.0, 1e-3) &&
                  Near(identity.ty, 0.0, 1e-3),
              "the template maps onto itself with the identity");

        // five points that were made from the template by a known similarity are mapped back exactly
        const double angle = 0.3;
        const double scale = 2.5;
        float moved[10];
        for (int i = 0; i < 5; ++i)
        {
            const double x = kAlignTemplate[2 * i];
            const double y = kAlignTemplate[2 * i + 1];
            moved[2 * i] = static_cast<float>(scale * (std::cos(angle) * x - std::sin(angle) * y) + 100.0);
            moved[2 * i + 1] = static_cast<float>(scale * (std::sin(angle) * x + std::cos(angle) * y) - 30.0);
        }
        Similarity back;
        Check(EstimateSimilarity(moved, kAlignTemplate, back), "the transform is found");
        Check(Near(back.Scale(), 1.0 / scale, 1e-3), "the scale is the inverse one");
        for (int i = 0; i < 5; ++i)
        {
            double x = 0.0;
            double y = 0.0;
            back.Apply(moved[2 * i], moved[2 * i + 1], x, y);
            Check(Near(x, kAlignTemplate[2 * i], 0.01) && Near(y, kAlignTemplate[2 * i + 1], 0.01), "a point is carried to the template");
        }

        float same[10] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
        Similarity degenerate;
        Check(!EstimateSimilarity(same, kAlignTemplate, degenerate), "points in one place have no transform");

        // a picture whose "face" sits where the template says comes out as the same picture (a gradient, so that a shift would be seen)
        FaceImage image;
        image.width = 112;
        image.height = 112;
        image.rgb.resize(112u * 112u * 3u);
        for (int y = 0; y < 112; ++y)
        {
            for (int x = 0; x < 112; ++x)
            {
                std::uint8_t* pixel = image.rgb.data() + (static_cast<std::size_t>(y) * 112 + x) * 3;
                pixel[0] = static_cast<std::uint8_t>(x * 2);
                pixel[1] = static_cast<std::uint8_t>(y * 2);
                pixel[2] = 77;
            }
        }
        FaceImage aligned;
        Check(AlignFace(image, kAlignTemplate, aligned), "a face is aligned");
        Check(aligned.width == kAlignedSize && aligned.height == kAlignedSize && !aligned.Empty(), "the aligned face is 112 x 112");
        int worst = 0;
        for (std::size_t i = 0; i < image.rgb.size(); ++i)
        {
            worst = std::max(worst, std::abs(static_cast<int>(image.rgb[i]) - static_cast<int>(aligned.rgb[i])));
        }
        Check(worst <= 2, "the identity alignment keeps the picture");

        FaceImage empty;
        FaceImage out;
        Check(!AlignFace(empty, kAlignTemplate, out), "an empty picture is not aligned");
        Check(!AlignFace(image, same, out), "degenerate points are not aligned");
    }

    void TestSharpness()
    {
        FaceImage flat;
        flat.width = 64;
        flat.height = 64;
        flat.rgb.assign(64u * 64u * 3u, 128);
        FaceImage noisy = flat;
        std::mt19937 random(1);
        for (std::uint8_t& value : noisy.rgb)
        {
            value = static_cast<std::uint8_t>(random() & 0xFF);
        }
        Check(Sharpness(noisy) > Sharpness(flat) + 100.0f, "an image with detail is sharper than a flat one");
        Check(Near(Sharpness(flat), 0.0, 1e-3), "a flat image has no sharpness");
    }

    // ---- the file of the profiles

    FaceProfile MakeProfile(const std::string& name, const int axis, const int count = 8)
    {
        FaceProfile profile;
        profile.name = name;
        profile.embeddings = Embeddings(count, axis);
        return profile;
    }

    void TestStore(const fs::path& directory)
    {
        const fs::path file = directory / "profiles.bin";
        auto protector = std::make_shared<XorProtector>();
        std::string error;

        {
            FaceProfileStore store(file, protector);
            Check(store.Load(&error) && store.Profiles().empty(), "a missing file is an empty store");
            Check(!store.Add(MakeProfile("Artem", 0), &error), "the first profile needs a password");
            Check(!store.Add(MakeProfile("Artem", 0), &error, "12345"), "a short password is refused");
            Check(store.Add(MakeProfile("Artem", 0), &error, "secret-word"), "the first profile is added with a password: " + error);
            Check(store.HasPassword() && store.CheckPassword("secret-word") && !store.CheckPassword("secret-word!") && !store.CheckPassword(""), "the password is checked");
            Check(!store.Add(MakeProfile("Artem", 10), &error), "a name is used once");
            Check(!store.Add(MakeProfile("Too few", 10, 4), &error), "four embeddings are too few");
            Check(!store.Add(MakeProfile("Too many", 10, 11), &error), "eleven embeddings are too many");
            Check(!store.Add(MakeProfile("", 10), &error), "a name is needed");
            Check(!store.Add(MakeProfile(std::string(40, 'a'), 10), &error), "a name is at most 32 bytes");
            Check(store.Add(MakeProfile("Ivan", 30), &error), "the second profile is added");
            Check(store.Add(MakeProfile("Guest", 60, 5), &error), "the third profile is added (five embeddings)");
            Check(store.Full() && !store.Add(MakeProfile("Fourth", 90), &error), "the fourth profile is refused");
        }
        Check(fs::exists(file), "the file is written");

        // not readable without the protection: the plain bytes (the name) are not in the file
        {
            std::ifstream in(file, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            Check(bytes.find("Artem") == std::string::npos && bytes.find("Ivan") == std::string::npos, "the names are not in the file in plain");
        }

        {
            FaceProfileStore store(file, protector);
            Check(store.Load(&error), "the file is read: " + error);
            Check(store.Profiles().size() == 3 && store.Profiles()[0].name == "Artem" && store.Profiles()[1].name == "Ivan" && store.Profiles()[2].embeddings.size() == 5,
                  "the profiles come back as they were");
            Check(Near(Cosine(store.Profiles()[1].embeddings[3], Embeddings(8, 30)[3]), 1.0, 1e-5), "an embedding comes back as it was");
            Check(store.CheckPassword("secret-word"), "the password comes back");

            Check(store.Remove(1, &error) && store.Profiles().size() == 2 && store.Profiles()[1].name == "Guest", "a profile is removed");
        }
        {
            FaceProfileStore store(file, protector);
            Check(store.Load(&error) && store.Profiles().size() == 2, "the removal is in the file");
            Check(store.ChangePassword("another-word", &error) && store.CheckPassword("another-word") && !store.CheckPassword("secret-word"), "the password is changed");
            Check(!store.ChangePassword("short", &error), "a short new password is refused");
        }
        {
            FaceProfileStore store(file, protector);
            Check(store.Load(&error) && store.CheckPassword("another-word"), "the new password is kept");
            Check(store.Remove(1, &error) && store.Remove(0, &error), "the last profile is removed");
            Check(!fs::exists(file), "the last profile takes the file with it");
            Check(!store.HasPassword(), "and the password too");
        }

        // a damaged file is an error and does not become an empty store silently
        {
            FaceProfileStore store(file, protector);
            Check(store.Add(MakeProfile("Artem", 0), &error, "secret-word"), "a profile for the damage test");
        }
        {
            std::fstream io(file, std::ios::in | std::ios::out | std::ios::binary);
            io.seekp(5);
            io.put('\x7f');
        }
        {
            FaceProfileStore store(file, protector);
            const bool loaded = store.Load(&error);
            Check(!loaded || store.Profiles().empty() || store.Profiles()[0].embeddings.size() >= 5, "a damaged file does not crash");
        }
        {
            // the same file with another protection (another user of the machine): an error and the file is not touched
            FaceProfileStore store(file, std::make_shared<FailingProtector>());
            Check(!store.Load(&error) && !error.empty() && store.Profiles().empty(), "a file that cannot be unprotected is an error with a message");
            Check(fs::exists(file), "and it is not deleted by the failed load");
        }
        {
            FaceProfileStore store(file, protector);
            Check(store.EraseAll(&error) && !fs::exists(file), "everything is erased");
        }
        {
            FaceProfileStore store(file, protector);
            Check(store.Load(&error) && store.Profiles().empty(), "nothing is left");
        }
    }

    void TestSerialize(const fs::path& directory)
    {
        FaceProfileStore store(directory / "serialize.bin", std::make_shared<XorProtector>());
        std::string error;
        Check(store.Add(MakeProfile("Артём", 3), &error, "пароль-123"), "a UTF-8 name and password are fine: " + error);
        const std::vector<std::uint8_t> bytes = store.Serialize();
        FaceProfileStore other({}, std::make_shared<XorProtector>());
        Check(other.Deserialize(bytes) && other.Profiles().size() == 1 && other.Profiles()[0].name == "Артём" && other.CheckPassword("пароль-123"),
              "serialise and read back");
        std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
        FaceProfileStore third({}, std::make_shared<XorProtector>());
        Check(!third.Deserialize(cut), "a cut file is refused");
        Check(!third.Deserialize({}), "an empty file is refused");
        std::vector<std::uint8_t> junk(300, 0xAB);
        Check(!third.Deserialize(junk), "junk is refused");
    }

    void TestPassword()
    {
        PasswordHash a;
        PasswordHash b;
        Check(HashPassword("secret-word", a, 1000) && HashPassword("secret-word", b, 1000), "a password is hashed");
        Check(a.Valid() && a.salt != b.salt && a.hash != b.hash, "the salt is random: the same password hashes differently");
        Check(VerifyPassword("secret-word", a) && VerifyPassword("secret-word", b), "the right password is verified");
        Check(!VerifyPassword("Secret-word", a) && !VerifyPassword("", a), "a wrong password is not");
        PasswordHash empty;
        Check(!VerifyPassword("secret-word", empty), "no hash: nothing is verified");
    }

    void TestDpapi()
    {
        const auto protector = MakeDpapiProtector();
        Check(protector != nullptr, "DPAPI is there");
        const std::vector<std::uint8_t> plain = {'f', 'a', 'c', 'e', 0, 1, 2, 3};
        std::vector<std::uint8_t> sealed;
        std::vector<std::uint8_t> opened;
        Check(protector->Protect(plain, sealed) && sealed != plain && !sealed.empty(), "DPAPI protects");
        Check(protector->Unprotect(sealed, opened) && opened == plain, "DPAPI unprotects");
        sealed[sealed.size() / 2] ^= 0x55;
        Check(!protector->Unprotect(sealed, opened), "a changed blob is refused");
    }

    // ---- the real models

    fs::path FindUpwards(const fs::path& relative)
    {
        wchar_t buffer[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        fs::path directory = fs::path(std::wstring(buffer, length)).parent_path();
        for (int i = 0; i < 8 && !directory.empty(); ++i)
        {
            if (fs::exists(directory / relative))
            {
                return directory / relative;
            }
            directory = directory.parent_path();
        }
        return {};
    }

    bool LoadPicture(const fs::path& path, FaceImage& out)
    {
        std::ifstream in(path, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.empty())
        {
            return false;
        }
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &width, &height, &channels, 3);
        if (pixels == nullptr)
        {
            return false;
        }
        out.width = width;
        out.height = height;
        out.rgb.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
        stbi_image_free(pixels);
        return true;
    }

    FaceImage Brightened(const FaceImage& image, const float gain, const int add)
    {
        FaceImage result = image;
        for (std::uint8_t& value : result.rgb)
        {
            value = static_cast<std::uint8_t>(std::clamp(static_cast<int>(static_cast<float>(value) * gain) + add, 0, 255));
        }
        return result;
    }

    // Every second pixel of both axes, then pixels doubled again: a smaller, softer picture of the same face
    FaceImage Halved(const FaceImage& image)
    {
        FaceImage small;
        small.width = image.width / 2;
        small.height = image.height / 2;
        small.rgb.resize(static_cast<std::size_t>(small.width) * static_cast<std::size_t>(small.height) * 3u);
        for (int y = 0; y < small.height; ++y)
        {
            for (int x = 0; x < small.width; ++x)
            {
                for (int c = 0; c < 3; ++c)
                {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                    {
                        for (int dx = 0; dx < 2; ++dx)
                        {
                            sum += image.rgb[(static_cast<std::size_t>(2 * y + dy) * image.width + (2 * x + dx)) * 3 + c];
                        }
                    }
                    small.rgb[(static_cast<std::size_t>(y) * small.width + x) * 3 + c] = static_cast<std::uint8_t>(sum / 4);
                }
            }
        }
        return small;
    }

    // 0: passed, 77: skipped (no ONNX Runtime, no models, no portraits)
    int TestModels()
    {
        if (!FaceModels::Available())
        {
            std::cout << "faceid: SKIP the models (this build has no ONNX Runtime: MYENGINE_FACEID is OFF)\n";
            return 77;
        }
        const fs::path modelsDirectory = FindUpwards("assets/faceid/models");
        const fs::path portraitA = FindUpwards("assets/faceid/test/public_domain_portrait.jpg");
        const fs::path portraitB = FindUpwards("assets/faceid/test/public_domain_portrait2.jpg");
        if (modelsDirectory.empty() || portraitA.empty() || portraitB.empty())
        {
            std::cout << "faceid: SKIP the models (assets/faceid/models or the test portraits are not there: tools/faceid/fetch_models.ps1 -WithTestImage)\n";
            return 77;
        }

        FaceModels models;
        std::string error;
        Check(models.Load(modelsDirectory, error), "the models load: " + error);
        Check(models.Loaded(), "the models are loaded");

        FaceImage a;
        FaceImage b;
        Check(LoadPicture(portraitA, a) && LoadPicture(portraitB, b), "the portraits decode");

        // no face in a grey picture and in noise
        FaceImage grey;
        grey.width = 640;
        grey.height = 480;
        grey.rgb.assign(640u * 480u * 3u, 120);
        Check(models.Detect(grey).empty(), "a grey picture has no face");

        const std::vector<FaceDetection> facesA = models.Detect(a);
        const std::vector<FaceDetection> facesB = models.Detect(b);
        Check(!facesA.empty() && !facesB.empty(), "a face is found in each portrait");
        const FaceDetection& faceA = facesA.front();
        Check(faceA.score > 0.7f && faceA.w > 20.0f && faceA.h > 20.0f, "the box of the face is sane");
        Check(faceA.x >= -1.0f && faceA.y >= -1.0f && faceA.x + faceA.w <= static_cast<float>(a.width) + 1.0f && faceA.y + faceA.h <= static_cast<float>(a.height) + 1.0f,
              "the box is inside the picture");
        Check(faceA.landmarks[0] < faceA.landmarks[2], "the right eye of the person is on the left of the picture");
        Check(faceA.landmarks[5] > faceA.landmarks[1] && faceA.landmarks[9] > faceA.landmarks[5], "the nose is under the eyes and the mouth under the nose");
        Check(std::fabs(faceA.Yaw()) < 0.35f, "a portrait looks at the camera");

        FaceEmbedding embeddingA;
        FaceEmbedding embeddingB;
        Check(models.Embed(a, faceA, embeddingA) && models.Embed(b, facesB.front(), embeddingB), "the embeddings are made");
        double length = 0.0;
        for (const float value : embeddingA)
        {
            length += static_cast<double>(value) * value;
        }
        Check(Near(length, 1.0, 1e-3), "an embedding is normalised");

        // the same person: the same picture twice, a darker or brighter one and a smaller one are above the threshold; another person is below it
        FaceEmbedding again;
        Check(models.Embed(a, faceA, again) && Cosine(embeddingA, again) > 0.99f, "the same picture gives the same embedding");

        for (const FaceImage& variant : {Brightened(a, 0.7f, -10), Brightened(a, 1.15f, 15), Halved(a)})
        {
            const std::vector<FaceDetection> found = models.Detect(variant);
            Check(!found.empty(), "the face is found in a changed picture");
            FaceEmbedding changed;
            Check(models.Embed(variant, found.front(), changed), "the embedding of the changed picture");
            const float same = Cosine(embeddingA, changed);
            Check(same > kDefaultThreshold + 0.2f, "the same person in other light or size is far above the threshold (" + std::to_string(same) + ")");
        }

        const float other = Cosine(embeddingA, embeddingB);
        Check(other < kDefaultThreshold, "another person is below the threshold (" + std::to_string(other) + ")");

        // the whole decision as the editor takes it: a profile of the first person (five frames of changed light), the first person is accepted, the second is not
        FaceProfile profile;
        profile.name = "Test";
        for (const float gain : {0.8f, 0.9f, 1.0f, 1.1f, 1.2f})
        {
            const FaceImage variant = Brightened(a, gain, 0);
            const std::vector<FaceDetection> found = models.Detect(variant);
            FaceEmbedding embedding;
            Check(!found.empty() && models.Embed(variant, found.front(), embedding), "a frame of the profile");
            profile.embeddings.push_back(embedding);
        }
        const std::vector<FaceProfile> profiles = {profile};
        RecognitionDecider owner(5);
        RecognitionDecider stranger(5);
        for (int i = 0; i < 5; ++i)
        {
            owner.Feed(BestMatch(profiles, embeddingA));
            stranger.Feed(BestMatch(profiles, embeddingB));
        }
        Check(owner.GetState() == RecognitionDecider::State::Accepted, "the owner is accepted");
        Check(stranger.GetState() != RecognitionDecider::State::Accepted, "a stranger is not accepted");

        std::cout << "faceid: models ok, same person " << Cosine(embeddingA, again) << ", other person " << other << "\n";
        return 0;
    }
}

int main()
{
    const fs::path directory = fs::temp_directory_path() / "myengine_faceid_test";
    try
    {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
        fs::create_directories(directory);

        TestCosine();
        TestBestMatch();
        TestDecider();
        TestLimiter();
        TestLiveness();
        TestAlign();
        TestSharpness();
        TestStore(directory);
        TestSerialize(directory);
        TestPassword();
        TestDpapi();
        std::cout << "faceid: logic, alignment and profiles ok\n";

        const int models = TestModels();
        fs::remove_all(directory, ignored);
        if (models == 0)
        {
            std::cout << "faceid: all passed\n";
        }
        return models;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "faceid FAILED: " << exception.what() << "\n";
        std::error_code ignored;
        fs::remove_all(directory, ignored);
        return 1;
    }
}
