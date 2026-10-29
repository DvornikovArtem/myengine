#include <myengine/faceid/FaceProfileStore.h>

#include <myengine/faceid/FaceMatcher.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

namespace myengine::faceid
{
    namespace
    {
        constexpr std::uint32_t kFileMagic = 0x5046454Du;   // "MEFP"
        constexpr std::uint32_t kFileVersion = 1;
        constexpr std::uint32_t kPayloadVersion = 1;

        // the extra secret of the application for DPAPI: another program of the same user does not read the file by accident
        const wchar_t* const kEntropyText = L"myengine-faceid-v1";

        class DpapiProtector final : public IDataProtector
        {
        public:
            bool Protect(const std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>& protectedBytes) override
            {
                DATA_BLOB in{static_cast<DWORD>(plain.size()), const_cast<BYTE*>(plain.data())};
                DATA_BLOB entropy{static_cast<DWORD>(std::wcslen(kEntropyText) * sizeof(wchar_t)), reinterpret_cast<BYTE*>(const_cast<wchar_t*>(kEntropyText))};
                DATA_BLOB out{};
                if (!CryptProtectData(&in, L"myengine Face ID", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
                {
                    return false;
                }
                protectedBytes.assign(out.pbData, out.pbData + out.cbData);
                LocalFree(out.pbData);
                return true;
            }

            bool Unprotect(const std::vector<std::uint8_t>& protectedBytes, std::vector<std::uint8_t>& plain) override
            {
                DATA_BLOB in{static_cast<DWORD>(protectedBytes.size()), const_cast<BYTE*>(protectedBytes.data())};
                DATA_BLOB entropy{static_cast<DWORD>(std::wcslen(kEntropyText) * sizeof(wchar_t)), reinterpret_cast<BYTE*>(const_cast<wchar_t*>(kEntropyText))};
                DATA_BLOB out{};
                if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
                {
                    return false;
                }
                plain.assign(out.pbData, out.pbData + out.cbData);
                SecureZeroMemory(out.pbData, out.cbData);
                LocalFree(out.pbData);
                return true;
            }
        };

        void Put32(std::vector<std::uint8_t>& out, const std::uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
            {
                out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
            }
        }

        struct Reader
        {
            const std::vector<std::uint8_t>& bytes;
            std::size_t position = 0;
            bool ok = true;

            bool Has(const std::size_t count)
            {
                if (!ok || position + count > bytes.size())
                {
                    ok = false;
                }
                return ok;
            }
            std::uint8_t U8() { return Has(1) ? bytes[position++] : 0; }
            std::uint32_t U32()
            {
                if (!Has(4))
                {
                    return 0;
                }
                std::uint32_t value = 0;
                for (int i = 0; i < 4; ++i)
                {
                    value |= static_cast<std::uint32_t>(bytes[position++]) << (8 * i);
                }
                return value;
            }
            void Read(void* target, const std::size_t count)
            {
                if (Has(count))
                {
                    std::memcpy(target, bytes.data() + position, count);
                    position += count;
                }
            }
        };

        void SetError(std::string* error, const std::string& text)
        {
            if (error != nullptr)
            {
                *error = text;
            }
        }
    }

    std::vector<std::uint8_t> RandomBytes(const std::size_t count)
    {
        std::vector<std::uint8_t> bytes(count);
        BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return bytes;
    }

