// The scene passes without a GPU (G7b): the bounds of a mesh, the lit and post shaders (reflection, root signature, the instance
// stream of the input layout) and the sampler presets. docs/render/scene-passes.md.

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderDescriptor.h>
#include <myengine/render/ShaderLayout.h>
#include <myengine/render/dx12/InputLayoutBuilder.h>
#include <myengine/render/dx12/RootSignatureBuilder.h>

#include "ShaderCompiler.h"
#include "ShaderReflector.h"

namespace
{
    using namespace myengine::render;
    using namespace myengine::render::dx12;
    namespace fs = std::filesystem;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Near(const float a, const float b) { return std::abs(a - b) < 1e-4f; }

    fs::path ShadersDirectory() { return fs::u8path(MYENGINE_SOURCE_DIR) / "assets" / "shaders"; }

    std::shared_ptr<const CompiledShaderProgram> CompileDescriptor(ShaderReflector& reflector, const std::string& name, ShaderProgramData& outProgram)
    {
        std::string error;
        Check(LoadShaderDescriptor(ShadersDirectory() / name, outProgram, error), name + ": the descriptor does not load: " + error);
        const ShaderCompileResult result = reflector.CompileProgram(outProgram);
        std::string messages;
        for (const ShaderDiagnostic& diagnostic : result.diagnostics)
        {
            messages += "\n  " + FormatShaderDiagnostic(diagnostic);
        }
        Check(result.ok && result.program != nullptr, name + " does not compile:" + messages);
        return result.program;
    }

    void TestMeshBounds()
    {
        MeshData empty;
        Check(!ComputeMeshBounds(empty).valid, "an empty mesh has no bounds");

        // A mesh far from its own origin: the bounds are around the vertices, not around (0, 0, 0)
        MeshData mesh;
        mesh.vertices.push_back({{10.0f, 0.0f, 0.0f}, {0, 1, 0}, {0, 0}});
        mesh.vertices.push_back({{12.0f, 2.0f, 0.0f}, {0, 1, 0}, {0, 0}});
        mesh.vertices.push_back({{11.0f, 1.0f, 1.0f}, {0, 1, 0}, {0, 0}});
        const MeshBounds bounds = ComputeMeshBounds(mesh);
        Check(bounds.valid, "bounds of a mesh");
        Check(Near(bounds.center.x, 11.0f) && Near(bounds.center.y, 1.0f) && Near(bounds.center.z, 0.5f), "the centre of the box");
        Check(Near(bounds.extents.x, 1.0f) && Near(bounds.extents.y, 1.0f) && Near(bounds.extents.z, 0.5f), "the half sizes");
        Check(Near(bounds.radius, 1.5f), "the sphere reaches the far vertex (12, 2, 0): " + std::to_string(bounds.radius));
        for (const MeshVertex& vertex : mesh.vertices)
        {
            const float dx = vertex.position.x - bounds.center.x;
            const float dy = vertex.position.y - bounds.center.y;
            const float dz = vertex.position.z - bounds.center.z;
            Check(std::sqrt(dx * dx + dy * dy + dz * dz) <= bounds.radius + 1e-4f, "every vertex is inside the sphere");
        }
    }

