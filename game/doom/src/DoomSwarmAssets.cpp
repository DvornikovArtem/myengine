// DoomSwarmAssets.cpp
//
// The look and the sound of a fly of the swarm, made here and not taken from a file: a drawing of a fly in the palette of the WAD in the lumps SKULA0..SKULK0
// (the frames of the Lost Soul: 0 and 1 in flight, 2..4 are the same drawing, 5..10 the death), and the buzz in the lump DSFLYBZ. They are ours (the code
// below is the only source), so they go into a package with no license of their own. The wrappers of DoomSwarm.c for the host are at the end.

#include <doom/DoomSwarm.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "DoomSwarmC.h"

namespace doom::swarm
{
    namespace
    {
        constexpr int W = kSpriteWidth;
        constexpr int H = kSpriteHeight;
        constexpr float kPi = 3.14159265358979f;

        struct Color
        {
            float r, g, b;
        };

        struct Pixel
        {
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;
            float a = 0.0f;
        };

        struct Canvas
        {
            std::array<Pixel, W * H> pixels;

            // `source` over what is there, with the coverage `alpha`
            void Blend(const int x, const int y, const Color source, const float alpha)
            {
                if (x < 0 || y < 0 || x >= W || y >= H || alpha <= 0.0f)
                {
                    return;
                }
                Pixel& pixel = pixels[static_cast<std::size_t>(y * W + x)];
                const float outAlpha = alpha + pixel.a * (1.0f - alpha);
                if (outAlpha <= 0.0f)
                {
                    return;
                }
                pixel.r = (source.r * alpha + pixel.r * pixel.a * (1.0f - alpha)) / outAlpha;
                pixel.g = (source.g * alpha + pixel.g * pixel.a * (1.0f - alpha)) / outAlpha;
                pixel.b = (source.b * alpha + pixel.b * pixel.a * (1.0f - alpha)) / outAlpha;
                pixel.a = outAlpha;
            }

            void Ellipse(const float cx, const float cy, const float rx, const float ry, const float rotationDegrees, const Color color, const float alpha = 1.0f)
            {
                const float radians = rotationDegrees * kPi / 180.0f;
                const float c = std::cos(radians);
                const float s = std::sin(radians);
                const float reach = std::max(rx, ry) + 2.0f;
                for (int y = static_cast<int>(cy - reach); y <= static_cast<int>(cy + reach); ++y)
                {
                    for (int x = static_cast<int>(cx - reach); x <= static_cast<int>(cx + reach); ++x)
                    {
                        const float dx = static_cast<float>(x) + 0.5f - cx;
                        const float dy = static_cast<float>(y) + 0.5f - cy;
                        const float u = dx * c + dy * s;
                        const float v = -dx * s + dy * c;
                        const float d = std::sqrt((u / rx) * (u / rx) + (v / ry) * (v / ry));
                        const float coverage = std::clamp((1.0f - d) * std::min(rx, ry) + 0.5f, 0.0f, 1.0f);
                        Blend(x, y, color, coverage * alpha);
                    }
                }
            }

            void Line(const float x0, const float y0, const float x1, const float y1, const float thickness, const Color color, const float alpha = 1.0f)
            {
                const float length = std::max(1.0f, std::hypot(x1 - x0, y1 - y0));
                const int steps = static_cast<int>(length * 2.0f);
                for (int i = 0; i <= steps; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(steps);
                    const float x = x0 + (x1 - x0) * t;
                    const float y = y0 + (y1 - y0) * t;
                    Ellipse(x, y, thickness * 0.5f + 0.4f, thickness * 0.5f + 0.4f, 0.0f, color, alpha * 0.55f);
                }
            }
        };

