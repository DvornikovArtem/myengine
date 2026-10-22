// WavFile.cpp

#include <myengine/audio/WavFile.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace myengine::audio
{
    namespace
    {
        std::uint32_t Read32(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
        std::uint16_t Read16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

        void Write32(std::vector<std::uint8_t>& out, const std::uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
            {
                out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
            }
        }

        void Write16(std::vector<std::uint8_t>& out, const std::uint16_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value & 0xFF));
            out.push_back(static_cast<std::uint8_t>(value >> 8));
        }

        std::int16_t ClampToInt16(const double value)
        {
            return static_cast<std::int16_t>(std::clamp(std::lround(value), -32768L, 32767L));
        }
    }

    bool ParseWav(const std::uint8_t* bytes, const std::size_t size, WavData& out, std::string& error)
    {
        out = {};
        if (bytes == nullptr || size < 12 || std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0)
        {
            error = "not a RIFF/WAVE file";
            return false;
        }

        std::uint16_t format = 0;
        std::uint16_t channels = 0;
        std::uint32_t sampleRate = 0;
        std::uint16_t bits = 0;
        bool haveFormat = false;
        const std::uint8_t* data = nullptr;
        std::size_t dataSize = 0;

        std::size_t position = 12;
        while (position + 8 <= size)
        {
            const std::uint8_t* chunk = bytes + position;
            const std::size_t chunkSize = Read32(chunk + 4);
            const std::size_t bodyStart = position + 8;
            if (bodyStart + chunkSize > size)
            {
                // The data chunk of a stream may claim more than there is: take what is there
                if (std::memcmp(chunk, "data", 4) == 0 && haveFormat)
                {
                    data = bytes + bodyStart;
                    dataSize = size - bodyStart;
                    break;
                }
                error = "a chunk is truncated";
                return false;
            }
            if (std::memcmp(chunk, "fmt ", 4) == 0)
            {
                if (chunkSize < 16)
                {
                    error = "the fmt chunk is too short";
                    return false;
                }
                format = Read16(bytes + bodyStart);
                channels = Read16(bytes + bodyStart + 2);
                sampleRate = Read32(bytes + bodyStart + 4);
                bits = Read16(bytes + bodyStart + 14);
                if (format == 0xFFFE && chunkSize >= 26)
                {
                    format = Read16(bytes + bodyStart + 24); // WAVE_FORMAT_EXTENSIBLE: the first two bytes of the sub format are the tag
                }
                haveFormat = true;
            }
            else if (std::memcmp(chunk, "data", 4) == 0)
            {
                data = bytes + bodyStart;
                dataSize = chunkSize;
            }
            position = bodyStart + chunkSize + (chunkSize & 1);
        }

        if (!haveFormat || data == nullptr)
        {
            error = !haveFormat ? "no fmt chunk" : "no data chunk";
            return false;
        }
        if (channels < 1 || channels > 2)
        {
            error = "only mono and stereo are supported (" + std::to_string(channels) + " channels)";
            return false;
        }
        if (sampleRate < 8000 || sampleRate > 192000)
        {
            error = "the sample rate " + std::to_string(sampleRate) + " is out of range";
            return false;
        }
        const bool pcm = format == 1;
        const bool floating = format == 3;
        if ((!pcm && !floating) || (pcm && bits != 8 && bits != 16 && bits != 24 && bits != 32) || (floating && bits != 32))
        {
            error = "unsupported format (tag " + std::to_string(format) + ", " + std::to_string(bits) + " bit): PCM 8/16/24/32 or float 32";
            return false;
        }

        const std::size_t bytesPerSample = bits / 8;
        const std::size_t count = dataSize / (bytesPerSample * channels) * channels;
        out.sampleRate = sampleRate;
        out.channels = channels;
        out.samples.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::uint8_t* p = data + i * bytesPerSample;
            std::int16_t value = 0;
            if (floating)
            {
                float f;
                std::memcpy(&f, p, 4);
                value = ClampToInt16(static_cast<double>(f) * 32767.0);
            }
            else if (bits == 16)
            {
                value = static_cast<std::int16_t>(Read16(p));
            }
            else if (bits == 8)
            {
                value = static_cast<std::int16_t>((static_cast<int>(p[0]) - 128) * 256);
            }
            else if (bits == 24)
            {
                const std::int32_t v = static_cast<std::int32_t>((static_cast<std::uint32_t>(p[0]) << 8) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 24)) >> 16;
                value = static_cast<std::int16_t>(v);
            }
            else
            {
                value = static_cast<std::int16_t>(static_cast<std::int32_t>(Read32(p)) >> 16);
            }
            out.samples[i] = value;
        }
        if (out.samples.empty())
        {
            error = "the data chunk has no samples";
            return false;
        }
        return true;
    }

    bool LoadWavFile(const std::filesystem::path& path, WavData& out, std::string& error)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open())
        {
            error = "cannot open the file";
            return false;
        }
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return ParseWav(bytes.data(), bytes.size(), out, error);
    }

    std::vector<std::uint8_t> BuildWav(const WavData& data)
    {
        const std::uint32_t dataBytes = static_cast<std::uint32_t>(data.samples.size() * 2);
        std::vector<std::uint8_t> out;
        out.reserve(44 + dataBytes);
        const auto tag = [&](const char* text) { out.insert(out.end(), text, text + 4); };
        tag("RIFF");
        Write32(out, 36 + dataBytes);
        tag("WAVE");
        tag("fmt ");
        Write32(out, 16);
        Write16(out, 1);
        Write16(out, data.channels);
        Write32(out, data.sampleRate);
        Write32(out, data.sampleRate * data.channels * 2);
        Write16(out, static_cast<std::uint16_t>(data.channels * 2));
        Write16(out, 16);
        tag("data");
        Write32(out, dataBytes);
        for (const std::int16_t sample : data.samples)
        {
            Write16(out, static_cast<std::uint16_t>(sample));
        }
        return out;
    }
}
