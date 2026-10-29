// RenderTypes.h

#pragma once

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
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

    // An offscreen colour + depth target (IRenderAdapter::CreateRenderTarget)
    struct RenderTargetHandle
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

    // The bounds of a mesh in its own space: the axis aligned box and the sphere around its centre. The culling of the scene
    // takes them (scaled by the entity) instead of a fixed radius around the origin of the entity.
    struct MeshBounds
    {
        Float3 center{};
        Float3 extents{};      // half sizes of the box
        float radius = 0.0f;   // sphere around `center` that holds every vertex
        bool valid = false;
    };

    inline MeshBounds ComputeMeshBounds(const MeshData& data)
    {
        MeshBounds bounds;
        if (data.vertices.empty())
        {
            return bounds;
        }
        Float3 lo = data.vertices.front().position;
        Float3 hi = lo;
        for (const MeshVertex& vertex : data.vertices)
        {
            const Float3& p = vertex.position;
            lo = {p.x < lo.x ? p.x : lo.x, p.y < lo.y ? p.y : lo.y, p.z < lo.z ? p.z : lo.z};
            hi = {p.x > hi.x ? p.x : hi.x, p.y > hi.y ? p.y : hi.y, p.z > hi.z ? p.z : hi.z};
        }
        bounds.center = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        bounds.extents = {(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
        float radiusSquared = 0.0f;
        for (const MeshVertex& vertex : data.vertices)
        {
            const float dx = vertex.position.x - bounds.center.x;
            const float dy = vertex.position.y - bounds.center.y;
            const float dz = vertex.position.z - bounds.center.z;
            const float d = dx * dx + dy * dy + dz * dz;
            radiusSquared = d > radiusSquared ? d : radiusSquared;
        }
        bounds.radius = std::sqrt(radiusSquared);
        bounds.valid = true;
        return bounds;
    }

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
        std::vector<std::pair<std::string, std::string>> defines; // #define name value for both entry points
        // The states of the static samplers, by the name of the sampler in the shader: "gSampler" -> "linear_wrap".
        // A sampler that is not named here is linear, wrap.
        std::vector<std::pair<std::string, std::string>> samplers;
        // The vertex stream that the program reads: the table of the formats and offsets of the vertex buffer it is drawn
        // with ("mesh", "debug_lines", "ui"). The input layout is built from it and from the inputs of the vertex shader.
        std::string vertexStream = "mesh";
    };

    // How a material is drawn: opaque (the depth pass of the scene), alpha blended (back to front after the opaque ones, no depth
    // write) or additive (after them, no depth write, order does not matter: flames, glows, orbs)
    enum class BlendMode : std::uint8_t
    {
        Opaque = 0,
        Alpha = 1,
        Additive = 2,
    };

    class MaterialParams;

    struct DrawItem
    {
        MeshHandle mesh;
        ShaderHandle shader;
        // The parameters of the material: the bytes of the constant buffers and all the textures of the shader by name
        // (gColor and the other values of the material come from here). Not owned: the resource manager keeps it alive
        // for the frame that asked for it (ResourceManager::GetMaterialParams). The renderer writes the engine
        // semantics (gModel, gViewProjection, gLightDirection) over it.
        const MaterialParams* material = nullptr;
        // Without a `material` only: the texture of the first texture slot of the shader and the value of gColor
        TextureHandle texture;
        Matrix4 model = Matrix4::Identity();
        core::Color color{1.0f, 1.0f, 1.0f, 1.0f};
        // From the material ("blend", "doubleSided", "castShadow" of the .material.json)
        BlendMode blend = BlendMode::Opaque;
        bool doubleSided = false;   // no back face culling (nets, flames, thin panels)
        bool castShadow = true;     // drawn into the shadow map
        // The world sphere of the draw (the culling sphere of the entity): the shadow map is fitted to the casters, the transparent
        // draws are sorted by its centre. radius 0: unknown, the translation of `model` is used.
        Float3 boundsCenter{};
        float boundsRadius = 0.0f;
    };

    struct DebugLine
    {
        Float3 start{};
        Float3 end{};
        core::Color color{1.0f, 0.3f, 0.2f, 1.0f};
    };

    // A cloud of glowing points that all look alike and flash over time (IRenderAdapter::CreatePointCloud, docs/flybrain/brain-view.md): the positions
    // go to the GPU once, only one float per point (when it flashed last) goes there every frame
    struct PointCloudHandle
    {
        std::uint32_t value = 0;

        bool IsValid() const
        {
            return value != 0;
        }
    };

    struct PointCloudData
    {
        std::vector<float> positions;          // x, y, z of every point (3 floats each)
        std::vector<std::uint8_t> colorIndex;  // the row of PointCloudDrawParams::palette of every point (0..15); empty: all 0
        std::uint32_t Count() const { return static_cast<std::uint32_t>(positions.size() / 3); }
    };

    // One draw of a cloud into the render target that is being rendered (between BeginRenderTarget and EndRenderTarget)
    struct PointCloudDrawParams
    {
        Matrix4 view = Matrix4::Identity();
        Matrix4 projection = Matrix4::Identity();
        const float* flashTimes = nullptr;     // one float per point: the time of its last flash, seconds (any clock, the same as `time`); read during the call
        float time = 0.0f;                     // now
        float decaySeconds = 0.15f;            // a flash fades to 1/e in this time
        float baseBrightness = 0.04f;          // a point at rest (HDR: above 1 glows)
        float flashBrightness = 5.0f;          // added by a point that flashes just now
        float baseSizePixels = 1.6f;
        float flashSizePixels = 3.0f;          // added by a flash, in pixels of the target
        float referenceDepth = 3.0f;           // distance of the camera at which the point size is as given; nearer ones are bigger
        float depthRange = 1.5f;               // points this much behind the reference depth are dimmed to a quarter
        float palette[16][4] = {};             // the colour of each colour index (rgb; a is not used)
        bool bloom = true;
        float bloomThreshold = 0.6f;
        float bloomIntensity = 1.0f;
        float exposure = 1.0f;                 // the HDR picture goes to the 8 bit target as 1 - exp(-x * exposure)
        core::Color background{0.0f, 0.0f, 0.0f, 1.0f};
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