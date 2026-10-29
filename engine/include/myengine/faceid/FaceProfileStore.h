// FaceProfileStore.h
// The profiles of Face ID: at most three (a name and 5 to 10 embeddings each) and the hash of the local password, in one file under the protection of Windows (DPAPI,
// the key of the current user). Neither photos nor embeddings go anywhere else: not to the repository, not to the logs, not to a package (docs/editor/faceid.md).

#pragma once

#include <myengine/faceid/FaceTypes.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace myengine::faceid
{
    struct FaceProfile
    {
        std::string name;
        std::vector<FaceEmbedding> embeddings;   // normalised
    };

    // Protects bytes for the current user of the machine; the tests put a simple one of their own in its place
    class IDataProtector
    {
    public:
        virtual ~IDataProtector() = default;
        virtual bool Protect(const std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>& protectedBytes) = 0;
        virtual bool Unprotect(const std::vector<std::uint8_t>& protectedBytes, std::vector<std::uint8_t>& plain) = 0;
    };
    // CryptProtectData / CryptUnprotectData (DPAPI) with a fixed extra secret of the application
    std::shared_ptr<IDataProtector> MakeDpapiProtector();

    // The helpers of the cryptography of Windows (BCrypt) that the signer of the shared database uses too
    std::vector<std::uint8_t> RandomBytes(std::size_t count);
    // PBKDF2-HMAC-SHA256, 32 bytes
    bool DerivePbkdf2(const std::string& password, const std::vector<std::uint8_t>& salt, std::uint32_t iterations, std::vector<std::uint8_t>& out);
    bool ConstantTimeEqual(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b);

    // PBKDF2-HMAC-SHA256 (BCrypt of Windows), a random salt of 16 bytes
    struct PasswordHash
    {
        std::vector<std::uint8_t> salt;
        std::vector<std::uint8_t> hash;
        std::uint32_t iterations = 0;
        bool Valid() const { return !salt.empty() && !hash.empty() && iterations > 0; }
    };
    bool HashPassword(const std::string& password, PasswordHash& out, std::uint32_t iterations = 120000);
    bool VerifyPassword(const std::string& password, const PasswordHash& stored);

    class FaceProfileStore
    {
    public:
        static constexpr std::size_t kMaxProfiles = 3;
        static constexpr std::size_t kMinEmbeddings = 5;
        static constexpr std::size_t kMaxEmbeddings = 10;
        static constexpr std::size_t kMaxNameBytes = 32;

        FaceProfileStore(std::filesystem::path file, std::shared_ptr<IDataProtector> protector);

        // %LOCALAPPDATA%\myengine\faceid\profiles.bin (the folder is not created here)
        static std::filesystem::path DefaultFile();

        // A missing file is an empty store; a file that cannot be unprotected or is damaged is an error, and the store stays empty (and is not overwritten until Add)
        bool Load(std::string* error = nullptr);
        bool Save(std::string* error = nullptr) const;

        const std::vector<FaceProfile>& Profiles() const { return profiles_; }
        bool Full() const { return profiles_.size() >= kMaxProfiles; }

        // The limits: three profiles, a name once, 5 to 10 embeddings. The first profile comes with the password (at least 6 characters).
        bool Add(FaceProfile profile, std::string* error = nullptr, const std::string& newPassword = std::string());
        // Erases the profile from the file; the last one takes the file with it (overwritten with zeros, then deleted), the password too
        bool Remove(std::size_t index, std::string* error = nullptr);
        // Erases the whole file
        bool EraseAll(std::string* error = nullptr);

        bool HasPassword() const { return password_.Valid(); }
        bool CheckPassword(const std::string& password) const { return VerifyPassword(password, password_); }
        bool ChangePassword(const std::string& password, std::string* error = nullptr);

        const std::filesystem::path& File() const { return file_; }

        // The bytes of the file before the protection (for the tests)
        std::vector<std::uint8_t> Serialize() const;
        bool Deserialize(const std::vector<std::uint8_t>& bytes);

    private:
        std::filesystem::path file_;
        std::shared_ptr<IDataProtector> protector_;
        std::vector<FaceProfile> profiles_;
        PasswordHash password_;
    };
}