    void TestLitShader()
    {
        ShaderReflector reflector;
        ShaderProgramData program;
        const auto compiled = CompileDescriptor(reflector, "lit.shader.json", program);
        const ShaderLayout& layout = compiled->layout;

        // what the renderer writes by name
        const ShaderConstantBuffer* draw = layout.FindConstantBuffer("DrawCB");
        Check(draw != nullptr, "lit: DrawCB");
        const auto variable = [&](const char* name, const ShaderValueType type)
        {
            for (const ShaderVariable& candidate : draw->variables)
            {
                if (candidate.name == name)
                {
                    Check(candidate.type == type, std::string("lit: ") + name + " has the wrong type");
                    return;
                }
            }
            Check(false, std::string("lit: no variable ") + name);
        };
        variable("gViewProjection", ShaderValueType::Float4x4);
        variable("gLightViewProjection", ShaderValueType::Float4x4);
        for (const char* name : {"gCameraPosition", "gLightDirection", "gLightColor", "gAmbientSky", "gAmbientGround", "gShadowParams", "gColor", "gEmissiveColor"})
        {
            variable(name, ShaderValueType::Float4);
        }
        for (const char* name : {"gSpecular", "gGloss", "gFresnel", "gEmissiveStrength", "gAlphaCutoff", "gUnlit"})
        {
            variable(name, ShaderValueType::Float);
        }
        Check(layout.FindResourceByName("gTexture") != nullptr && layout.FindResourceByName("gShadowMap") != nullptr, "lit: the albedo and the shadow map");

        // the model matrix comes from the instance stream
        int instanceInputs = 0;
        for (const VertexInput& input : layout.vertexInputs)
        {
            if (input.semantic == "INSTANCE")
            {
                ++instanceInputs;
                Check(input.format == VertexFormat::Float4, "INSTANCE rows are float4");
            }
        }
        Check(instanceInputs == 4, "lit reads INSTANCE0..3");

        const VertexStreamDesc* stream = FindVertexStream(program.vertexStream);
        Check(stream != nullptr && stream->name == "mesh_instanced" && stream->instanceStride == kInstanceStride, "lit.shader.json names the instanced stream");
        const InputLayoutBuild build = BuildInputLayout(layout, *stream, "lit");
        Check(build.ok, "the input layout of lit builds");
        Check(build.instanceStride == 64 && build.stride == 32 && build.elements.size() == 7, "7 elements: 3 per vertex, 4 per instance");
        const std::vector<D3D12_INPUT_ELEMENT_DESC> elements = ToD3D12Elements(build);
        int perInstance = 0;
        for (const D3D12_INPUT_ELEMENT_DESC& element : elements)
        {
            if (element.InputSlot == 1)
            {
                ++perInstance;
                Check(element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA && element.InstanceDataStepRate == 1 &&
                        element.Format == DXGI_FORMAT_R32G32B32A32_FLOAT,
                    "instance elements: slot 1, per instance, float4");
            }
            else
            {
                Check(element.InputSlot == 0 && element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, "mesh elements: slot 0, per vertex");
            }
        }
        Check(perInstance == 4, "four per instance elements");

        // the plain mesh stream does not have them: an instanced program on it is an error that says so
        const InputLayoutBuild onMesh = BuildInputLayout(layout, *FindVertexStream("mesh"), "lit");
        Check(!onMesh.ok, "lit on the plain mesh stream does not build");

        // the root signature: the constants of lit are over 64 DWORD, so a root CBV; the samplers are the presets of the descriptor
        const RootSignatureBuild root = BuildRootSignatureDescription(layout, program.samplers, "lit", RootSignatureOptions{});
        Check(root.ok, "the root signature of lit builds");
        const ResourceBinding* binding = root.binding.Find("DrawCB");
        Check(binding != nullptr && !binding->rootConstants, "lit: DrawCB is a root CBV (over 64 DWORD)");
        bool anisotropic = false;
        bool shadow = false;
        for (const StaticSamplerDesc& sampler : root.description.samplers)
        {
            anisotropic |= sampler.name == "gSampler" && sampler.preset == SamplerPreset::AnisotropicWrap;
            shadow |= sampler.name == "gShadowSampler" && sampler.preset == SamplerPreset::ShadowCompare;
        }
        Check(anisotropic && shadow, "gSampler is anisotropic_wrap, gShadowSampler is shadow_compare");

        // the clamp variant of the program differs in the sampler only
        ShaderProgramData clamp;
        CompileDescriptor(reflector, "lit_clamp.shader.json", clamp);
        const RootSignatureBuild clampRoot = BuildRootSignatureDescription(reflector.CompileProgram(clamp).program->layout, clamp.samplers, "lit_clamp", RootSignatureOptions{});
        Check(clampRoot.ok && clampRoot.description.samplers[0].preset == SamplerPreset::AnisotropicClamp, "lit_clamp: anisotropic_clamp");
    }

    void TestPostShader()
    {
        ShaderReflector reflector;
        ShaderProgramData program;
        const auto compiled = CompileDescriptor(reflector, "post.shader.json", program);
        const ShaderLayout& layout = compiled->layout;
        Check(layout.vertexInputs.empty(), "post: no vertex inputs (SV_VertexID is not data)");
        const ShaderConstantBuffer* cb = layout.FindConstantBuffer("PostCB");
        Check(cb != nullptr && cb->variables.size() == 2, "post: PostCB with gPostParams and gTexelSize");
        Check(layout.FindResourceByName("gSource") != nullptr && layout.FindResourceByName("gBloom") != nullptr, "post: gSource and gBloom");
        const RootSignatureBuild root = BuildRootSignatureDescription(layout, program.samplers, "post", RootSignatureOptions{});
        Check(root.ok, "the root signature of post builds");
        const ResourceBinding* binding = root.binding.Find("PostCB");
        Check(binding != nullptr && binding->rootConstants, "post: small constants stay root constants");
        const InputLayoutBuild build = BuildInputLayout(layout, *FindVertexStream(program.vertexStream), "post");
        Check(build.ok && build.elements.empty(), "post: an empty input layout");
    }

    void TestSamplerPresets()
    {
        for (const char* name : {"linear_wrap", "linear_clamp", "point_wrap", "point_clamp", "anisotropic_wrap", "anisotropic_clamp", "shadow_compare"})
        {
            SamplerPreset preset{};
            Check(ParseSamplerPreset(name, preset) && ToString(preset) == name, std::string("the preset ") + name + " round-trips");
        }
        SamplerPreset unknown{};
        Check(!ParseSamplerPreset("anisotropic_mirror", unknown), "an unknown preset is rejected");
    }

    void TestDrawItemDefaults()
    {
        const DrawItem item;
        Check(item.blend == BlendMode::Opaque && !item.doubleSided && item.castShadow && item.boundsRadius == 0.0f, "a draw item is opaque, culled, a caster, without bounds");
        const RenderSettings settings;
        Check(settings.shadowQuality == 2 && settings.msaaSamples == 4 && settings.bloom && settings.instancing, "the default render settings are the high quality ones");
    }
}

int main()
{
    try
    {
        TestMeshBounds();
        TestLitShader();
        TestPostShader();
        TestSamplerPresets();
        TestDrawItemDefaults();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: " << exception.what() << "\n";
        return 1;
    }
    std::cout << "scene pass tests passed\n";
    return 0;
}
