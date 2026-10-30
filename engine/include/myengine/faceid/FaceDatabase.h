// FaceDatabase.h
// The shared face database allowed_faces.fdb (docs/editor/faceid.md, "The shared database"): the faces that every copy of the engine lets in, and the signature of the
// publisher over them. The embeddings are NOT encrypted (the key would be in the exe: a show of protection); the file is protected against a change, not against reading.
//
//   u32 "MEFD"  u32 version  u32 bodyLength  body  u32 signatureLength  signature (ECDSA P-256 over everything before the signature: the header and the body)
//   body: u32 count; per face: u8 nameLength, name (UTF-8), u8 dateLength, date "YYYY-MM-DD", u8 embeddingCount, embeddings (128 float32 each);
//         u8 hasPassword [u32 iterations, u8 saltLength, salt, u8 hashLength, hash]; u8 createdLength, created "YYYY-MM-DD"

#pragma once

#include <myengine/faceid/FaceProfileStore.h>
#include <myengine/faceid/FaceTypes.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace myengine::faceid
{
    class FaceSigner;

    struct SharedFace
    {
        std::string name;                        // 1 to 32 bytes
        std::string date;                        // when it was recorded, YYYY-MM-DD
        std::vector<FaceEmbedding> embeddings;   // 5 to 10, normalised
    };

    struct FaceDatabase
    {
        static constexpr std::uint32_t kVersion = 1;
        static constexpr std::size_t kMaxFaces = 8;

        std::vector<SharedFace> faces;
        PasswordHash password;       // optional: the password of the publisher as a hash, the same for everybody (the camera may not work)
        std::string created;         // when it was signed, YYYY-MM-DD

        // the faces as the profiles that the recognition works with
        std::vector<FaceProfile> Profiles() const;
        // the same rules as the profile store: sizes, names, dates, finite numbers; false and the reason
        bool Validate(std::string* error = nullptr) const;
    };

    // The bytes that are signed and the signed file
    std::vector<std::uint8_t> SerializeDatabaseBody(const FaceDatabase& database);
    bool ParseDatabaseBody(const std::vector<std::uint8_t>& body, FaceDatabase& database, std::string* error = nullptr);
    // header + body + signature of the signer; false when the database is not valid or the signer has no key
    bool SignDatabase(const FaceDatabase& database, const FaceSigner& signer, std::vector<std::uint8_t>& file, std::string* error = nullptr);

    enum class DatabaseStatus
    {
        Ok,
        Unreadable,       // the file cannot be read
        Damaged,          // not a database, cut, wrong sizes
        NoTrustedKey,     // the engine has no key to check it with
        BadSignature      // a key is there, the signature does not fit: a changed file, or another publisher
    };

    struct DatabaseCheck
    {
        DatabaseStatus status = DatabaseStatus::Unreadable;
        std::string message;       // for the screen: what is wrong, in words
        int keyIndex = -1;         // which of the trusted keys fitted
        FaceDatabase database;     // only when status is Ok
    };

    // Checks the signature with each of the trusted keys (64 bytes each) and reads the faces only after that: nothing of a file that is not signed is used
    DatabaseCheck CheckDatabase(const std::vector<std::uint8_t>& file, const std::vector<std::vector<std::uint8_t>>& trustedKeys);
    DatabaseCheck LoadDatabase(const std::filesystem::path& file, const std::vector<std::vector<std::uint8_t>>& trustedKeys);
    bool SaveDatabaseFile(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes, std::string* error = nullptr);

    // Where the database is looked for: the file named by --faceid-db (MYENGINE_FACEID_DB), then <folder of the exe>/faceid/allowed_faces.fdb (the package), then
    // <folder of the exe>/assets/faceid/allowed_faces.fdb (the repository and the build). An empty path: there is none. A path that was named but is not there is
    // returned as it is, with `named` true: the screens say "the file you named is not there" instead of silently looking elsewhere.
    struct DatabaseLocation
    {
        std::filesystem::path path;
        bool named = false;
        bool exists = false;
    };
    constexpr char kDatabaseFileName[] = "allowed_faces.fdb";
    DatabaseLocation FindDatabase(const std::filesystem::path& named, const std::filesystem::path& executableDirectory);
    std::filesystem::path ExecutableDirectory();

    // Today as YYYY-MM-DD (UTC)
    std::string TodayText();
    // The public key built into the engine (FaceIdTrustedKey.h); empty when the publisher has not put theirs in yet
    std::vector<std::uint8_t> BuiltInPublicKey();
}