        constexpr Color kBody{44.0f, 30.0f, 24.0f};
        constexpr Color kThorax{82.0f, 58.0f, 40.0f};
        constexpr Color kAbdomen{70.0f, 50.0f, 34.0f};
        constexpr Color kStripe{128.0f, 98.0f, 56.0f};
        constexpr Color kEye{214.0f, 28.0f, 28.0f};
        constexpr Color kEyeLight{255.0f, 150.0f, 130.0f};
        constexpr Color kLeg{30.0f, 22.0f, 18.0f};
        constexpr Color kWing{198.0f, 214.0f, 236.0f};
        constexpr Color kVein{110.0f, 130.0f, 168.0f};
        constexpr Color kSplat{150.0f, 176.0f, 40.0f};
        constexpr Color kSplatDark{88.0f, 112.0f, 22.0f};

        // a fly seen from the front, wings up (frame 0) or down (frame 1)
        void DrawLiving(Canvas& canvas, const bool wingsUp, const float scale = 1.0f, const float shiftY = 0.0f)
        {
            const float cx = 28.0f;
            const float oy = shiftY;
            // the wings first: translucent, with the veins
            for (int side = -1; side <= 1; side += 2)
            {
                const float sign = static_cast<float>(side);
                const float wx = cx + sign * 15.0f * scale;
                const float wy = (wingsUp ? 16.0f : 27.0f) * scale + oy + (1.0f - scale) * 24.0f;
                const float tilt = (wingsUp ? -36.0f : 14.0f) * sign;
                canvas.Ellipse(wx, wy, 14.0f * scale, 5.6f * scale, tilt, kWing, 0.5f);
                const float radians = tilt * kPi / 180.0f;
                for (int vein = -1; vein <= 1; ++vein)
                {
                    const float ox = std::cos(radians) * 12.0f * scale * (sign < 0 ? -1.0f : 1.0f) * -sign;
                    const float oyy = std::sin(radians) * 12.0f * scale;
                    canvas.Line(wx - ox, wy - oyy + static_cast<float>(vein) * 1.6f, wx + ox, wy + oyy + static_cast<float>(vein) * 1.6f, 0.6f, kVein, 0.5f);
                }
            }
            // the legs
            for (int side = -1; side <= 1; side += 2)
            {
                const float sign = static_cast<float>(side);
                for (int leg = 0; leg < 3; ++leg)
                {
                    const float y0 = (25.0f + static_cast<float>(leg) * 2.0f) * scale + oy + (1.0f - scale) * 24.0f;
                    canvas.Line(cx + sign * 3.5f * scale, y0, cx + sign * (9.0f + static_cast<float>(leg) * 3.0f) * scale, y0 + (11.0f - static_cast<float>(leg)) * scale,
                                1.2f * scale, kLeg);
                }
            }
            const float base = (1.0f - scale) * 24.0f + oy;
            // the abdomen with the stripes, the thorax, the head with the two big red eyes
            canvas.Ellipse(cx, 37.0f * scale + base, 5.4f * scale, 9.0f * scale, 0.0f, kAbdomen);
            for (int stripe = 0; stripe < 3; ++stripe)
            {
                canvas.Ellipse(cx, (33.0f + static_cast<float>(stripe) * 4.0f) * scale + base, 5.0f * scale, 1.2f * scale, 0.0f, kStripe, 0.8f);
            }
            canvas.Ellipse(cx, 24.0f * scale + base, 6.6f * scale, 8.2f * scale, 0.0f, kThorax);
            canvas.Ellipse(cx, 22.0f * scale + base, 4.0f * scale, 3.2f * scale, 0.0f, kBody, 0.6f);
            canvas.Ellipse(cx, 15.0f * scale + base, 5.2f * scale, 4.6f * scale, 0.0f, kBody);
            canvas.Ellipse(cx - 4.6f * scale, 14.0f * scale + base, 3.9f * scale, 4.2f * scale, 0.0f, kEye);
            canvas.Ellipse(cx + 4.6f * scale, 14.0f * scale + base, 3.9f * scale, 4.2f * scale, 0.0f, kEye);
            canvas.Ellipse(cx - 5.4f * scale, 12.6f * scale + base, 1.3f * scale, 1.5f * scale, 0.0f, kEyeLight);
            canvas.Ellipse(cx + 3.8f * scale, 12.6f * scale + base, 1.3f * scale, 1.5f * scale, 0.0f, kEyeLight);
            canvas.Line(cx, 18.0f * scale + base, cx, 21.0f * scale + base, 1.2f * scale, kLeg);
        }

