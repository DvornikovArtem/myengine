// FaceSigner.h
// The signatures of the shared face database (docs/editor/faceid.md, "The shared database"): ECDSA P-256 with SHA-256 on the cryptography of Windows (CNG, BCrypt), no
// library of a third party. The public key (64 bytes, X then Y) is built into the engine; the private key lives only on the machine of the publisher, in
// %LOCALAPPDATA%\myengine\faceid\signing.key under DPAPI, and can be exported to a file under a password (a backup) and imported back.

#pragma once

#include <myengine/faceid/FaceProfileStore.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace myengine::faceid
{
    constexpr std::size_t kPublicKeySize = 64;   // X and Y of the point, 32 bytes each
    constexpr std::size_t kSignatureSize = 64;   // r and s, 32 bytes each

    // true when `signature` is the ECDSA P-256 / SHA-256 signature of `message` made with the private key of `publicKey`
    bool VerifySignature(const std::vector<std::uint8_t>& publicKey, const std::vector<std::uint8_t>& message, const std::vector<std::uint8_t>& signature);

    // 128 hex digits <-> 64 bytes (spaces and 0x are not accepted; the engine writes its key as one string)
    std::string ToHex(const std::vector<std::uint8_t>& bytes);
    bool FromHex(const std::string& text, std::vector<std::uint8_t>& bytes);

    class FaceSigner
    {
    public:
        FaceSigner(std::filesystem::path file, std::shared_ptr<IDataProtector> protector);

        // %LOCALAPPDATA%\myengine\faceid\signing.key
        static std::filesystem::path DefaultFile();

        // Reads the key if the file is there. A missing file is not an error (no key); a file that cannot be unprotected or is damaged is, and `error` says so.
        bool Load(std::string* error = nullptr);
        bool HasKey() const { return !privateBlob_.empty(); }
        // 64 bytes X || Y; empty without a key
        const std::vector<std::uint8_t>& PublicKey() const { return publicKey_; }

        // A new key pair. Refused when a key exists: the old one would be lost together with everything signed with it (delete signing.key by hand to start again).
        bool Generate(std::string* error = nullptr);
        // 64 bytes r || s
        bool Sign(const std::vector<std::uint8_t>& message, std::vector<std::uint8_t>& signature, std::string* error = nullptr) const;

        // The backup of the key: the private key encrypted with AES-256-GCM under a key from the password (PBKDF2-HMAC-SHA256, 200 000 rounds, a random salt).
        // The password is at least 8 characters. The file is not overwritten.
        bool Export(const std::filesystem::path& file, const std::string& password, std::string* error = nullptr) const;
        // The key from a backup. A wrong password and a changed file are the same error (the tag of GCM does not match). An existing key is not replaced.
        bool Import(const std::filesystem::path& file, const std::string& password, std::string* error = nullptr);

        const std::filesystem::path& File() const { return file_; }

    private:
        bool SetFromBlob(const std::vector<std::uint8_t>& blob, std::string* error);
        bool Store(std::string* error) const;

        std::filesystem::path file_;
        std::shared_ptr<IDataProtector> protector_;
        std::vector<std::uint8_t> privateBlob_;   // BCRYPT_ECCPRIVATE_BLOB of P-256: 8 bytes of the header, X, Y, d
        std::vector<std::uint8_t> publicKey_;
    };
}
