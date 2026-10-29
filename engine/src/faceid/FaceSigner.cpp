#include <myengine/faceid/FaceSigner.h>

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
#include <shlobj.h>

namespace myengine::faceid
{
    namespace
    {
        constexpr std::uint32_t kKeyFileMagic = 0x4B53454Du;      // "MESK": the key of this machine under DPAPI
        constexpr std::uint32_t kBackupMagic = 0x584B454Du;       // "MEKX": the backup under a password
        constexpr std::uint32_t kFileVersion = 1;
        constexpr std::size_t kPrivateBlobSize = sizeof(BCRYPT_ECCKEY_BLOB) + 3 * 32;
        constexpr std::size_t kPublicBlobSize = sizeof(BCRYPT_ECCKEY_BLOB) + 2 * 32;
        constexpr std::uint32_t kBackupIterations = 200000;
        constexpr std::size_t kBackupSaltSize = 16;
        constexpr std::size_t kBackupNonceSize = 12;
        constexpr std::size_t kBackupTagSize = 16;
        constexpr std::size_t kMinBackupPassword = 8;

        void SetError(std::string* error, const std::string& text)
        {
            if (error != nullptr)
            {
                *error = text;
            }
        }

        void Put32(std::vector<std::uint8_t>& out, const std::uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
            {
                out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
            }
        }

        std::uint32_t Get32(const std::vector<std::uint8_t>& bytes, const std::size_t at)
        {
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i)
            {
                value |= static_cast<std::uint32_t>(bytes[at + static_cast<std::size_t>(i)]) << (8 * i);
            }
            return value;
        }