        // what is left of a fly: a splash that grows and a splat on the floor
        void DrawDeath(Canvas& canvas, const int frame)
        {
            std::uint32_t seed = 12345u;
            auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f; };
            if (frame == 5)
            {
                DrawLiving(canvas, false, 0.9f, 3.0f);
                for (int i = 0; i < 9; ++i)
                {
                    const float angle = next() * 2.0f * kPi;
                    const float radius = 7.0f + next() * 6.0f;
                    canvas.Ellipse(28.0f + std::cos(angle) * radius, 26.0f + std::sin(angle) * radius, 1.6f, 1.6f, 0.0f, kSplat);
                }
                return;
            }
            if (frame <= 8)
            {
                const float shrink = 1.0f - static_cast<float>(frame - 5) * 0.28f;
                if (shrink > 0.0f)
                {
                    DrawLiving(canvas, false, std::max(0.25f, shrink), 6.0f);
                }
                const float grow = static_cast<float>(frame - 5);
                for (int i = 0; i < 14; ++i)
                {
                    const float angle = next() * 2.0f * kPi;
                    const float radius = 5.0f + grow * 4.0f + next() * 6.0f;
                    canvas.Ellipse(28.0f + std::cos(angle) * radius, 28.0f + std::sin(angle) * radius * 0.8f, 2.0f - grow * 0.2f, 2.0f - grow * 0.2f, 0.0f, i % 2 ? kSplat : kSplatDark);
                }
                return;
            }
            // 9 and 10: on the floor
            const float spread = frame == 9 ? 13.0f : 16.0f;
            canvas.Ellipse(28.0f, 43.0f, spread, 3.8f, 0.0f, kSplatDark);
            canvas.Ellipse(28.0f, 43.0f, spread - 2.5f, 2.8f, 0.0f, kSplat);
            canvas.Ellipse(24.0f, 41.5f, 3.0f, 1.2f, 0.0f, kEyeLight, 0.6f);
            for (int i = 0; i < 6; ++i)
            {
                const float x = 28.0f + (next() - 0.5f) * 2.0f * (spread + 2.0f);
                canvas.Ellipse(x, 44.5f - next() * 2.0f, 1.2f, 0.9f, 0.0f, kSplat);
            }
            // the wings left
            canvas.Ellipse(20.0f, 41.0f, 5.0f, 1.6f, -8.0f, kWing, 0.5f);
            canvas.Ellipse(37.0f, 41.5f, 4.5f, 1.4f, 10.0f, kWing, 0.5f);
        }

        void Draw(Canvas& canvas, const int frame)
        {
            if (frame >= 5)
            {
                DrawDeath(canvas, frame);
            }
            else
            {
                DrawLiving(canvas, frame % 2 == 0);
            }
        }

        std::uint32_t Pack(const unsigned char r, const unsigned char g, const unsigned char b)
        {
            return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
        }

        // The pixel as 8 bit colour and whether it is there: more than half covered is a pixel; a third is every second one (a translucent wing in the 1993 way)
        bool Resolve(const Pixel& pixel, const int x, const int y, unsigned char& r, unsigned char& g, unsigned char& b)
        {
            const bool visible = pixel.a >= 0.6f || (pixel.a >= 0.22f && ((x + y) & 1) == 0);
            if (!visible)
            {
                return false;
            }
            r = static_cast<unsigned char>(std::clamp(pixel.r, 0.0f, 255.0f));
            g = static_cast<unsigned char>(std::clamp(pixel.g, 0.0f, 255.0f));
            b = static_cast<unsigned char>(std::clamp(pixel.b, 0.0f, 255.0f));
            return true;
        }