    bool DerivePbkdf2(const std::string& password, const std::vector<std::uint8_t>& salt, const std::uint32_t iterations, std::vector<std::uint8_t>& out)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0)
        {
            return false;
        }
        out.assign(32, 0);
        const NTSTATUS status = BCryptDeriveKeyPBKDF2(algorithm, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())), static_cast<ULONG>(password.size()),
                                                      const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()), iterations, out.data(), static_cast<ULONG>(out.size()), 0);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return status >= 0;
    }

    bool ConstantTimeEqual(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        std::uint8_t difference = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
        }
        return difference == 0;
    }

    std::shared_ptr<IDataProtector> MakeDpapiProtector() { return std::make_shared<DpapiProtector>(); }

    bool HashPassword(const std::string& password, PasswordHash& out, const std::uint32_t iterations)
    {
        out.salt = RandomBytes(16);
        out.iterations = iterations;
        return DerivePbkdf2(password, out.salt, iterations, out.hash);
    }

    bool VerifyPassword(const std::string& password, const PasswordHash& stored)
    {
        if (!stored.Valid())
        {
            return false;
        }
        std::vector<std::uint8_t> hash;
        return DerivePbkdf2(password, stored.salt, stored.iterations, hash) && ConstantTimeEqual(hash, stored.hash);
    }

    FaceProfileStore::FaceProfileStore(std::filesystem::path file, std::shared_ptr<IDataProtector> protector) : file_(std::move(file)), protector_(std::move(protector)) {}

    std::filesystem::path FaceProfileStore::DefaultFile()
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
        std::filesystem::path base = (length > 0 && length < MAX_PATH) ? std::filesystem::path(buffer) : std::filesystem::temp_directory_path();
        return base / "myengine" / "faceid" / "profiles.bin";
    }

    std::vector<std::uint8_t> FaceProfileStore::Serialize() const
    {
        std::vector<std::uint8_t> out;
        Put32(out, kPayloadVersion);
        Put32(out, password_.iterations);
        out.push_back(static_cast<std::uint8_t>(password_.salt.size()));
        out.insert(out.end(), password_.salt.begin(), password_.salt.end());
        out.push_back(static_cast<std::uint8_t>(password_.hash.size()));
        out.insert(out.end(), password_.hash.begin(), password_.hash.end());
        Put32(out, static_cast<std::uint32_t>(profiles_.size()));
        for (const FaceProfile& profile : profiles_)
        {
            out.push_back(static_cast<std::uint8_t>(profile.name.size()));
            out.insert(out.end(), profile.name.begin(), profile.name.end());
            out.push_back(static_cast<std::uint8_t>(profile.embeddings.size()));
            for (const FaceEmbedding& embedding : profile.embeddings)
            {
                const auto* raw = reinterpret_cast<const std::uint8_t*>(embedding.data());
                out.insert(out.end(), raw, raw + sizeof(float) * kEmbeddingSize);
            }
        }
        return out;
    }

    bool FaceProfileStore::Deserialize(const std::vector<std::uint8_t>& bytes)
    {
        Reader r{bytes};
        if (r.U32() != kPayloadVersion)
        {
            return false;
        }
        PasswordHash password;
        password.iterations = r.U32();
        password.salt.resize(r.U8());
        r.Read(password.salt.data(), password.salt.size());
        password.hash.resize(r.U8());
        r.Read(password.hash.data(), password.hash.size());
        const std::uint32_t count = r.U32();
        if (!r.ok || count > kMaxProfiles)
        {
            return false;
        }
        std::vector<FaceProfile> profiles;
        for (std::uint32_t i = 0; i < count && r.ok; ++i)
        {
            FaceProfile profile;
            profile.name.resize(r.U8());
            r.Read(profile.name.data(), profile.name.size());
            const std::size_t embeddings = r.U8();
            if (embeddings > kMaxEmbeddings)
            {
                return false;
            }
            for (std::size_t e = 0; e < embeddings && r.ok; ++e)
            {
                FaceEmbedding embedding{};
                r.Read(embedding.data(), sizeof(float) * kEmbeddingSize);
                profile.embeddings.push_back(embedding);
            }
            profiles.push_back(std::move(profile));
        }
        if (!r.ok)
        {
            return false;
        }
        profiles_ = std::move(profiles);
        password_ = std::move(password);
        return true;
    }

    bool FaceProfileStore::Load(std::string* error)
    {
        profiles_.clear();
        password_ = {};
        std::error_code ec;
        if (!std::filesystem::exists(file_, ec))
        {
            return true;
        }
        std::ifstream stream(file_, std::ios::binary);
        std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        if (!stream && !stream.eof())
        {
            SetError(error, "the file of the profiles cannot be read");
            return false;
        }
        Reader r{file};
        if (r.U32() != kFileMagic || r.U32() != kFileVersion)
        {
            SetError(error, "the file of the profiles is not one of Face ID");
            return false;
        }
        const std::uint32_t length = r.U32();
        if (!r.ok || r.position + length > file.size())
        {
            SetError(error, "the file of the profiles is damaged");
            return false;
        }
        const std::vector<std::uint8_t> protectedBytes(file.begin() + static_cast<std::ptrdiff_t>(r.position), file.begin() + static_cast<std::ptrdiff_t>(r.position + length));
        std::vector<std::uint8_t> plain;
        if (protector_ == nullptr || !protector_->Unprotect(protectedBytes, plain))
        {
            SetError(error, "the profiles cannot be decrypted: they were saved by another user of Windows or on another computer");
            return false;
        }
        const bool ok = Deserialize(plain);
        std::fill(plain.begin(), plain.end(), std::uint8_t{0});
        if (!ok)
        {
            profiles_.clear();
            password_ = {};
            SetError(error, "the file of the profiles is damaged");
        }
        return ok;
    }

    bool FaceProfileStore::Save(std::string* error) const
    {
        if (protector_ == nullptr)
        {
            SetError(error, "no protection of the data");
            return false;
        }
        if (file_.empty())
        {
            SetError(error, "no file for the profiles");
            return false;
        }
        std::vector<std::uint8_t> plain = Serialize();
        std::vector<std::uint8_t> protectedBytes;
        const bool protectedOk = protector_->Protect(plain, protectedBytes);
        std::fill(plain.begin(), plain.end(), std::uint8_t{0});
        if (!protectedOk)
        {
            SetError(error, "Windows could not encrypt the profiles");
            return false;
        }
        std::vector<std::uint8_t> file;
        Put32(file, kFileMagic);
        Put32(file, kFileVersion);
        Put32(file, static_cast<std::uint32_t>(protectedBytes.size()));
        file.insert(file.end(), protectedBytes.begin(), protectedBytes.end());
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec);
        const std::filesystem::path temporary = file_.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
            if (!stream)
            {
                SetError(error, "the file of the profiles cannot be written");
                return false;
            }
        }
        std::filesystem::rename(temporary, file_, ec);
        if (ec)
        {
            std::filesystem::remove(file_, ec);
            std::filesystem::rename(temporary, file_, ec);
        }
        if (ec)
        {
            SetError(error, "the file of the profiles cannot be replaced: " + ec.message());
            return false;
        }
        return true;
    }

    bool FaceProfileStore::Add(FaceProfile profile, std::string* error, const std::string& newPassword)
    {
        if (Full())
        {
            SetError(error, "there are three profiles already: delete one first");
            return false;
        }
        if (profile.name.empty() || profile.name.size() > kMaxNameBytes)
        {
            SetError(error, "the name must be 1 to 32 bytes");
            return false;
        }
        for (const FaceProfile& other : profiles_)
        {
            if (other.name == profile.name)
            {
                SetError(error, "there is a profile with this name");
                return false;
            }
        }
        if (profile.embeddings.size() < kMinEmbeddings || profile.embeddings.size() > kMaxEmbeddings)
        {
            SetError(error, "a profile needs 5 to 10 faces");
            return false;
        }
        for (FaceEmbedding& embedding : profile.embeddings)
        {
            Normalize(embedding);
        }
        const PasswordHash oldPassword = password_;
        if (!HasPassword())
        {
            if (newPassword.size() < 6)
            {
                SetError(error, "the first profile needs a password of at least 6 characters (for the days when the camera does not work)");
                return false;
            }
            if (!HashPassword(newPassword, password_))
            {
                SetError(error, "the password could not be hashed");
                return false;
            }
        }
        profiles_.push_back(std::move(profile));
        if (!Save(error))
        {
            profiles_.pop_back();
            password_ = oldPassword;
            return false;
        }
        return true;
    }

    bool FaceProfileStore::EraseAll(std::string* error)
    {
        profiles_.clear();
        password_ = {};
        std::error_code ec;
        if (!std::filesystem::exists(file_, ec))
        {
            return true;
        }
        {
            // the content is overwritten before the file goes
            const auto size = std::filesystem::file_size(file_, ec);
            if (!ec && size > 0)
            {
                std::ofstream stream(file_, std::ios::binary | std::ios::in | std::ios::out);
                const std::vector<char> zeros(static_cast<std::size_t>(size), 0);
                stream.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
            }
        }
        std::filesystem::remove(file_, ec);
        if (ec)
        {
            SetError(error, "the file of the profiles cannot be deleted: " + ec.message());
            return false;
        }
        return true;
    }

    bool FaceProfileStore::Remove(const std::size_t index, std::string* error)
    {
        if (index >= profiles_.size())
        {
            SetError(error, "there is no such profile");
            return false;
        }
        if (profiles_.size() == 1)
        {
            return EraseAll(error);
        }
        const FaceProfile removed = profiles_[index];
        profiles_.erase(profiles_.begin() + static_cast<std::ptrdiff_t>(index));
        if (!Save(error))
        {
            profiles_.insert(profiles_.begin() + static_cast<std::ptrdiff_t>(index), removed);
            return false;
        }
        return true;
    }

    bool FaceProfileStore::ChangePassword(const std::string& password, std::string* error)
    {
        if (password.size() < 6)
        {
            SetError(error, "the password needs at least 6 characters");
            return false;
        }
        const PasswordHash old = password_;
        if (!HashPassword(password, password_))
        {
            password_ = old;
            SetError(error, "the password could not be hashed");
            return false;
        }
        if (!Save(error))
        {
            password_ = old;
            return false;
        }
        return true;
    }
}