        bool ReadFile(const std::filesystem::path& file, std::vector<std::uint8_t>& bytes)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                return false;
            }
            bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            return true;
        }

        // The file appears whole or not at all: written beside, then renamed
        bool WriteFileAtomic(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes)
        {
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            const std::filesystem::path temporary = file.wstring() + L".tmp";
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!stream)
                {
                    return false;
                }
            }
            std::filesystem::rename(temporary, file, ec);
            if (ec)
            {
                std::filesystem::remove(file, ec);
                ec.clear();
                std::filesystem::rename(temporary, file, ec);
            }
            return !ec;
        }

        struct Provider
        {
            BCRYPT_ALG_HANDLE handle = nullptr;
            explicit Provider(LPCWSTR algorithm, const ULONG flags = 0)
            {
                if (BCryptOpenAlgorithmProvider(&handle, algorithm, nullptr, flags) < 0)
                {
                    handle = nullptr;
                }
            }
            ~Provider()
            {
                if (handle != nullptr)
                {
                    BCryptCloseAlgorithmProvider(handle, 0);
                }
            }
            Provider(const Provider&) = delete;
            Provider& operator=(const Provider&) = delete;
        };

        struct KeyHandle
        {
            BCRYPT_KEY_HANDLE handle = nullptr;
            ~KeyHandle()
            {
                if (handle != nullptr)
                {
                    BCryptDestroyKey(handle);
                }
            }
        };

        bool Sha256(const std::vector<std::uint8_t>& message, std::vector<std::uint8_t>& hash)
        {
            hash.assign(32, 0);
            Provider provider(BCRYPT_SHA256_ALGORITHM);
            return provider.handle != nullptr &&
                   BCryptHash(provider.handle, nullptr, 0, const_cast<PUCHAR>(message.data()), static_cast<ULONG>(message.size()), hash.data(), static_cast<ULONG>(hash.size())) >= 0;
        }

        std::vector<std::uint8_t> PublicBlob(const std::vector<std::uint8_t>& publicKey)
        {
            std::vector<std::uint8_t> blob(kPublicBlobSize, 0);
            BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
            std::memcpy(blob.data(), &header, sizeof(header));
            std::memcpy(blob.data() + sizeof(header), publicKey.data(), kPublicKeySize);
            return blob;
        }

        // The key pair that a private blob holds must be a real point of the curve: CNG checks it on the import
        bool ImportPrivate(const Provider& provider, const std::vector<std::uint8_t>& blob, KeyHandle& key)
        {
            return provider.handle != nullptr &&
                   BCryptImportKeyPair(provider.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key.handle, const_cast<PUCHAR>(blob.data()), static_cast<ULONG>(blob.size()), 0) >= 0;
        }

        bool AesGcm(const bool encrypt, const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& nonce, const std::vector<std::uint8_t>& aad,
                    const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& output, std::vector<std::uint8_t>& tag)
        {
            Provider provider(BCRYPT_AES_ALGORITHM);
            if (provider.handle == nullptr)
            {
                return false;
            }
            if (BCryptSetProperty(provider.handle, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                  static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0) < 0)
            {
                return false;
            }
            KeyHandle aesKey;
            if (BCryptGenerateSymmetricKey(provider.handle, &aesKey.handle, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) < 0)
            {
                return false;
            }
            BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
            BCRYPT_INIT_AUTH_MODE_INFO(info);
            info.pbNonce = const_cast<PUCHAR>(nonce.data());
            info.cbNonce = static_cast<ULONG>(nonce.size());
            info.pbAuthData = const_cast<PUCHAR>(aad.data());
            info.cbAuthData = static_cast<ULONG>(aad.size());
            info.pbTag = tag.data();
            info.cbTag = static_cast<ULONG>(tag.size());
            output.assign(input.size(), 0);
            ULONG done = 0;
            const NTSTATUS status = encrypt ? BCryptEncrypt(aesKey.handle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &info, nullptr, 0, output.data(),
                                                            static_cast<ULONG>(output.size()), &done, 0)
                                            : BCryptDecrypt(aesKey.handle, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), &info, nullptr, 0, output.data(),
                                                            static_cast<ULONG>(output.size()), &done, 0);
            return status >= 0 && done == input.size();
        }
    }

    bool VerifySignature(const std::vector<std::uint8_t>& publicKey, const std::vector<std::uint8_t>& message, const std::vector<std::uint8_t>& signature)
    {
        if (publicKey.size() != kPublicKeySize || signature.size() != kSignatureSize)
        {
            return false;
        }
        std::vector<std::uint8_t> hash;
        if (!Sha256(message, hash))
        {
            return false;
        }
        Provider provider(BCRYPT_ECDSA_P256_ALGORITHM);
        if (provider.handle == nullptr)
        {
            return false;
        }
        std::vector<std::uint8_t> blob = PublicBlob(publicKey);
        KeyHandle key;
        if (BCryptImportKeyPair(provider.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.handle, blob.data(), static_cast<ULONG>(blob.size()), 0) < 0)
        {
            return false;   // the point is not on the curve, or the key is garbage
        }
        return BCryptVerifySignature(key.handle, nullptr, hash.data(), static_cast<ULONG>(hash.size()), const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0) >= 0;
    }

    std::string ToHex(const std::vector<std::uint8_t>& bytes)
    {
        static const char digits[] = "0123456789abcdef";
        std::string text;
        for (const std::uint8_t byte : bytes)
        {
            text.push_back(digits[byte >> 4]);
            text.push_back(digits[byte & 15]);
        }
        return text;
    }

    bool FromHex(const std::string& text, std::vector<std::uint8_t>& bytes)
    {
        if (text.empty() || text.size() % 2 != 0)
        {
            return false;
        }
        const auto value = [](const char c) -> int
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        };
        std::vector<std::uint8_t> result;
        for (std::size_t i = 0; i < text.size(); i += 2)
        {
            const int high = value(text[i]);
            const int low = value(text[i + 1]);
            if (high < 0 || low < 0)
            {
                return false;
            }
            result.push_back(static_cast<std::uint8_t>(high * 16 + low));
        }
        bytes = std::move(result);
        return true;
    }

    FaceSigner::FaceSigner(std::filesystem::path file, std::shared_ptr<IDataProtector> protector) : file_(std::move(file)), protector_(std::move(protector)) {}

    std::filesystem::path FaceSigner::DefaultFile() { return FaceProfileStore::DefaultFile().parent_path() / "signing.key"; }

    bool FaceSigner::SetFromBlob(const std::vector<std::uint8_t>& blob, std::string* error)
    {
        if (blob.size() != kPrivateBlobSize)
        {
            SetError(error, "the key has a wrong size");
            return false;
        }
        BCRYPT_ECCKEY_BLOB header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        if (header.dwMagic != BCRYPT_ECDSA_PRIVATE_P256_MAGIC || header.cbKey != 32)
        {
            SetError(error, "this is not a key of ECDSA P-256");
            return false;
        }
        Provider provider(BCRYPT_ECDSA_P256_ALGORITHM);
        KeyHandle key;
        if (!ImportPrivate(provider, blob, key))
        {
            SetError(error, "the key is not a valid key of ECDSA P-256");
            return false;
        }
        privateBlob_ = blob;
        publicKey_.assign(blob.begin() + sizeof(header), blob.begin() + sizeof(header) + kPublicKeySize);
        return true;
    }

    bool FaceSigner::Load(std::string* error)
    {
        privateBlob_.clear();
        publicKey_.clear();
        std::error_code ec;
        if (!std::filesystem::exists(file_, ec))
        {
            return true;
        }
        std::vector<std::uint8_t> file;
        if (!ReadFile(file_, file) || file.size() < 12 || Get32(file, 0) != kKeyFileMagic || Get32(file, 4) != kFileVersion || Get32(file, 8) != file.size() - 12)
        {
            SetError(error, "the file of the publisher key is damaged: " + file_.string());
            return false;
        }
        std::vector<std::uint8_t> sealed(file.begin() + 12, file.end());
        std::vector<std::uint8_t> blob;
        if (protector_ == nullptr || !protector_->Unprotect(sealed, blob))
        {
            SetError(error, "the publisher key cannot be opened: it was made under another Windows account (import it from the backup)");
            return false;
        }
        const bool ok = SetFromBlob(blob, error);
        std::fill(blob.begin(), blob.end(), std::uint8_t{0});
        return ok;
    }

    bool FaceSigner::Store(std::string* error) const
    {
        std::vector<std::uint8_t> sealed;
        if (protector_ == nullptr || !protector_->Protect(privateBlob_, sealed))
        {
            SetError(error, "Windows could not encrypt the key");
            return false;
        }
        std::vector<std::uint8_t> file;
        Put32(file, kKeyFileMagic);
        Put32(file, kFileVersion);
        Put32(file, static_cast<std::uint32_t>(sealed.size()));
        file.insert(file.end(), sealed.begin(), sealed.end());
        if (!WriteFileAtomic(file_, file))
        {
            SetError(error, "the key file could not be written: " + file_.string());
            return false;
        }
        return true;
    }

    bool FaceSigner::Generate(std::string* error)
    {
        if (HasKey())
        {
            SetError(error, "there is a publisher key already; a new one would make everything signed with the old one useless");
            return false;
        }
        std::error_code ec;
        if (std::filesystem::exists(file_, ec))
        {
            SetError(error, "a key file is there but cannot be read; move it away first: " + file_.string());
            return false;
        }
        Provider provider(BCRYPT_ECDSA_P256_ALGORITHM);
        KeyHandle key;
        if (provider.handle == nullptr || BCryptGenerateKeyPair(provider.handle, &key.handle, 256, 0) < 0 || BCryptFinalizeKeyPair(key.handle, 0) < 0)
        {
            SetError(error, "Windows could not make a key");
            return false;
        }
        ULONG size = 0;
        std::vector<std::uint8_t> blob(kPrivateBlobSize + 16, 0);
        if (BCryptExportKey(key.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), static_cast<ULONG>(blob.size()), &size, 0) < 0)
        {
            SetError(error, "Windows could not export the key");
            return false;
        }
        blob.resize(size);
        if (!SetFromBlob(blob, error))
        {
            return false;
        }
        std::fill(blob.begin(), blob.end(), std::uint8_t{0});
        if (!Store(error))
        {
            privateBlob_.clear();
            publicKey_.clear();
            return false;
        }
        return true;
    }

    bool FaceSigner::Sign(const std::vector<std::uint8_t>& message, std::vector<std::uint8_t>& signature, std::string* error) const
    {
        if (!HasKey())
        {
            SetError(error, "there is no publisher key on this computer");
            return false;
        }
        std::vector<std::uint8_t> hash;
        Provider provider(BCRYPT_ECDSA_P256_ALGORITHM);
        KeyHandle key;
        if (!Sha256(message, hash) || !ImportPrivate(provider, privateBlob_, key))
        {
            SetError(error, "Windows could not use the key");
            return false;
        }
        signature.assign(kSignatureSize, 0);
        ULONG size = 0;
        if (BCryptSignHash(key.handle, nullptr, hash.data(), static_cast<ULONG>(hash.size()), signature.data(), static_cast<ULONG>(signature.size()), &size, 0) < 0 || size != kSignatureSize)
        {
            SetError(error, "Windows could not sign");
            return false;
        }
        return true;
    }

    bool FaceSigner::Export(const std::filesystem::path& file, const std::string& password, std::string* error) const
    {
        if (!HasKey())
        {
            SetError(error, "there is no publisher key to export");
            return false;
        }
        if (password.size() < kMinBackupPassword)
        {
            SetError(error, "the password of the backup needs 8 characters or more");
            return false;
        }
        std::error_code ec;
        if (std::filesystem::exists(file, ec))
        {
            SetError(error, "the file exists already; the backup does not overwrite files: " + file.string());
            return false;
        }
        const std::vector<std::uint8_t> salt = RandomBytes(kBackupSaltSize);
        const std::vector<std::uint8_t> nonce = RandomBytes(kBackupNonceSize);
        std::vector<std::uint8_t> aesKey;
        if (!DerivePbkdf2(password, salt, kBackupIterations, aesKey))
        {
            SetError(error, "Windows could not derive the key of the backup");
            return false;
        }
        std::vector<std::uint8_t> header;
        Put32(header, kBackupMagic);
        Put32(header, kFileVersion);
        Put32(header, kBackupIterations);
        header.insert(header.end(), salt.begin(), salt.end());
        header.insert(header.end(), nonce.begin(), nonce.end());
        std::vector<std::uint8_t> cipher;
        std::vector<std::uint8_t> tag(kBackupTagSize, 0);
        const bool ok = AesGcm(true, aesKey, nonce, header, privateBlob_, cipher, tag);
        std::fill(aesKey.begin(), aesKey.end(), std::uint8_t{0});
        if (!ok)
        {
            SetError(error, "Windows could not encrypt the backup");
            return false;
        }
        std::vector<std::uint8_t> bytes = header;
        bytes.insert(bytes.end(), cipher.begin(), cipher.end());
        bytes.insert(bytes.end(), tag.begin(), tag.end());
        if (!WriteFileAtomic(file, bytes))
        {
            SetError(error, "the backup could not be written: " + file.string());
            return false;
        }
        return true;
    }

    bool FaceSigner::Import(const std::filesystem::path& file, const std::string& password, std::string* error)
    {
        if (HasKey())
        {
            SetError(error, "there is a publisher key already: delete signing.key first if you really want to replace it");
            return false;
        }
        std::error_code ec;
        if (std::filesystem::exists(file_, ec))
        {
            SetError(error, "a key file is there but cannot be read; move it away first: " + file_.string());
            return false;
        }
        std::vector<std::uint8_t> bytes;
        const std::size_t headerSize = 12 + kBackupSaltSize + kBackupNonceSize;
        if (!ReadFile(file, bytes))
        {
            SetError(error, "the backup cannot be read: " + file.string());
            return false;
        }
        if (bytes.size() != headerSize + kPrivateBlobSize + kBackupTagSize || Get32(bytes, 0) != kBackupMagic || Get32(bytes, 4) != kFileVersion)
        {
            SetError(error, "this is not a backup of the publisher key");
            return false;
        }
        const std::uint32_t iterations = Get32(bytes, 8);
        if (iterations < 1000 || iterations > 5000000)
        {
            SetError(error, "this is not a backup of the publisher key");
            return false;
        }
        const std::vector<std::uint8_t> header(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(headerSize));
        const std::vector<std::uint8_t> salt(bytes.begin() + 12, bytes.begin() + 12 + kBackupSaltSize);
        const std::vector<std::uint8_t> nonce(bytes.begin() + 12 + kBackupSaltSize, bytes.begin() + static_cast<std::ptrdiff_t>(headerSize));
        const std::vector<std::uint8_t> cipher(bytes.begin() + static_cast<std::ptrdiff_t>(headerSize), bytes.begin() + static_cast<std::ptrdiff_t>(headerSize + kPrivateBlobSize));
        std::vector<std::uint8_t> tag(bytes.end() - static_cast<std::ptrdiff_t>(kBackupTagSize), bytes.end());
        std::vector<std::uint8_t> aesKey;
        std::vector<std::uint8_t> blob;
        const bool derived = DerivePbkdf2(password, salt, iterations, aesKey);
        const bool opened = derived && AesGcm(false, aesKey, nonce, header, cipher, blob, tag);
        std::fill(aesKey.begin(), aesKey.end(), std::uint8_t{0});
        if (!opened)
        {
            SetError(error, "the password is wrong or the backup was changed");
            return false;
        }
        if (!SetFromBlob(blob, error))
        {
            std::fill(blob.begin(), blob.end(), std::uint8_t{0});
            return false;
        }
        std::fill(blob.begin(), blob.end(), std::uint8_t{0});
        if (!Store(error))
        {
            privateBlob_.clear();
            publicKey_.clear();
            return false;
        }
        return true;
    }
}