        // a picture of DOOM (the "patch" format): the header, the offsets of the columns, the columns made of posts of opaque pixels
        std::vector<unsigned char> EncodePatch(const Canvas& canvas, const std::array<unsigned char, 768>& palette)
        {
            std::map<std::uint32_t, unsigned char> cache;
            auto nearest = [&](const unsigned char r, const unsigned char g, const unsigned char b)
            {
                const std::uint32_t key = Pack(r, g, b);
                const auto found = cache.find(key);
                if (found != cache.end())
                {
                    return found->second;
                }
                int best = 0;
                int bestDistance = 1 << 30;
                for (int i = 0; i < 256; ++i)
                {
                    const int dr = static_cast<int>(palette[static_cast<std::size_t>(i) * 3]) - r;
                    const int dg = static_cast<int>(palette[static_cast<std::size_t>(i) * 3 + 1]) - g;
                    const int db = static_cast<int>(palette[static_cast<std::size_t>(i) * 3 + 2]) - b;
                    const int distance = 2 * dr * dr + 4 * dg * dg + 3 * db * db;   // the eye is most sensitive to green
                    if (distance < bestDistance)
                    {
                        bestDistance = distance;
                        best = i;
                    }
                }
                cache[key] = static_cast<unsigned char>(best);
                return static_cast<unsigned char>(best);
            };

            std::vector<unsigned char> columns;
            std::vector<int> offsets(static_cast<std::size_t>(W));
            const int headerSize = 8 + 4 * W;
            for (int x = 0; x < W; ++x)
            {
                offsets[static_cast<std::size_t>(x)] = headerSize + static_cast<int>(columns.size());
                int y = 0;
                while (y < H)
                {
                    unsigned char r = 0, g = 0, b = 0;
                    if (!Resolve(canvas.pixels[static_cast<std::size_t>(y * W + x)], x, y, r, g, b))
                    {
                        ++y;
                        continue;
                    }
                    const int top = y;
                    std::vector<unsigned char> run;
                    while (y < H && Resolve(canvas.pixels[static_cast<std::size_t>(y * W + x)], x, y, r, g, b))
                    {
                        run.push_back(nearest(r, g, b));
                        ++y;
                    }
                    columns.push_back(static_cast<unsigned char>(top));
                    columns.push_back(static_cast<unsigned char>(run.size()));
                    columns.push_back(0);
                    columns.insert(columns.end(), run.begin(), run.end());
                    columns.push_back(0);
                }
                columns.push_back(0xFF);
            }

            std::vector<unsigned char> patch(static_cast<std::size_t>(headerSize));
            auto put16 = [&patch](const std::size_t at, const int value) { patch[at] = static_cast<unsigned char>(value & 0xFF); patch[at + 1] = static_cast<unsigned char>((value >> 8) & 0xFF); };
            put16(0, W);
            put16(2, H);
            put16(4, W / 2);   // the left offset: the middle of the picture is the middle of the mobj
            put16(6, H);       // the top offset: the bottom of the picture is the bottom of the mobj
            for (int x = 0; x < W; ++x)
            {
                const std::size_t at = static_cast<std::size_t>(8 + 4 * x);
                const int offset = offsets[static_cast<std::size_t>(x)];
                patch[at] = static_cast<unsigned char>(offset & 0xFF);
                patch[at + 1] = static_cast<unsigned char>((offset >> 8) & 0xFF);
                patch[at + 2] = static_cast<unsigned char>((offset >> 16) & 0xFF);
                patch[at + 3] = static_cast<unsigned char>((offset >> 24) & 0xFF);
            }
            patch.insert(patch.end(), columns.begin(), columns.end());
            return patch;
        }

