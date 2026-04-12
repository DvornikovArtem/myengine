// RenderTypes.h

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <myengine/core/Types.h>

namespace myengine::render
{
    enum class PrimitiveType : std::uint8_t
    {
        Line = 0,
        Triangle = 1,
        Quad = 2,
        Cube = 3,
    };

    struct Float2
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Float3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct UByte4
    {
        std::uint8_t r = 255;
        std::uint8_t g = 255;
        std::uint8_t b = 255;
        std::uint8_t a = 255;
    };

    struct IntRect
    {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };

    struct RenderSurfaceHandle
    {
        std::uint32_t value = 0;

        bool IsValid() const
        {
            return value != 0;
        }
    };

    struct MeshHandle
    {
        std::uint32_t value = 0;

        bool IsValid() const
        {
            return value != 0;
        }
    };

    struct TextureHandle
    {
        std::uint32_t value = 0;

        bool IsValid() const
        {
            return value != 0;
        }
    };

    struct ShaderHandle
    {
        std::uint32_t value = 0;

        bool IsValid() const
        {
            return value != 0;
        }
    };

    struct Matrix4
    {
        std::array<float, 16> data{};

        static Matrix4 Identity()
        {
            Matrix4 matrix{};
            matrix.data[0] = 1.0f;
            matrix.data[5] = 1.0f;
            matrix.data[10] = 1.0f;
            matrix.data[15] = 1.0f;
            return matrix;
        }
    };

    struct MeshVertex
    {
        Float3 position{};
        Float3 normal{0.0f, 1.0f, 0.0f};
        Float2 uv{};
    };

    struct MeshData
    {
        std::vector<MeshVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    struct TextureData
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t channels = 4;
        bool srgb = true;
        std::vector<std::uint8_t> pixelsRgba8;
    };

    struct ShaderProgramData
    {
        std::filesystem::path sourcePath;
        std::string vertexEntry = "VSMain";
        std::string pixelEntry = "PSMain";
        std::string vertexProfile = "vs_5_0";
        std::string pixelProfile = "ps_5_0";
    };

    struct DrawItem
    {
        MeshHandle mesh;
        ShaderHandle shader;
        TextureHandle texture;
        Matrix4 model = Matrix4::Identity();
        core::Color color{1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct DebugLine
    {
        Float3 start{};
        Float3 end{};
        core::Color color{1.0f, 0.3f, 0.2f, 1.0f};
    };

    struct UiVertex
    {
        Float2 position{};
        UByte4 color{};
        Float2 uv{};
    };

    struct UiDrawData
    {
        const UiVertex* vertices = nullptr;
        std::size_t vertexCount = 0;
        const std::uint32_t* indices = nullptr;
        std::size_t indexCount = 0;
        TextureHandle texture;
        float translationX = 0.0f;
        float translationY = 0.0f;
        bool scissorEnabled = false;
        IntRect scissor{};
    };
}