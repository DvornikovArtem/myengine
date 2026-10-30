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
#include <myengine/faceid/FaceDatabase.h>
#include <myengine/faceid/FaceIdService.h>
#include <myengine/faceid/FaceIdTrustedKey.h>
#include <myengine/faceid/FaceSigner.h>
#include <myengine/faceid/FaceMatcher.h>
#include <myengine/faceid/FaceModels.h>
#include <myengine/faceid/FacePhotos.h>
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

    // ---- F1: the signer, the shared database, where it is looked for, the modes of the service

    std::vector<std::uint8_t> Bytes(const std::string& text) { return std::vector<std::uint8_t>(text.begin(), text.end()); }

    FaceDatabase MakeDatabase(const int count, const std::string& password = "publisher-pass")
    {
        FaceDatabase database;
        const char* names[] = {"Artem", "Friend", "Teacher", "Fourth", "Fifth", "Sixth", "Seventh", "Eighth", "Ninth"};
        for (int i = 0; i < count; ++i)
        {
            database.faces.push_back({names[i], "2026-10-29", Embeddings(8, 12 * i)});
        }
        database.created = "2026-10-29";
        if (!password.empty())
        {
            Check(HashPassword(password, database.password, 2000), "the password hash of the database");
        }
        return database;
    }

    void WriteBytes(const fs::path& file, const std::vector<std::uint8_t>& bytes)
    {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void TestHex()
    {
        std::vector<std::uint8_t> bytes;
        Check(FromHex("00ff10Ab", bytes) && bytes.size() == 4 && bytes[1] == 0xFF && bytes[3] == 0xAB, "hex is read");
        Check(ToHex(bytes) == "00ff10ab", "hex is written in lower case");
        Check(!FromHex("", bytes) && !FromHex("abc", bytes) && !FromHex("zz", bytes) && !FromHex("0x00", bytes), "bad hex is refused");
        Check(BuiltInPublicKey().empty() == (kTrustedPublicKeyHex[0] == '\0'), "the built-in key is read from the header");
    }

    void TestSigner(const fs::path& directory)
    {
        const auto protector = std::make_shared<XorProtector>();
        const fs::path keyFile = directory / "signer" / "signing.key";
        std::string error;
        FaceSigner signer(keyFile, protector);
        Check(signer.Load(&error) && !signer.HasKey(), "no file is no key, not an error");
        std::vector<std::uint8_t> signature;
        Check(!signer.Sign(Bytes("x"), signature, &error), "without a key nothing is signed");
        Check(signer.Generate(&error) && signer.HasKey() && signer.PublicKey().size() == kPublicKeySize, "a key is made: " + error);
        Check(fs::exists(keyFile), "the key file is written");
        Check(!signer.Generate(&error), "a second key is refused (the first would be lost)");

        const std::vector<std::uint8_t> message = Bytes("the faces of the database");
        Check(signer.Sign(message, signature, &error) && signature.size() == kSignatureSize, "a message is signed");
        Check(VerifySignature(signer.PublicKey(), message, signature), "the signature fits the public key");
        std::vector<std::uint8_t> changed = message;
        changed[3] ^= 1;
        Check(!VerifySignature(signer.PublicKey(), changed, signature), "a changed message does not fit");
        std::vector<std::uint8_t> badSignature = signature;
        badSignature[10] ^= 0x40;
        Check(!VerifySignature(signer.PublicKey(), message, badSignature), "a changed signature does not fit");
        badSignature = signature;
        badSignature.pop_back();
        Check(!VerifySignature(signer.PublicKey(), message, badSignature), "a short signature does not fit");

        FaceSigner other(directory / "signer" / "other.key", protector);
        Check(other.Generate(&error) && other.PublicKey() != signer.PublicKey(), "another key is another key");
        Check(!VerifySignature(other.PublicKey(), message, signature), "another publisher's key does not fit");
        std::vector<std::uint8_t> notOnCurve(kPublicKeySize, 7);
        Check(!VerifySignature(notOnCurve, message, signature), "a point that is not on the curve is no key");
        Check(!VerifySignature({}, message, signature), "no key: nothing fits");

        // the key comes back from the file; the bytes of the file do not hold the private key in plain
        FaceSigner again(keyFile, protector);
        Check(again.Load(&error) && again.HasKey() && again.PublicKey() == signer.PublicKey(), "the key is read from its file: " + error);
        std::vector<std::uint8_t> signedAgain;
        Check(again.Sign(message, signedAgain, &error) && VerifySignature(signer.PublicKey(), message, signedAgain), "the key that was read signs");
        FaceSigner unprotectFails(keyFile, std::make_shared<FailingProtector>());
        Check(!unprotectFails.Load(&error) && !error.empty() && !unprotectFails.HasKey(), "a key of another Windows account is an error with words");
        Check(fs::exists(keyFile), "and the file is kept");
        {
            std::fstream io(keyFile, std::ios::in | std::ios::out | std::ios::binary);
            io.seekp(2);
            io.put('\x55');
        }
        FaceSigner damaged(keyFile, protector);
        Check(!damaged.Load(&error) && !damaged.HasKey(), "a damaged key file is an error");

        // the backup
        const fs::path backup = directory / "signer" / "backup.key";
        Check(!signer.Export(backup, "short", &error), "the password of a backup needs 8 characters");
        Check(signer.Export(backup, "backup-password", &error), "the key is exported: " + error);
        Check(!signer.Export(backup, "backup-password", &error), "a backup does not overwrite a file");
        {
            std::ifstream in(backup, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            Check(bytes.size() > 100, "the backup has the key in it");
        }
        FaceSigner imported(directory / "signer2" / "signing.key", protector);
        Check(!imported.Import(backup, "wrong-password", &error) && !imported.HasKey(), "a wrong password does not open the backup");
        Check(!fs::exists(directory / "signer2" / "signing.key"), "and nothing is written");
        Check(imported.Import(backup, "backup-password", &error) && imported.PublicKey() == signer.PublicKey(), "the key is imported: " + error);
        Check(!imported.Import(backup, "backup-password", &error), "a key is not replaced by an import");
        std::vector<std::uint8_t> signedImported;
        Check(imported.Sign(message, signedImported, &error) && VerifySignature(signer.PublicKey(), message, signedImported), "the imported key signs the same way");
        {
            // a changed byte of the backup: the tag of GCM does not match
            std::vector<std::uint8_t> bytes;
            {
                std::ifstream in(backup, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            bytes[bytes.size() / 2] ^= 0x01;
            WriteBytes(directory / "signer" / "changed.key", bytes);
            FaceSigner third(directory / "signer3" / "signing.key", protector);
            Check(!third.Import(directory / "signer" / "changed.key", "backup-password", &error) && !third.HasKey(), "a changed backup is refused");
            Check(!third.Import(directory / "signer" / "missing.key", "backup-password", &error), "a missing backup is an error");
            WriteBytes(directory / "signer" / "junk.key", std::vector<std::uint8_t>(200, 0x11));
            Check(!third.Import(directory / "signer" / "junk.key", "backup-password", &error), "junk is not a backup");
        }
    }

    void TestDatabaseFile(const fs::path& directory)
    {
        const auto protector = std::make_shared<XorProtector>();
        std::string error;
        FaceSigner signer(directory / "db" / "signing.key", protector);
        FaceSigner stranger(directory / "db" / "stranger.key", protector);
        Check(signer.Generate(&error) && stranger.Generate(&error), "two keys");
        const std::vector<std::vector<std::uint8_t>> trusted = {signer.PublicKey()};

        const FaceDatabase database = MakeDatabase(3);
        std::vector<std::uint8_t> file;
        Check(SignDatabase(database, signer, file, &error), "the database is signed: " + error);
        const DatabaseCheck ok = CheckDatabase(file, trusted);
        Check(ok.status == DatabaseStatus::Ok && ok.keyIndex == 0, "a signed database passes: " + ok.message);
        Check(ok.database.faces.size() == 3 && ok.database.faces[1].name == "Friend" && ok.database.faces[2].date == "2026-10-29" && ok.database.created == "2026-10-29",
              "the faces come back as they were");
        Check(Near(Cosine(ok.database.faces[2].embeddings[4], database.faces[2].embeddings[4]), 1.0, 1e-5), "an embedding comes back as it was");
        Check(ok.database.password.Valid() && VerifyPassword("publisher-pass", ok.database.password) && !VerifyPassword("other", ok.database.password), "the password comes back");
        Check(ok.database.Profiles().size() == 3 && ok.database.Profiles()[0].name == "Artem", "the faces make the profiles");

        // the embeddings are in the file in plain (the file is protected against a change, not against reading): the name is readable too
        {
            const std::string text(file.begin(), file.end());
            Check(text.find("Teacher") != std::string::npos, "the names are in the file, as the doc says");
        }

        // without a password
        {
            std::vector<std::uint8_t> noPassword;
            Check(SignDatabase(MakeDatabase(2, ""), signer, noPassword, &error), "a database without a password is signed");
            const DatabaseCheck check = CheckDatabase(noPassword, trusted);
            Check(check.status == DatabaseStatus::Ok && !check.database.password.Valid(), "it has no password");
        }

        // a changed byte anywhere: the header, the body, the signature
        int tried = 0;
        for (std::size_t at = 0; at < file.size(); at += (at < 40 || at + 80 > file.size()) ? 1 : 13)
        {
            std::vector<std::uint8_t> bad = file;
            bad[at] ^= 0x10;
            const DatabaseCheck check = CheckDatabase(bad, trusted);
            Check(check.status != DatabaseStatus::Ok, "a changed byte at " + std::to_string(at) + " is not accepted");
            Check(check.message.find("damaged or forged") != std::string::npos, "and the message says so");
            ++tried;
        }
        Check(tried > 200, "many positions were tried");

        // a face that is added by hand to the signed file (the case that matters): the body changes, the signature stays
        {
            FaceDatabase bigger = MakeDatabase(4);
            std::vector<std::uint8_t> biggerFile;
            Check(SignDatabase(bigger, stranger, biggerFile, &error), "the stranger's own database");
            const std::vector<std::uint8_t> body = SerializeDatabaseBody(bigger);
            std::vector<std::uint8_t> spliced = file;   // header and signature of the real one, the body of the bigger one
            const std::size_t oldBody = spliced.size() - 12 - 4 - kSignatureSize;
            spliced.erase(spliced.begin() + 12, spliced.begin() + 12 + static_cast<std::ptrdiff_t>(oldBody));
            spliced.insert(spliced.begin() + 12, body.begin(), body.end());
            for (int i = 0; i < 4; ++i)
            {
                spliced[8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(body.size() >> (8 * i));
            }
            Check(CheckDatabase(spliced, trusted).status == DatabaseStatus::BadSignature, "a face added to the signed file is rejected");
            Check(CheckDatabase(biggerFile, trusted).status == DatabaseStatus::BadSignature, "a database signed by a stranger is rejected");
        }

        Check(CheckDatabase(file, {stranger.PublicKey()}).status == DatabaseStatus::BadSignature, "another publisher's key does not accept it");
        const DatabaseCheck two = CheckDatabase(file, {stranger.PublicKey(), signer.PublicKey()});
        Check(two.status == DatabaseStatus::Ok && two.keyIndex == 1, "any of the trusted keys is enough");
        const DatabaseCheck none = CheckDatabase(file, {});
        Check(none.status == DatabaseStatus::NoTrustedKey && none.message.find("no key") != std::string::npos, "no key built in: the database is not used");

        std::vector<std::uint8_t> cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(file.size() / 2));
        Check(CheckDatabase(cut, trusted).status == DatabaseStatus::Damaged, "a cut file is damaged");
        Check(CheckDatabase({}, trusted).status == DatabaseStatus::Damaged, "an empty file is damaged");
        std::vector<std::uint8_t> longer = file;
        longer.push_back(0);
        Check(CheckDatabase(longer, trusted).status == DatabaseStatus::Damaged, "something after the end is not accepted");
        Check(CheckDatabase(std::vector<std::uint8_t>(500, 0x42), trusted).status == DatabaseStatus::Damaged, "junk is damaged");

        // the rules of the contents are the same on both sides: a database that breaks them is not signed
        {
            std::vector<std::uint8_t> out;
            FaceDatabase empty = MakeDatabase(1);
            empty.faces.clear();
            Check(!SignDatabase(empty, signer, out, &error), "a database without faces is not signed");
            Check(!SignDatabase(MakeDatabase(9), signer, out, &error), "more than eight faces are not signed");
            FaceDatabase twice = MakeDatabase(2);
            twice.faces[1].name = twice.faces[0].name;
            Check(!SignDatabase(twice, signer, out, &error), "a name used twice is not signed");
            FaceDatabase fewPictures = MakeDatabase(2);
            fewPictures.faces[0].embeddings.resize(3);
            Check(!SignDatabase(fewPictures, signer, out, &error), "three pictures are too few");
            FaceDatabase badDate = MakeDatabase(2);
            badDate.faces[0].date = "yesterday";
            Check(!SignDatabase(badDate, signer, out, &error), "a date is YYYY-MM-DD");
            FaceDatabase nan = MakeDatabase(2);
            nan.faces[0].embeddings[0][5] = std::nanf("");
            Check(!SignDatabase(nan, signer, out, &error), "a number that is not a number is not signed");
            FaceSigner keyless(directory / "db" / "none.key", protector);
            Check(!SignDatabase(MakeDatabase(2), keyless, out, &error), "no key: nothing is signed");
        }

        // a correct signature does not make a body that lies safe to read: the parser alone survives anything
        {
            const std::vector<std::uint8_t> body = SerializeDatabaseBody(database);
            std::mt19937 random(7);
            for (int round = 0; round < 3000; ++round)
            {
                std::vector<std::uint8_t> mutated = body;
                const int changes = 1 + static_cast<int>(random() % 4);
                for (int i = 0; i < changes; ++i)
                {
                    mutated[random() % mutated.size()] = static_cast<std::uint8_t>(random());
                }
                if (round % 5 == 0)
                {
                    mutated.resize(random() % mutated.size());
                }
                FaceDatabase parsed;
                ParseDatabaseBody(mutated, parsed, nullptr);   // must neither crash nor read outside
            }
        }

        // the file on disk
        const fs::path path = directory / "db" / "allowed_faces.fdb";
        Check(SaveDatabaseFile(path, file, &error) && fs::exists(path), "the file is written");
        Check(LoadDatabase(path, trusted).status == DatabaseStatus::Ok, "and read back");
        Check(LoadDatabase(directory / "db" / "missing.fdb", trusted).status == DatabaseStatus::Unreadable, "a missing file is unreadable");
    }

    void TestFindDatabase(const fs::path& directory)
    {
        const fs::path exe = directory / "find" / "exe";
        const fs::path named = directory / "find" / "named.fdb";
        const std::vector<std::uint8_t> bytes = {1, 2, 3};
        Check(FindDatabase({}, exe).path.empty(), "nothing is there: no database");
        WriteBytes(exe / "assets" / "faceid" / kDatabaseFileName, bytes);
        DatabaseLocation found = FindDatabase({}, exe);
        Check(found.exists && !found.named && found.path == exe / "assets" / "faceid" / kDatabaseFileName, "assets/faceid is the last place");
        WriteBytes(exe / "faceid" / kDatabaseFileName, bytes);
        found = FindDatabase({}, exe);
        Check(found.exists && found.path == exe / "faceid" / kDatabaseFileName, "faceid/ next to the exe comes before assets/");
        WriteBytes(named, bytes);
        found = FindDatabase(named, exe);
        Check(found.exists && found.named && found.path == named, "the named file comes first");
        found = FindDatabase(directory / "find" / "nothing.fdb", exe);
        Check(!found.exists && found.named && !found.path.empty(), "a named file that is not there is reported, not replaced by another one");
        Check(FindDatabase({}, {}).path.empty(), "no folder of the exe and no name: nothing");
    }

    struct ServiceFixture
    {
        fs::path root;
        std::shared_ptr<XorProtector> protector = std::make_shared<XorProtector>();

        explicit ServiceFixture(const fs::path& base, const std::string& name) : root(base / "service" / name) { fs::create_directories(root / "exe"); }

        FaceIdService::Options Options(const std::string& trustedHex, const fs::path& named = {}) const
        {
            FaceIdService::Options options;
            options.directory = root / "data";
            options.modelsDirectory = root / "no-models";   // the modes do not need the networks
            options.protector = protector;
            options.executableDirectory = root / "exe";
            options.databaseFile = named;
            options.overrideTrustedKey = true;
            options.trustedKeyHex = trustedHex;
            return options;
        }
        fs::path DatabasePath() const { return root / "exe" / "faceid" / kDatabaseFileName; }
    };

    void TestServiceModes(const fs::path& directory)
    {
        std::string error;
        const auto protector = std::make_shared<XorProtector>();
        FaceSigner publisher(directory / "svc-keys" / "publisher.key", protector);
        FaceSigner stranger(directory / "svc-keys" / "stranger.key", protector);
        Check(publisher.Generate(&error) && stranger.Generate(&error), "keys for the service tests");
        const std::string trusted = ToHex(publisher.PublicKey());

        // Local: no database
        {
            ServiceFixture fixture(directory, "local");
            FaceIdService service(fixture.Options(trusted));
            service.Initialize();
            Check(service.Source() == FaceSource::Local && !service.SharedBlocked() && service.ActiveProfiles().empty(), "no database: the Local mode");
            Check(!service.PasswordAvailable(), "no profiles, no password");
            FaceProfile profile = MakeProfile("Mine", 5);
            Check(service.Store().Add(profile, &error, "local-password"), "a local profile");
            Check(service.ActiveProfiles().size() == 1 && service.PasswordAvailable() && service.LoginByPassword("local-password") && !service.LoginByPassword("other"),
                  "the local profile and password are used");
        }

        // Shared: a signed database decides, the local profiles do not count
        {
            ServiceFixture fixture(directory, "shared");
            std::vector<std::uint8_t> file;
            Check(SignDatabase(MakeDatabase(3), publisher, file, &error), "a database for the shared mode");
            WriteBytes(fixture.DatabasePath(), file);
            {
                FaceIdService seed(fixture.Options(trusted));
                seed.Initialize();
            }
            FaceProfileStore local(fixture.root / "data" / "profiles.bin", fixture.protector);
            Check(local.Add(MakeProfile("Intruder", 77), &error, "local-password"), "a local profile that the shared mode must ignore");
            FaceIdService service(fixture.Options(trusted));
            service.Initialize();
            Check(service.Source() == FaceSource::Shared && service.Database().valid && !service.SharedBlocked(), "a signed database: the Shared mode");
            Check(service.Database().faces == 3 && service.Database().created == "2026-10-29" && service.Database().hasPassword && service.Database().signedByBuiltInKey, "what is known about it");
            Check(service.ActiveProfiles().size() == 3 && service.ActiveProfiles()[0].name == "Artem", "the faces of the database are in use");
            Check(std::none_of(service.ActiveProfiles().begin(), service.ActiveProfiles().end(), [](const FaceProfile& p) { return p.name == "Intruder"; }), "the local profile is not");
            Check(service.PasswordAvailable() && service.LoginByPassword("publisher-pass"), "the password of the publisher works");
            Check(!service.LoginByPassword("local-password"), "the local password does not");
            Check(!service.IsPublisher() && service.PublisherKeyNote().empty(), "this computer has no key: it is not a publisher");
            std::string refused;
            Check(!service.PublisherSave(fixture.root / "x.fdb", false, "", &refused) && !fs::exists(fixture.root / "x.fdb"), "a computer without a key cannot sign");
        }

        // Shared, no password in the database: the camera is the only way
        {
            ServiceFixture fixture(directory, "nopassword");
            std::vector<std::uint8_t> file;
            Check(SignDatabase(MakeDatabase(2, ""), publisher, file, &error), "a database without a password");
            WriteBytes(fixture.DatabasePath(), file);
            FaceIdService service(fixture.Options(trusted));
            service.Initialize();
            Check(service.Source() == FaceSource::Shared && !service.PasswordAvailable() && !service.LoginByPassword("anything"), "no password in the database: none works");
        }

        // a changed database closes the door: no face, no password
        {
            ServiceFixture fixture(directory, "forged");
            std::vector<std::uint8_t> file;
            Check(SignDatabase(MakeDatabase(3), publisher, file, &error), "a database to forge");
            file[file.size() / 2] ^= 0x04;
            WriteBytes(fixture.DatabasePath(), file);
            FaceIdService service(fixture.Options(trusted));
            service.Initialize();
            Check(service.Source() == FaceSource::Shared && service.SharedBlocked() && !service.Database().valid, "a forged database blocks");
            Check(service.Database().error.find("damaged or forged") != std::string::npos, "with the words for the screen");
            Check(service.ActiveProfiles().empty() && !service.PasswordAvailable() && !service.LoginByPassword("publisher-pass"), "no face and no password is accepted");
            Check(!service.IsPublisher(), "and the machine is not a publisher");
        }

        // another publisher's database, a build with no key, a named file that is not there
        {
            ServiceFixture fixture(directory, "other");
            std::vector<std::uint8_t> file;
            Check(SignDatabase(MakeDatabase(3), stranger, file, &error), "a database of a stranger");
            WriteBytes(fixture.DatabasePath(), file);
            {
                FaceIdService service(fixture.Options(trusted));
                service.Initialize();
                Check(service.SharedBlocked() && service.Database().error.find("signed by somebody else") != std::string::npos, "signed by another key: blocked");
            }
            {
                FaceIdService service(fixture.Options(""));
                service.Initialize();
                Check(service.SharedBlocked() && service.Database().error.find("no key") != std::string::npos, "a build with no key built in cannot check it: blocked");
            }
            {
                FaceIdService service(fixture.Options(trusted, fixture.root / "named.fdb"));
                service.Initialize();
                Check(service.SharedBlocked() && !service.Database().found && service.Database().named && service.Database().error.find("not there") != std::string::npos,
                      "a named database that is missing: blocked, not replaced by the other one");
            }
            {
                FaceIdService service(fixture.Options(trusted, fixture.DatabasePath()));
                service.Initialize();
                Check(service.SharedBlocked(), "the named path leads to the same file: still blocked");
            }
        }

        // the publisher: records into the draft, signs, the service uses the file at once; the Local mode on request
        {
            ServiceFixture fixture(directory, "publisher");
            FaceIdService::Options options = fixture.Options(trusted);
            options.signingKeyFile = directory / "svc-keys" / "publisher.key";
            FaceIdService service(options);
            service.Initialize();
            Check(service.Source() == FaceSource::Local && service.IsPublisher(), "a publisher with no database yet is in the Local mode");
            Check(service.PublisherKeyNote().find("built into the engine") != std::string::npos && service.PublisherKeyNote().find("NOT") == std::string::npos, "the key is the built-in one");
            std::string saveError;
            Check(!service.PublisherSave(fixture.DatabasePath(), false, "", &saveError), "an empty draft is not signed");
            Check(service.DraftAddFace("Artem", Embeddings(8, 0), &saveError), "a face into the draft: " + saveError);
            Check(!service.DraftAddFace("Artem", Embeddings(8, 10), &saveError), "a name once");
            Check(!service.DraftAddFace("Few", Embeddings(2, 10), &saveError), "two pictures are too few");
            Check(service.DraftAddFace("Friend", Embeddings(8, 30), &saveError) && service.DraftAddFace("Teacher", Embeddings(8, 60), &saveError) && service.DraftDirty(), "three faces");
            Check(!service.PublisherSave(fixture.DatabasePath(), true, "", &saveError), "the password option needs a password");
            Check(!service.PublisherSave(fixture.DatabasePath(), true, "short", &saveError), "and a long enough one");
            Check(service.PublisherSave(fixture.DatabasePath(), true, "publisher-pass", &saveError), "signed and saved: " + saveError);
            Check(fs::exists(fixture.DatabasePath()) && !service.DraftDirty(), "the file is there, the draft is clean");
            Check(service.Source() == FaceSource::Shared && service.Database().valid && service.Database().faces == 3, "the service uses the new database at once");
            Check(service.LoginByPassword("publisher-pass"), "with the password that was put in");
            Check(service.DraftRemoveFace(1) && service.Draft().faces.size() == 2 && service.DraftDirty(), "a face is removed from the draft");
            service.DraftDiscard();
            Check(service.Draft().faces.size() == 3 && !service.DraftDirty(), "discarding brings the signed database back");
            Check(service.DraftRemoveFace(2), "remove again");
            Check(service.PublisherSave(fixture.DatabasePath(), true, "", &saveError), "the password in the database is kept when the field is empty: " + saveError);
            Check(service.Database().faces == 2 && service.LoginByPassword("publisher-pass"), "two faces, the same password");
            Check(service.PublisherSave(fixture.DatabasePath(), false, "", &saveError) && !service.Database().hasPassword, "the password can be left out");

            // a second computer with only the file and the built-in key
            ServiceFixture second(directory, "publisher-copy");
            fs::create_directories(second.DatabasePath().parent_path());
            fs::copy_file(fixture.DatabasePath(), second.DatabasePath(), fs::copy_options::overwrite_existing);
            FaceIdService other(second.Options(trusted));
            other.Initialize();
            Check(other.Source() == FaceSource::Shared && other.Database().valid && other.ActiveProfiles().size() == 2 && !other.IsPublisher(), "the copied file works on another computer");

            // the Local mode on request of the publisher
            service.Settings().forceLocal = true;
            service.ReloadDatabase();
            Check(service.Source() == FaceSource::Local, "the publisher can switch to the Local mode");
            other.Settings().forceLocal = true;
            other.ReloadDatabase();
            Check(other.Source() == FaceSource::Shared, "a computer that is not a publisher cannot");
        }

        // a key that is not the built-in one: it does not make a publisher, and what it signs is rejected
        {
            ServiceFixture fixture(directory, "wrongkey");
            FaceIdService::Options options = fixture.Options(trusted);
            options.signingKeyFile = directory / "svc-keys" / "stranger.key";
            FaceIdService service(options);
            service.Initialize();
            Check(!service.IsPublisher() && service.PublisherKeyNote().find("NOT the one") != std::string::npos, "a key that is not the built-in one is not a publisher");
        }

        // no key is built in yet: the publisher's own machine trusts its own key (the work goes on before the engine is built again)
        {
            ServiceFixture fixture(directory, "bootstrap");
            FaceIdService::Options options = fixture.Options("");
            options.signingKeyFile = directory / "svc-keys" / "publisher.key";
            FaceIdService service(options);
            service.Initialize();
            Check(service.IsPublisher() && service.PublisherKeyNote().find("No key is built into the engine") != std::string::npos, "before the key is built in the machine with the key is the publisher");
            std::string saveError;
            Check(service.DraftAddFace("Artem", Embeddings(8, 0), &saveError) && service.PublisherSave(fixture.DatabasePath(), false, "", &saveError), "it signs: " + saveError);
            Check(service.Source() == FaceSource::Shared && service.Database().valid && !service.Database().signedByBuiltInKey, "and uses the database, with a note that no key is built in");
            FaceIdService nobody(fixture.Options(""));
            nobody.Initialize();
            Check(nobody.SharedBlocked(), "another machine with no built-in key rejects the same file");
        }
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

    // ---- faces from photos (F2): the NASA portraits of two men, in other years and other light (tools/faceid/fetch_models.ps1 -WithTestImage)

    FaceImage Blurred(const FaceImage& image, const int radius)
    {
        FaceImage result = image;
        for (int y = 0; y < image.height; ++y)
        {
            for (int x = 0; x < image.width; ++x)
            {
                for (int c = 0; c < 3; ++c)
                {
                    int sum = 0;
                    int count = 0;
                    for (int dy = -radius; dy <= radius; ++dy)
                    {
                        for (int dx = -radius; dx <= radius; ++dx)
                        {
                            const int xx = x + dx;
                            const int yy = y + dy;
                            if (xx >= 0 && yy >= 0 && xx < image.width && yy < image.height)
                            {
                                sum += image.rgb[(static_cast<std::size_t>(yy) * image.width + xx) * 3 + c];
                                ++count;
                            }
                        }
                    }
                    result.rgb[(static_cast<std::size_t>(y) * image.width + x) * 3 + c] = static_cast<std::uint8_t>(sum / count);
                }
            }
        }
        return result;
    }

    // Two pictures next to each other (the same height)
    FaceImage SideBySide(const FaceImage& left, const FaceImage& right)
    {
        FaceImage result;
        result.width = left.width + right.width;
        result.height = std::max(left.height, right.height);
        result.rgb.assign(static_cast<std::size_t>(result.width) * result.height * 3u, 0);
        for (int y = 0; y < left.height; ++y)
        {
            std::copy_n(left.rgb.begin() + static_cast<std::ptrdiff_t>(y) * left.width * 3, static_cast<std::ptrdiff_t>(left.width) * 3, result.rgb.begin() + static_cast<std::ptrdiff_t>(y) * result.width * 3);
        }
        for (int y = 0; y < right.height; ++y)
        {
            std::copy_n(right.rgb.begin() + static_cast<std::ptrdiff_t>(y) * right.width * 3, static_cast<std::ptrdiff_t>(right.width) * 3,
                        result.rgb.begin() + (static_cast<std::ptrdiff_t>(y) * result.width + left.width) * 3);
        }
        return result;
    }

    // 0: passed, 77: skipped (no ONNX Runtime, no models, no photos)
    int TestPhotos(const fs::path& directory)
    {
        if (!FaceModels::Available())
        {
            return 77;
        }
        const fs::path modelsDirectory = FindUpwards("assets/faceid/models");
        const fs::path photoDirectory = FindUpwards("assets/faceid/test/photos");
        if (modelsDirectory.empty() || photoDirectory.empty() || !fs::exists(photoDirectory / "armstrong1.jpg") || !fs::exists(photoDirectory / "aldrin1.jpg"))
        {
            std::cout << "faceid: SKIP the photos (assets/faceid/test/photos is not there: tools/faceid/fetch_models.ps1 -WithTestImage)\n";
            return 77;
        }
        FaceModels models;
        std::string error;
        Check(models.Load(modelsDirectory, error), "the models load for the photos: " + error);

        // what the files say when they are not pictures
        {
            FaceImage image;
            std::string reason;
            const fs::path text = directory / "not_a_picture.jpg";
            WriteBytes(text, std::vector<std::uint8_t>{'h', 'e', 'l', 'l', 'o'});
            Check(!LoadPhoto(text, image, reason) && reason.find("jpg or png") != std::string::npos, "a text file is not a photo: " + reason);
            Check(!LoadPhoto(directory / "missing.jpg", image, reason) && reason.find("cannot be read") != std::string::npos, "a missing file: " + reason);
            Check(LoadPhoto(photoDirectory / "armstrong1.jpg", image, reason) && image.width > 100, "a jpg is read");
        }

        // every portrait: what is found in it
        struct Entry
        {
            std::string name;
            char person;
            PhotoResult result;
        };
        std::vector<Entry> entries;
        for (const char person : {'a', 'b'})
        {
            const std::string prefix = person == 'a' ? "armstrong" : "aldrin";
            for (int i = 1; i <= 6; ++i)
            {
                const fs::path file = photoDirectory / (prefix + std::to_string(i) + ".jpg");
                FaceImage image;
                std::string reason;
                if (!fs::exists(file))
                {
                    continue;
                }
                Check(LoadPhoto(file, image, reason), "the photo " + file.filename().string() + " is read: " + reason);
                Entry entry{file.filename().string(), person, ExaminePhoto(models, image)};
                std::cout << "faceid: photo " << entry.name << " " << (entry.result.accepted ? "accepted" : "refused: " + entry.result.reason) << " width " << entry.result.faceWidth
                          << " sharpness " << entry.result.sharpness << " yaw " << entry.result.yaw << "\n";
                entries.push_back(std::move(entry));
            }
        }
        std::vector<const Entry*> armstrong;
        std::vector<const Entry*> aldrin;
        for (const Entry& entry : entries)
        {
            if (entry.result.accepted)
            {
                (entry.person == 'a' ? armstrong : aldrin).push_back(&entry);
            }
        }
        Check(armstrong.size() >= 3 && aldrin.size() >= 2, "enough of the portraits pass the checks to compare people");

        // the same man in other years is above the threshold, the other man is below it
        float lowestSame = 1.0f;
        float highestOther = -1.0f;
        for (const Entry& one : entries)
        {
            for (const Entry& two : entries)
            {
                if (&one >= &two || !one.result.accepted || !two.result.accepted)
                {
                    continue;
                }
                const float cosine = Cosine(one.result.embedding, two.result.embedding);
                std::cout << "faceid: " << one.name << " ~ " << two.name << " " << cosine << "\n";
                if (one.person == two.person)
                {
                    lowestSame = std::min(lowestSame, cosine);
                }
                else
                {
                    highestOther = std::max(highestOther, cosine);
                }
            }
        }
        std::cout << "faceid: photos: the lowest same-person similarity " << lowestSame << ", the highest between two people " << highestOther << "\n";
        Check(highestOther < kDefaultThreshold, "two different people are below the threshold on the photos (" + std::to_string(highestOther) + ")");
        Check(highestOther < kPhotoSamePerson, "two different people never look like one person to the photo check (" + std::to_string(highestOther) + ")");
        // every photo has a look-alike among the others of the same man, except the one in a helmet with a visor (the face behind glass)
        int lonely = 0;
        for (const Entry& one : entries)
        {
            float best = -1.0f;
            for (const Entry& two : entries)
            {
                if (&one != &two && one.person == two.person && one.result.accepted && two.result.accepted)
                {
                    best = std::max(best, Cosine(one.result.embedding, two.result.embedding));
                }
            }
            lonely += best < kPhotoSamePerson ? 1 : 0;
        }
        Check(lonely <= 1, "a photo of the same man is taken for another only when his face is behind a visor (" + std::to_string(lonely) + ")");

        // the whole way: a profile from photos of one man (the others are held out), the held-out photo of him is known, the photos of the other man are not
        {
            FaceProfile profile;
            profile.name = "FromPhotos";
            const Entry* heldOut = armstrong.back();
            for (const Entry* entry : armstrong)
            {
                if (entry != heldOut && profile.embeddings.size() < 4)
                {
                    profile.embeddings.push_back(entry->result.embedding);
                }
            }
            const std::vector<FaceProfile> profiles = {profile};
            const MatchResult same = BestMatch(profiles, heldOut->result.embedding);
            std::cout << "faceid: a profile of " << profile.embeddings.size() << " photos knows the held-out one with " << same.score << "\n";
            Check(same.score >= kDefaultThreshold, "a profile made of photos knows another photo of the man (" + std::to_string(same.score) + ")");
            for (const Entry* entry : aldrin)
            {
                const MatchResult other = BestMatch(profiles, entry->result.embedding);
                Check(other.score < kDefaultThreshold, "a profile made of photos does not know the other man (" + entry->name + " " + std::to_string(other.score) + ")");
            }
        }

        // the mirrored face is the same person
        for (const Entry* entry : armstrong)
        {
            const float mirrored = Cosine(entry->result.embedding, entry->result.mirrored);
            Check(mirrored > kDefaultThreshold + 0.1f, "the mirrored face of " + entry->name + " is the same person (" + std::to_string(mirrored) + ")");
        }

        // what a photo is refused for
        FaceImage portrait;
        std::string reason;
        Check(LoadPhoto(photoDirectory / "armstrong1.jpg", portrait, reason), "the portrait for the refusals");
        Check(ExaminePhoto(models, portrait).accepted, "the portrait itself is accepted");
        {
            FaceImage grey;
            grey.width = 640;
            grey.height = 480;
            grey.rgb.assign(640u * 480u * 3u, 120);
            const PhotoResult none = ExaminePhoto(models, grey);
            Check(!none.accepted && none.reason.find("no face") != std::string::npos, "a grey picture: no face (" + none.reason + ")");
        }
        {
            const PhotoResult small = ExaminePhoto(models, ShrinkImage(portrait, 4));
            Check(!small.accepted && (small.reason.find("too small") != std::string::npos || small.reason.find("no face") != std::string::npos), "a small picture is refused: " + small.reason);
        }
        {
            const PhotoResult blurred = ExaminePhoto(models, Blurred(portrait, 6));
            Check(!blurred.accepted, "a blurred picture is refused: " + blurred.reason);
        }
        {
            FaceImage other;
            Check(LoadPhoto(photoDirectory / "aldrin1.jpg", other, reason), "the second portrait");
            const PhotoResult two = ExaminePhoto(models, SideBySide(portrait, other));
            std::cout << "faceid: two portraits in one photo: " << two.reason << "\n";
            Check(!two.accepted && two.reason.find("faces found") != std::string::npos, "two faces in one photo are refused: " + two.reason);
        }
        {
            // a photo that a phone took sideways: the face is found after the turn, and it is the same face
            FaceImage sideways = portrait;
            sideways = RotateClockwise(sideways);
            const PhotoResult turned = ExaminePhoto(models, sideways);
            Check(turned.accepted, "a photo that lies on its side is turned and read: " + turned.reason);
            const PhotoResult upright = ExaminePhoto(models, portrait);
            Check(Cosine(turned.embedding, upright.embedding) > 0.9f, "the face of the turned photo is the same face (" + std::to_string(Cosine(turned.embedding, upright.embedding)) + ", width " + std::to_string(turned.faceWidth) + " against " + std::to_string(upright.faceWidth) + ")");
        }
        {
            // a big photo is shrunk before the search: the face is the same
            FaceImage big;
            big.width = portrait.width * 4;
            big.height = portrait.height * 4;
            big.rgb.resize(static_cast<std::size_t>(big.width) * big.height * 3u);
            for (int y = 0; y < big.height; ++y)
            {
                for (int x = 0; x < big.width; ++x)
                {
                    std::copy_n(portrait.rgb.begin() + (static_cast<std::ptrdiff_t>(y / 4) * portrait.width + x / 4) * 3, 3, big.rgb.begin() + (static_cast<std::ptrdiff_t>(y) * big.width + x) * 3);
                }
            }
            const PhotoResult large = ExaminePhoto(models, big);
            Check(large.accepted && large.faceWidth > 4.0f * kPhotoMinFaceWidth, "a photo four times larger is accepted: " + large.reason);
        }

        // the service: photos in, a profile out
        {
            ServiceFixture fixture(directory, "photos");
            FaceIdService::Options options = fixture.Options("");
            options.modelsDirectory = modelsDirectory;
            FaceIdService service(options);
            Check(service.Initialize() && service.Available(), "the service with the models");
            const auto file = [&](const std::string& name) { return photoDirectory / name; };

            Check(FaceIdService::CameraFacesAfter(0) == 8 && FaceIdService::CameraFacesAfter(3) == 5 && FaceIdService::CameraFacesAfter(7) == 1 &&
                      FaceIdService::CameraFacesAfter(9) == 1 && FaceIdService::CameraFacesAfter(10) == 0,
                  "how many faces the camera adds to the photos");

            std::vector<PhotoReport> reports = service.AddPhotos({file("armstrong1.jpg"), file("armstrong3.jpg"), file("armstrong4.jpg"), file("aldrin1.jpg"), file("armstrong1.jpg"), directory / "not_a_picture.jpg"});
            for (const PhotoReport& report : reports)
            {
                std::cout << "faceid: service: " << report.file << " " << (report.accepted ? "ok" : "refused") << ": " << report.message << "\n";
            }
            Check(reports.size() == 6, "a report for each file");
            Check(reports[0].accepted && reports[1].accepted && reports[2].accepted, "three photos of the man are accepted");
            Check(!reports[3].accepted && reports[3].message.find("does not look like") != std::string::npos, "a photo of another man is refused: " + reports[3].message);
            Check(!reports[4].accepted && reports[4].message.find("the same picture as armstrong1.jpg") != std::string::npos, "the same photo twice is refused: " + reports[4].message);
            Check(!reports[5].accepted && reports[5].message.find("jpg or png") != std::string::npos, "a file that is not a photo: " + reports[5].message);
            Check(service.PhotoCount() == 3 && FaceIdService::CameraFacesAfter(service.PhotoCount()) == 5, "three photos are in the list");

            std::vector<FaceEmbedding> embeddings;
            Check(service.TakePhotoFaces(embeddings, &error) && embeddings.size() == FaceProfileStore::kMinEmbeddings && service.PhotoCount() == 0,
                  "three photos make five faces (two of them mirrored) and the list is empty again");
            FaceProfile profile;
            profile.name = "Photos";
            profile.embeddings = embeddings;
            Check(service.Store().Add(std::move(profile), &error, "photo-password"), "a profile from photos is stored: " + error);
            Check(service.ActiveProfiles().size() == 1, "and is in use");
            FaceImage held;
            Check(LoadPhoto(file("armstrong5.jpg"), held, reason), "a photo that was not in the profile");
            const PhotoResult heldResult = ExaminePhoto(models, held);
            if (heldResult.accepted)
            {
                Check(BestMatch(service.ActiveProfiles(), heldResult.embedding).score >= kDefaultThreshold, "the profile from photos knows another photo of the man");
            }
            std::string empty;
            Check(!service.TakePhotoFaces(embeddings, &empty) && !empty.empty(), "no photos: nothing to take");

            // the limit of ten, and the clearing
            std::vector<fs::path> many(12, file("armstrong1.jpg"));
            many[1] = file("armstrong3.jpg");
            many[2] = file("armstrong4.jpg");
            many[3] = file("armstrong5.jpg");
            many[4] = file("armstrong6.jpg");
            many[5] = file("armstrong2.jpg");
            service.ClearPhotos();
            reports = service.AddPhotos(many);
            Check(service.PhotoCount() <= FaceProfileStore::kMaxEmbeddings, "no more than ten photos are kept");
            service.ClearPhotos();
            Check(service.PhotoCount() == 0, "the list is cleared");

            // the shared draft takes a face made of photos as well
            service.ClearPhotos();
            service.AddPhotos({file("aldrin1.jpg"), file("aldrin2.jpg"), file("aldrin4.jpg")});
            if (service.PhotoCount() >= 3)
            {
                Check(service.TakePhotoFaces(embeddings, &error) && service.DraftAddFace("Aldrin", embeddings, &error), "a face from photos goes into the draft of the shared database: " + error);
            }
        }
        // the batch decides who the person is: the biggest group wins, a photo behind a visor and a stranger are refused wherever they stand in the list
        {
            ServiceFixture fixture(directory, "photos-batch");
            FaceIdService::Options options = fixture.Options("");
            options.modelsDirectory = modelsDirectory;
            FaceIdService service(options);
            Check(service.Initialize(), "the service for the batch");
            const auto file = [&](const std::string& name) { return photoDirectory / name; };
            std::vector<PhotoReport> reports = service.AddPhotos({file("aldrin3.jpg"), file("aldrin1.jpg"), file("armstrong1.jpg"), file("aldrin2.jpg"), file("aldrin5.jpg")});
            Check(reports.size() == 5 && !reports[0].accepted && reports[1].accepted && !reports[2].accepted && reports[3].accepted && reports[4].accepted,
                  "the visor and the other man are refused, the three photos of the person stay (the first of the list is not the anchor)");
            Check(service.PhotoCount() == 3, "three photos are in the list");
            // a later batch is held to the people that are in the list already
            reports = service.AddPhotos({file("armstrong3.jpg"), file("aldrin4.jpg")});
            Check(!reports[0].accepted && reports[1].accepted, "a later photo must look like the earlier ones");
            service.ClearPhotos();
            // two people, one photo each: the first stays
            reports = service.AddPhotos({file("armstrong1.jpg"), file("aldrin1.jpg")});
            Check(reports[0].accepted && !reports[1].accepted, "of two different people the first is kept");
        }
        std::cout << "faceid: photos ok\n";
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
        TestHex();
        TestSigner(directory);
        TestDatabaseFile(directory);
        TestFindDatabase(directory);
        TestServiceModes(directory);
        std::cout << "faceid: logic, alignment, profiles, signatures and the shared database ok\n";

        int models = TestModels();
        if (models == 0)
        {
            models = TestPhotos(directory);
        }
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
