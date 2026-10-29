#include <myengine/faceid/FaceDatabase.h>

#include <myengine/faceid/FaceIdTrustedKey.h>
#include <myengine/faceid/FaceMatcher.h>
#include <myengine/faceid/FaceSigner.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <system_error>

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
        constexpr std::uint32_t kMagic = 0x4446454Du;   // "MEFD"
        constexpr std::size_t kHeaderSize = 12;
        constexpr std::uint32_t kMinIterations = 1000;
        constexpr std::uint32_t kMaxIterations = 5000000;

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

        void PutText(std::vector<std::uint8_t>& out, const std::string& text)
        {
            out.push_back(static_cast<std::uint8_t>(text.size()));
            out.insert(out.end(), text.begin(), text.end());
        }

        // every read is checked against the end: a cut or a lying file never reads outside
        struct Reader
        {
            const std::vector<std::uint8_t>& bytes;
            std::size_t position = 0;
            bool ok = true;

            bool Has(const std::size_t count)
            {
                if (!ok || count > bytes.size() - position)
                {
                    ok = false;
                }
                return ok;
            }
            std::uint8_t U8() { return Has(1) ? bytes[position++] : std::uint8_t{0}; }
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
            std::string Text()
            {
                const std::size_t length = U8();
                if (!Has(length))
                {
                    return {};
                }
                std::string text(reinterpret_cast<const char*>(bytes.data() + position), length);
                position += length;
                return text;
            }
            std::vector<std::uint8_t> Bytes(const std::size_t count)
            {
                if (!Has(count))
                {
                    return {};
                }
                std::vector<std::uint8_t> result(bytes.begin() + static_cast<std::ptrdiff_t>(position), bytes.begin() + static_cast<std::ptrdiff_t>(position + count));
                position += count;
                return result;
            }
        };

        bool IsDate(const std::string& text)
        {
            if (text.size() != 10)
            {
                return false;
            }
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const bool dash = i == 4 || i == 7;
                if (dash ? text[i] != '-' : (text[i] < '0' || text[i] > '9'))
                {
                    return false;
                }
            }
            return true;
        }

        bool ReadAll(const std::filesystem::path& file, std::vector<std::uint8_t>& bytes)
        {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
            {
                return false;
            }
            bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            return true;
        }

        DatabaseCheck Failure(const DatabaseStatus status, const std::string& message)
        {
            DatabaseCheck check;
            check.status = status;
            check.message = message;
            return check;
        }
    }

    std::vector<FaceProfile> FaceDatabase::Profiles() const
    {
        std::vector<FaceProfile> profiles;
        for (const SharedFace& face : faces)
        {
            profiles.push_back({face.name, face.embeddings});
        }
        return profiles;
    }

    bool FaceDatabase::Validate(std::string* error) const
    {
        if (faces.empty())
        {
            SetError(error, "the database has no faces");
            return false;
        }
        if (faces.size() > kMaxFaces)
        {
            SetError(error, "the database has too many faces (" + std::to_string(kMaxFaces) + " at most)");
            return false;
        }
        for (std::size_t i = 0; i < faces.size(); ++i)
        {
            const SharedFace& face = faces[i];
            if (face.name.empty() || face.name.size() > FaceProfileStore::kMaxNameBytes)
            {
                SetError(error, "a name must be 1 to 32 bytes");
                return false;
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (faces[j].name == face.name)
                {
                    SetError(error, "the name \"" + face.name + "\" is used twice");
                    return false;
                }
            }
            if (!IsDate(face.date))
            {
                SetError(error, "the date of \"" + face.name + "\" is not YYYY-MM-DD");
                return false;
            }
            if (face.embeddings.size() < FaceProfileStore::kMinEmbeddings || face.embeddings.size() > FaceProfileStore::kMaxEmbeddings)
            {
                SetError(error, "\"" + face.name + "\" needs 5 to 10 faces");
                return false;
            }
            for (const FaceEmbedding& embedding : face.embeddings)
            {
                for (const float value : embedding)
                {
                    if (!std::isfinite(value))
                    {
                        SetError(error, "the numbers of \"" + face.name + "\" are not valid");
                        return false;
                    }
                }
            }
        }
        if (!IsDate(created))
        {
            SetError(error, "the date of the database is not YYYY-MM-DD");
            return false;
        }
        if (password.Valid() &&
            (password.iterations < kMinIterations || password.iterations > kMaxIterations || password.salt.size() < 8 || password.salt.size() > 64 || password.hash.size() != 32))
        {
            SetError(error, "the password hash is not valid");
            return false;
        }
        return true;
    }

    std::vector<std::uint8_t> SerializeDatabaseBody(const FaceDatabase& database)
    {
        std::vector<std::uint8_t> body;
        Put32(body, static_cast<std::uint32_t>(database.faces.size()));
        for (const SharedFace& face : database.faces)
        {
            PutText(body, face.name);
            PutText(body, face.date);
            body.push_back(static_cast<std::uint8_t>(face.embeddings.size()));
            for (const FaceEmbedding& embedding : face.embeddings)
            {
                for (const float value : embedding)
                {
                    std::uint32_t bits = 0;
                    std::memcpy(&bits, &value, sizeof(bits));
                    Put32(body, bits);
                }
            }
        }
        if (database.password.Valid())
        {
            body.push_back(1);
            Put32(body, database.password.iterations);
            body.push_back(static_cast<std::uint8_t>(database.password.salt.size()));
            body.insert(body.end(), database.password.salt.begin(), database.password.salt.end());
            body.push_back(static_cast<std::uint8_t>(database.password.hash.size()));
            body.insert(body.end(), database.password.hash.begin(), database.password.hash.end());
        }
        else
        {
            body.push_back(0);
        }
        PutText(body, database.created);
        return body;
    }

    bool ParseDatabaseBody(const std::vector<std::uint8_t>& body, FaceDatabase& database, std::string* error)
    {
        Reader reader{body};
        FaceDatabase result;
        const std::uint32_t count = reader.U32();
        if (!reader.ok || count == 0 || count > FaceDatabase::kMaxFaces)
        {
            SetError(error, "the number of faces is wrong");
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i)
        {
            SharedFace face;
            face.name = reader.Text();
            face.date = reader.Text();
            const std::size_t embeddings = reader.U8();
            if (!reader.ok || embeddings < FaceProfileStore::kMinEmbeddings || embeddings > FaceProfileStore::kMaxEmbeddings)
            {
                SetError(error, "a face has a wrong number of embeddings");
                return false;
            }
            for (std::size_t e = 0; e < embeddings; ++e)
            {
                FaceEmbedding embedding{};
                for (float& value : embedding)
                {
                    const std::uint32_t bits = reader.U32();
                    std::memcpy(&value, &bits, sizeof(value));
                }
                face.embeddings.push_back(embedding);
            }
            if (!reader.ok)
            {
                SetError(error, "the file is cut");
                return false;
            }
            result.faces.push_back(std::move(face));
        }
        if (reader.U8() == 1)
        {
            result.password.iterations = reader.U32();
            result.password.salt = reader.Bytes(reader.U8());
            result.password.hash = reader.Bytes(reader.U8());
        }
        result.created = reader.Text();
        if (!reader.ok || reader.position != body.size())
        {
            SetError(error, "the file is cut or has something after its end");
            return false;
        }
        if (!result.Validate(error))
        {
            return false;
        }
        database = std::move(result);
        return true;
    }

    bool SignDatabase(const FaceDatabase& database, const FaceSigner& signer, std::vector<std::uint8_t>& file, std::string* error)
    {
        if (!database.Validate(error))
        {
            return false;
        }
        const std::vector<std::uint8_t> body = SerializeDatabaseBody(database);
        std::vector<std::uint8_t> bytes;
        Put32(bytes, kMagic);
        Put32(bytes, FaceDatabase::kVersion);
        Put32(bytes, static_cast<std::uint32_t>(body.size()));
        bytes.insert(bytes.end(), body.begin(), body.end());
        std::vector<std::uint8_t> signature;
        if (!signer.Sign(bytes, signature, error))
        {
            return false;
        }
        Put32(bytes, static_cast<std::uint32_t>(signature.size()));
        bytes.insert(bytes.end(), signature.begin(), signature.end());
        file = std::move(bytes);
        return true;
    }

    DatabaseCheck CheckDatabase(const std::vector<std::uint8_t>& file, const std::vector<std::vector<std::uint8_t>>& trustedKeys)
    {
        const std::string damaged = "The face database is damaged or forged: ";
        if (file.size() < kHeaderSize + 4)
        {
            return Failure(DatabaseStatus::Damaged, damaged + "the file is too short.");
        }
        Reader reader{file};
        if (reader.U32() != kMagic)
        {
            return Failure(DatabaseStatus::Damaged, damaged + "this is not a face database.");
        }
        if (reader.U32() != FaceDatabase::kVersion)
        {
            return Failure(DatabaseStatus::Damaged, damaged + "the version of the file is not known to this engine.");
        }
        const std::uint32_t bodyLength = reader.U32();
        if (bodyLength > file.size() - kHeaderSize || file.size() - kHeaderSize - bodyLength < 4)
        {
            return Failure(DatabaseStatus::Damaged, damaged + "the file is cut.");
        }
        const std::size_t signedSize = kHeaderSize + bodyLength;
        Reader tail{file, signedSize};
        const std::uint32_t signatureLength = tail.U32();
        if (signatureLength != kSignatureSize || file.size() != signedSize + 4 + signatureLength)
        {
            return Failure(DatabaseStatus::Damaged, damaged + "there is no signature.");
        }
        if (trustedKeys.empty())
        {
            return Failure(DatabaseStatus::NoTrustedKey, "This build has no key to check the face database with, so the database is not used. The publisher puts the key into FaceIdTrustedKey.h.");
        }
        const std::vector<std::uint8_t> message(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(signedSize));
        const std::vector<std::uint8_t> signature(file.begin() + static_cast<std::ptrdiff_t>(signedSize + 4), file.end());
        int keyIndex = -1;
        for (std::size_t i = 0; i < trustedKeys.size(); ++i)
        {
            if (VerifySignature(trustedKeys[i], message, signature))
            {
                keyIndex = static_cast<int>(i);
                break;
            }
        }
        if (keyIndex < 0)
        {
            return Failure(DatabaseStatus::BadSignature, damaged + "the signature does not fit: the file was changed or it was signed by somebody else.");
        }
        DatabaseCheck check;
        std::string error;
        const std::vector<std::uint8_t> body(file.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), file.begin() + static_cast<std::ptrdiff_t>(signedSize));
        if (!ParseDatabaseBody(body, check.database, &error))
        {
            return Failure(DatabaseStatus::Damaged, damaged + error + ".");
        }
        check.status = DatabaseStatus::Ok;
        check.keyIndex = keyIndex;
        return check;
    }

    DatabaseCheck LoadDatabase(const std::filesystem::path& file, const std::vector<std::vector<std::uint8_t>>& trustedKeys)
    {
        std::vector<std::uint8_t> bytes;
        if (!ReadAll(file, bytes))
        {
            return Failure(DatabaseStatus::Unreadable, "The face database cannot be read: " + file.string());
        }
        return CheckDatabase(bytes, trustedKeys);
    }

    bool SaveDatabaseFile(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes, std::string* error)
    {
        std::error_code ec;
        if (!file.parent_path().empty())
        {
            std::filesystem::create_directories(file.parent_path(), ec);
        }
        const std::filesystem::path temporary = file.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!stream)
            {
                SetError(error, "the file could not be written: " + file.string());
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
        if (ec)
        {
            SetError(error, "the file could not be replaced: " + file.string());
            return false;
        }
        return true;
    }

    std::filesystem::path ExecutableDirectory()
    {
        wchar_t buffer[MAX_PATH * 2];
        const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        if (length == 0 || length >= std::size(buffer))
        {
            return {};
        }
        return std::filesystem::path(std::wstring(buffer, length)).parent_path();
    }

    DatabaseLocation FindDatabase(const std::filesystem::path& named, const std::filesystem::path& executableDirectory)
    {
        std::error_code ec;
        DatabaseLocation location;
        if (!named.empty())
        {
            location.path = named;
            location.named = true;
            location.exists = std::filesystem::is_regular_file(named, ec);
            return location;
        }
        const std::filesystem::path candidates[] = {executableDirectory / "faceid" / kDatabaseFileName, executableDirectory / "assets" / "faceid" / kDatabaseFileName};
        for (const std::filesystem::path& candidate : candidates)
        {
            if (!executableDirectory.empty() && std::filesystem::is_regular_file(candidate, ec))
            {
                location.path = candidate;
                location.exists = true;
                return location;
            }
        }
        return location;
    }

    std::string TodayText()
    {
        const std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_s(&utc, &now);
        char buffer[16];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &utc);
        return buffer;
    }

    std::vector<std::uint8_t> BuiltInPublicKey()
    {
        std::vector<std::uint8_t> key;
        if (kTrustedPublicKeyHex[0] != '\0' && FromHex(kTrustedPublicKeyHex, key) && key.size() == kPublicKeySize)
        {
            return key;
        }
        return {};
    }
}
