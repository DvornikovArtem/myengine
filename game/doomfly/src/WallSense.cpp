// WallSense.cpp

#include <doomfly/WallSense.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace doomfly
{
    namespace
    {
        constexpr char kMagic[8] = {'F', 'L', 'Y', 'W', 'A', 'L', 'L', '3'};
        constexpr float kByteScale = 127.0f;   // a code value in the file: a signed byte of value * 127 (a value is -1 .. 1)

        void Fail(std::string* error, const std::string& text)
        {
            if (error != nullptr)
            {
                *error = text;
            }
        }
    }

    void WallSense::Build(const WallSenseConfig& config, const std::vector<float>& columnX, const std::vector<float>& columnY)
    {
        config_ = config;
        config_.cellsX = std::max(2u, config_.cellsX & ~1u);
        config_.cellsY = std::max(1u, config_.cellsY);
        config_.capacity = std::max(config_.capacity, config_.neighbours + 1);
        half_ = config_.cellsX * config_.cellsY / 2;
        const uint32_t halfX = config_.cellsX / 2;
        cellOf_.resize(columnX.size());
        for (size_t c = 0; c < columnX.size(); ++c)
        {
            const uint32_t x = std::min(config_.cellsX - 1, static_cast<uint32_t>(std::clamp(columnX[c], 0.0f, 0.999f) * config_.cellsX));
            const uint32_t y = std::min(config_.cellsY - 1, static_cast<uint32_t>(std::clamp(c < columnY.size() ? columnY[c] : 0.5f, 0.0f, 0.999f) * config_.cellsY));
            // the code: the cells of the left half row by row, then those of the right half
            cellOf_[c] = (x < halfX ? 0 : half_) + y * halfX + (x % halfX);
        }
        memories_.clear();
        place_.clear();
        memory_ = nullptr;
        recent_.clear();
        SetPlace("");
    }

    void WallSense::SetPlace(const std::string& place)
    {
        if (memory_ != nullptr && place == place_)
        {
            return;
        }
        place_ = place;
        memory_ = &memories_[place];
        recent_.clear();
        avoidTics_ = 0;
        restTics_ = 0;
        touched_ = false;
    }

    uint32_t WallSense::Positives() const
    {
        return memory_ != nullptr ? memory_->positives : 0;
    }

    uint32_t WallSense::Remembered() const
    {
        return memory_ != nullptr ? static_cast<uint32_t>(memory_->marks.size()) : 0;
    }

    void WallSense::Remember(const Pending& picture)
    {
        Memory& m = *memory_;
        const size_t size = 2 * static_cast<size_t>(half_);
        if (m.marks.size() < config_.capacity)
        {
            m.codes.insert(m.codes.end(), picture.code.begin(), picture.code.end());
            m.marks.push_back(picture.positive ? 1 : 0);
        }
        else
        {
            m.positives -= m.marks[m.next];
            std::copy(picture.code.begin(), picture.code.end(), m.codes.begin() + static_cast<std::ptrdiff_t>(m.next * size));
            m.marks[m.next] = picture.positive ? 1 : 0;
            m.next = (m.next + 1) % config_.capacity;
        }
        m.positives += picture.positive ? 1 : 0;
    }

    // half 0: the left half, 1: the right half, 2: the whole picture (the halves each have length 1, the whole the cosine of the two together / 2)
    float WallSense::Recall(const std::vector<float>& code, const uint32_t half) const
    {
        const Memory& m = *memory_;
        const size_t count = m.marks.size();
        const uint32_t k = config_.neighbours;
        if (count <= k || m.positives == 0)
        {
            return 0.0f;
        }
        const size_t size = 2 * static_cast<size_t>(half_);
        std::vector<std::pair<float, uint8_t>> similar(count);
        const size_t from = half == 2 ? 0 : static_cast<size_t>(half) * half_;
        const size_t length = half == 2 ? size : half_;
        const float norm = half == 2 ? 0.5f : 1.0f;
        const float* now = code.data() + from;
        for (size_t i = 0; i < count; ++i)
        {
            const float* stored = m.codes.data() + i * size + from;
            float dot = 0.0f;
            for (size_t j = 0; j < length; ++j)
            {
                dot += stored[j] * now[j];
            }
            similar[i] = {dot * norm, m.marks[i]};
        }
        std::nth_element(similar.begin(), similar.begin() + k, similar.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        float weight = 0.0f;
        float marked = 0.0f;
        for (uint32_t i = 0; i < k; ++i)
        {
            const float s = std::max(similar[i].first, 0.0f);
            const float w = s * s * s * s;
            weight += w;
            marked += w * similar[i].second;
        }
        return weight > 0.0f ? marked / weight : 0.0f;
    }

    void WallSense::Tick(const std::vector<float>& columnLuminance, const bool touching, const bool walking)
    {
        if (!config_.enabled || memory_ == nullptr || columnLuminance.size() != cellOf_.size())
        {
            return;
        }
        // the code of the picture: the mean light of the cells, per half the mean taken away and the length made 1
        std::vector<float> code(2 * static_cast<size_t>(half_), 0.0f);
        std::vector<float> count(code.size(), 0.0f);
        for (size_t c = 0; c < columnLuminance.size(); ++c)
        {
            code[cellOf_[c]] += columnLuminance[c];
            count[cellOf_[c]] += 1.0f;
        }
        for (uint32_t half = 0; half < 2; ++half)
        {
            float* v = code.data() + static_cast<size_t>(half) * half_;
            const float* n = count.data() + static_cast<size_t>(half) * half_;
            float mean = 0.0f;
            for (uint32_t j = 0; j < half_; ++j)
            {
                v[j] = n[j] > 0.0f ? v[j] / n[j] : 0.0f;
                mean += v[j];
            }
            mean /= static_cast<float>(half_);
            float length = 0.0f;
            for (uint32_t j = 0; j < half_; ++j)
            {
                v[j] -= mean;
                length += v[j] * v[j];
            }
            length = std::sqrt(length) + 1e-6f;
            for (uint32_t j = 0; j < half_; ++j)
            {
                v[j] /= length;
            }
        }

        // the chance of a wall from the whole picture; the side from the halves (the half that sees the wall is like the pictures before a touch, the other
        // one is like anything)
        chance_ = Recall(code, 2);
        const float left = chance_ >= config_.avoidFrom ? Recall(code, 0) : 0.0f;
        const float right = chance_ >= config_.avoidFrom ? Recall(code, 1) : 0.0f;
        if (restTics_ > 0)
        {
            --restTics_;
        }
        else if (avoidTics_ == 0 && chance_ >= config_.avoidFrom)
        {
            // the side is chosen when it starts and kept while it lasts: away from the half that sees more wall
            avoidTics_ = 1;
            turnSign_ = left > right ? 1.0f : -1.0f;
        }
        else if (avoidTics_ > 0 && (chance_ < config_.avoidUntil || avoidTics_ >= config_.avoidMaxTics))
        {
            restTics_ = avoidTics_ >= config_.avoidMaxTics ? 20 : 0;
            avoidTics_ = 0;
        }
        else if (avoidTics_ > 0)
        {
            ++avoidTics_;
        }
        brake_ = avoidTics_ > 0 ? 1.0f : 0.0f;
        turn_ = turnSign_ * config_.turnDegrees * brake_;

        // learning: the pictures of the tics before a touch starts are positives, the ones that passed without one negatives; the pictures while it touches
        // are not remembered (they are the wall, not its coming)
        if (config_.learning)
        {
            if (touching && !touched_)
            {
                for (Pending& picture : recent_)
                {
                    picture.positive = true;
                }
                while (!recent_.empty())
                {
                    Remember(recent_.front());
                    recent_.pop_front();
                }
            }
            if (!touching && walking && tics_ % std::max(1u, config_.everyTics) == 0)
            {
                recent_.push_back({std::move(code), false});
            }
            while (recent_.size() > config_.horizonTics / std::max(1u, config_.everyTics))
            {
                Remember(recent_.front());
                recent_.pop_front();
            }
        }
        touched_ = touching;
        ++tics_;
    }

    // The file: the magic, cellsX, cellsY, the number of places; per place the length of its name, the name, the number of pictures, their marks and
    // their codes (bytes), the oldest picture first (a loaded memory forgets in the same order)
    bool WallSense::Save(const std::string& path, std::string* error) const
    {
        std::error_code ignored;
        std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ignored);
        std::ofstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
        const size_t size = 2 * static_cast<size_t>(half_);
        uint32_t places = 0;
        for (const auto& [name, m] : memories_)
        {
            places += m.marks.empty() ? 0 : 1;
        }
        const uint32_t header[3] = {config_.cellsX, config_.cellsY, places};
        file.write(kMagic, sizeof(kMagic));
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (const auto& [name, m] : memories_)
        {
            if (m.marks.empty())
            {
                continue;
            }
            const uint32_t length = static_cast<uint32_t>(name.size());
            const uint32_t pictures = static_cast<uint32_t>(m.marks.size());
            std::vector<uint8_t> marks(pictures);
            std::vector<int8_t> codes(pictures * size);
            for (size_t i = 0; i < pictures; ++i)
            {
                const size_t at = (m.next + i) % pictures;
                marks[i] = m.marks[at];
                for (size_t j = 0; j < size; ++j)
                {
                    codes[i * size + j] = static_cast<int8_t>(std::lround(std::clamp(m.codes[at * size + j], -1.0f, 1.0f) * kByteScale));
                }
            }
            file.write(reinterpret_cast<const char*>(&length), 4);
            file.write(name.data(), length);
            file.write(reinterpret_cast<const char*>(&pictures), 4);
            file.write(reinterpret_cast<const char*>(marks.data()), static_cast<std::streamsize>(marks.size()));
            file.write(reinterpret_cast<const char*>(codes.data()), static_cast<std::streamsize>(codes.size()));
        }
        if (!file)
        {
            Fail(error, "cannot write " + path);
            return false;
        }
        return true;
    }

    bool WallSense::Load(const std::string& path, std::string* error, const bool merge)
    {
        std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
        char magic[8] = {};
        uint32_t header[3] = {};
        file.read(magic, sizeof(magic));
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!file || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        {
            Fail(error, "not a trained fly: " + path);
            return false;
        }
        if (header[0] != config_.cellsX || header[1] != config_.cellsY)
        {
            Fail(error, "the trained fly has another grid: " + path);
            return false;
        }
        const size_t size = 2 * static_cast<size_t>(half_);
        std::map<std::string, Memory> loaded;
        for (uint32_t p = 0; p < header[2]; ++p)
        {
            uint32_t length = 0;
            uint32_t pictures = 0;
            file.read(reinterpret_cast<char*>(&length), 4);
            if (!file || length > 1024)
            {
                Fail(error, "the trained fly is broken: " + path);
                return false;
            }
            std::string name(length, '\0');
            file.read(name.data(), length);
            file.read(reinterpret_cast<char*>(&pictures), 4);
            if (!file || pictures > 10u * 1000u * 1000u)
            {
                Fail(error, "the trained fly is broken: " + path);
                return false;
            }
            std::vector<uint8_t> marks(pictures);
            std::vector<int8_t> codes(pictures * size);
            file.read(reinterpret_cast<char*>(marks.data()), static_cast<std::streamsize>(marks.size()));
            file.read(reinterpret_cast<char*>(codes.data()), static_cast<std::streamsize>(codes.size()));
            if (!file)
            {
                Fail(error, "the trained fly is cut short: " + path);
                return false;
            }
            // the newest pictures when the file has more than the memory holds
            const size_t skip = pictures - std::min<size_t>(pictures, config_.capacity);
            Memory& m = loaded[name];
            m.marks.assign(marks.begin() + static_cast<std::ptrdiff_t>(skip), marks.end());
            m.codes.resize(m.marks.size() * size);
            for (size_t i = 0; i < m.codes.size(); ++i)
            {
                m.codes[i] = static_cast<float>(codes[skip * size + i]) / kByteScale;
            }
            for (const uint8_t mark : m.marks)
            {
                m.positives += mark;
            }
        }
        if (!merge)
        {
            memories_.clear();
        }
        for (auto& [name, m] : loaded)
        {
            memories_[name] = std::move(m);
        }
        const std::string place = place_;
        memory_ = nullptr;
        SetPlace(place);
        return true;
    }
}