        // The buzz: a saw-ish tone of about 200 Hz that trembles at the beat of the wings, with a little noise, as a sound of DMX (format 3, 11025 Hz, unsigned 8 bit,
        // 16 samples of padding on both sides)
        std::vector<unsigned char> MakeBuzz()
        {
            constexpr int kRate = 11025;
            constexpr int kSamples = kRate * 3 / 10;
            std::vector<unsigned char> lump(8 + 16 + kSamples + 16, 128);
            lump[0] = 3;
            lump[1] = 0;
            lump[2] = static_cast<unsigned char>(kRate & 0xFF);
            lump[3] = static_cast<unsigned char>((kRate >> 8) & 0xFF);
            const std::uint32_t length = kSamples + 32;
            lump[4] = static_cast<unsigned char>(length & 0xFF);
            lump[5] = static_cast<unsigned char>((length >> 8) & 0xFF);
            lump[6] = static_cast<unsigned char>((length >> 16) & 0xFF);
            lump[7] = static_cast<unsigned char>((length >> 24) & 0xFF);
            std::uint32_t seed = 777u;
            float phase = 0.0f;
            for (int i = 0; i < kSamples; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(kRate);
                const float frequency = 205.0f + 14.0f * std::sin(2.0f * kPi * 7.0f * t);
                phase += 2.0f * kPi * frequency / static_cast<float>(kRate);
                float tone = 0.0f;
                for (int harmonic = 1; harmonic <= 7; ++harmonic)
                {
                    tone += std::sin(phase * static_cast<float>(harmonic)) / static_cast<float>(harmonic);
                }
                const float wings = 0.55f + 0.45f * std::sin(2.0f * kPi * 36.0f * t);
                seed = seed * 1664525u + 1013904223u;
                const float noise = (static_cast<float>((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f) * 0.18f;
                const float envelope = std::min(1.0f, t / 0.02f) * std::min(1.0f, (0.3f - t) / 0.06f);
                const float value = (tone * 0.42f * wings + noise) * envelope;
                const int sample = 128 + static_cast<int>(std::clamp(value, -1.0f, 1.0f) * 120.0f);
                lump[static_cast<std::size_t>(8 + 16 + i)] = static_cast<unsigned char>(sample);
            }
            return lump;
        }

        // PLAYPAL (the first of the 14 palettes) of the WAD
        bool ReadPalette(const std::filesystem::path& wad, std::array<unsigned char, 768>& palette, std::string& error)
        {
            std::ifstream file(wad, std::ios::binary);
            if (!file)
            {
                error = "Cannot open " + wad.string();
                return false;
            }
            char header[12];
            file.read(header, 12);
            std::int32_t count = 0;
            std::int32_t directory = 0;
            std::memcpy(&count, header + 4, 4);
            std::memcpy(&directory, header + 8, 4);
            if (!file || count <= 0 || count > 1000000 || directory < 12)
            {
                error = wad.string() + " is not a WAD file";
                return false;
            }
            file.seekg(directory);
            for (std::int32_t i = 0; i < count; ++i)
            {
                char entry[16];
                file.read(entry, 16);
                if (!file)
                {
                    break;
                }
                if (std::strncmp(entry + 8, "PLAYPAL", 8) == 0)
                {
                    std::int32_t position = 0;
                    std::memcpy(&position, entry, 4);
                    file.seekg(position);
                    file.read(reinterpret_cast<char*>(palette.data()), 768);
                    if (!file)
                    {
                        break;
                    }
                    return true;
                }
            }
            error = wad.string() + " has no PLAYPAL";
            return false;
        }

        bool g_active = false;
    }

    void DrawSprite(const int frame, unsigned char* rgba)
    {
        Canvas canvas;
        Draw(canvas, std::clamp(frame, 0, kSpriteFrames - 1));
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                unsigned char r = 0, g = 0, b = 0;
                const bool there = Resolve(canvas.pixels[static_cast<std::size_t>(y * W + x)], x, y, r, g, b);
                unsigned char* out = rgba + (y * W + x) * 4;
                out[0] = there ? r : 0;
                out[1] = there ? g : 0;
                out[2] = there ? b : 0;
                out[3] = there ? 255 : 0;
            }
        }
    }

    bool PrepareAssets(const std::filesystem::path& iwad, std::string& error)
    {
        std::array<unsigned char, 768> palette{};
        if (!ReadPalette(iwad, palette, error))
        {
            return false;
        }
        std::vector<std::array<char, 8>> spriteNames;
        std::vector<std::vector<unsigned char>> sprites;
        for (int frame = 0; frame < kSpriteFrames; ++frame)
        {
            Canvas canvas;
            Draw(canvas, frame);
            sprites.push_back(EncodePatch(canvas, palette));
            std::array<char, 8> name{};
            std::memcpy(name.data(), "SKUL", 4);
            name[4] = static_cast<char>('A' + frame);
            name[5] = '0';
            spriteNames.push_back(name);
        }
        const std::vector<unsigned char> buzz = MakeBuzz();
        std::array<char, 8> buzzName{};
        std::memcpy(buzzName.data(), "DSFLYBZ", 7);

        std::vector<const unsigned char*> spriteData;
        std::vector<int> spriteSizes;
        std::vector<char> names;
        for (std::size_t i = 0; i < sprites.size(); ++i)
        {
            spriteData.push_back(sprites[i].data());
            spriteSizes.push_back(static_cast<int>(sprites[i].size()));
            names.insert(names.end(), spriteNames[i].begin(), spriteNames[i].end());
        }
        const unsigned char* soundData[1] = {buzz.data()};
        const int soundSizes[1] = {static_cast<int>(buzz.size())};
        dgs_SetAssets(static_cast<unsigned int>(sprites.size()), reinterpret_cast<const char(*)[8]>(names.data()), spriteData.data(), spriteSizes.data(), 1,
                      reinterpret_cast<const char(*)[8]>(buzzName.data()), soundData, soundSizes);
        g_active = true;
        return true;
    }

    // ---- the wrappers for the host ----

    bool SpotOk(const double x, const double y, double& floorZ, double& ceilingZ)
    {
        return g_active && dgs_SpotOk(x, y, &floorZ, &ceilingZ) != 0;
    }

    int Spawn(const double x, const double y, const double z, const double angleDegrees, const int health, const bool requireSight)
    {
        return g_active ? dgs_Spawn(x, y, z, angleDegrees, health, requireSight ? 1 : 0) : -1;
    }

    bool Get(const int handle, Fly& out)
    {
        dgs_fly fly{};
        if (!g_active || dgs_Get(handle, &fly) == 0)
        {
            return false;
        }
        out.alive = fly.alive != 0;
        out.health = fly.health;
        out.x = fly.x;
        out.y = fly.y;
        out.z = fly.z;
        out.angle = fly.angle;
        out.momx = fly.momx;
        out.momy = fly.momy;
        out.momz = fly.momz;
        out.floorZ = fly.floorZ;
        out.ceilingZ = fly.ceilingZ;
        out.frame = fly.frame;
        return true;
    }

    void Drive(const int handle, const double angleDegrees, const double momx, const double momy, const double momz, const int frame)
    {
        if (g_active)
        {
            dgs_Drive(handle, angleDegrees, momx, momy, momz, frame);
        }
    }

    void Hurt(const int handle, const int damage)
    {
        if (g_active)
        {
            dgs_Hurt(handle, damage);
        }
    }

    void Fall(const int handle)
    {
        if (g_active)
        {
            dgs_Fall(handle);
        }
    }

    void Remove(const int handle)
    {
        if (g_active)
        {
            dgs_Remove(handle);
        }
    }

    void Reset()
    {
        if (g_active)
        {
            dgs_Reset();
        }
    }

    bool Sees(const int handle)
    {
        return g_active && dgs_Sees(handle) != 0;
    }

    double NavAngle(const int handle, const bool renew)
    {
        return g_active ? dgs_NavAngle(handle, renew ? 1 : 0) : 0.0;
    }

    void Bite(const int handle, const int damage)
    {
        if (g_active)
        {
            dgs_Bite(handle, damage);
        }
    }

    void Buzz(const int handle)
    {
        if (g_active)
        {
            dgs_Buzz(handle);
        }
    }

    int LevelTime()
    {
        return g_active ? dgs_LevelTime() : -1;
    }
}
