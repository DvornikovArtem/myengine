// The root signature builder without a GPU: the rules that turn a ShaderLayout into root parameters, static samplers and
// a ProgramBinding. D3D12SerializeRootSignature needs no device, so every successful build is also serialized.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderDescriptor.h>
#include <myengine/render/ShaderLayout.h>
#include <myengine/render/dx12/RootSignatureBuilder.h>

namespace
{
    using namespace myengine::render;
    using namespace myengine::render::dx12;
    namespace fs = std::filesystem;

    constexpr std::uint8_t kVs = static_cast<std::uint8_t>(ShaderStage::Vertex);
    constexpr std::uint8_t kPs = static_cast<std::uint8_t>(ShaderStage::Pixel);

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    using Samplers = std::vector<std::pair<std::string, std::string>>;

    ShaderResource Resource(const std::string& name, const ShaderResourceType type, const std::uint32_t registerIndex, const std::uint8_t stages, const std::uint32_t bindCount = 1)
    {
        ShaderResource resource;
        resource.name = name;
        resource.type = type;
        resource.registerIndex = registerIndex;
        resource.stages = stages;
        resource.bindCount = bindCount;
        return resource;
    }

    // A constant buffer of the given size: the resource and its description
    void AddBuffer(ShaderLayout& layout, const std::string& name, const std::uint32_t registerIndex, const std::uint32_t sizeBytes, const std::uint8_t stages)
    {
        layout.resources.push_back(Resource(name, ShaderResourceType::ConstantBuffer, registerIndex, stages));
        ShaderConstantBuffer buffer;
        buffer.name = name;
        buffer.registerIndex = registerIndex;
        buffer.sizeBytes = sizeBytes;
        buffer.stages = stages;
        layout.constantBuffers.push_back(std::move(buffer));
    }

    void AddInput(ShaderLayout& layout)
    {
        VertexInput input;
        input.semantic = "POSITION";
        input.format = VertexFormat::Float3;
        layout.vertexInputs.push_back(input);
    }

    // The scene program as the reflection gives it: DrawCB b0 (160 bytes, both stages), gTexture t0, gSampler s0 (pixel)
    ShaderLayout SceneLayout()
    {
        ShaderLayout layout;
        AddBuffer(layout, "DrawCB", 0, 160, kVs | kPs);
        layout.resources.push_back(Resource("gTexture", ShaderResourceType::Texture, 0, kPs));
        layout.resources.push_back(Resource("gSampler", ShaderResourceType::Sampler, 0, kPs));
        AddInput(layout);
        return layout;
    }

    void CheckSerializes(const RootSignatureBuild& build, const std::string& what)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        std::string error;
        Check(SerializeRootSignature(build.description, blob, error), what + ": serialization failed: " + error);
        Check(blob != nullptr && blob->GetBufferSize() > 0, what + ": empty blob");
    }

    std::string Diagnostics(const RootSignatureBuild& build)
    {
        std::string text;
        for (const ShaderDiagnostic& diagnostic : build.diagnostics)
        {
            text += "\n  " + FormatShaderDiagnostic(diagnostic);
        }
        return text;
    }

    bool HasDiagnostic(const RootSignatureBuild& build, const std::string& text, const bool warning)
    {
        for (const ShaderDiagnostic& diagnostic : build.diagnostics)
        {
            if (diagnostic.warning == warning && diagnostic.message.find(text) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // ---- the rules for the scene program ----

    void TestSceneProgram()
    {
        const RootSignatureBuild build = BuildRootSignatureDescription(SceneLayout(), {}, "scene");
        Check(build.ok, "the scene layout builds:" + Diagnostics(build));
        const RootSignatureDescription& description = build.description;

        Check(description.parameters.size() == 2, "two root parameters (the constants and the texture table)");
        const RootParameterDesc& constants = description.parameters[0];
        Check(constants.kind == RootParameterKind::Constants && constants.registerIndex == 0 && constants.dwords == 40 &&
                constants.visibility == RootVisibility::All && constants.name == "DrawCB",
            "DrawCB is 40 root constants at b0 for both stages");
        const RootParameterDesc& table = description.parameters[1];
        Check(table.kind == RootParameterKind::TextureTable && table.registerIndex == 0 && table.dwords == 1 &&
                table.visibility == RootVisibility::Pixel && table.name == "gTexture",
            "gTexture is a table with t0 for the pixel stage");
        Check(description.samplers.size() == 1 && description.samplers[0].registerIndex == 0 &&
                description.samplers[0].preset == SamplerPreset::LinearWrap && description.samplers[0].visibility == RootVisibility::Pixel,
            "gSampler is s0, linear wrap by default, pixel stage");
        Check(description.allowInputAssembler, "a vertex shader with inputs gets the input assembler flag");
        Check(description.totalDwords == 41, "40 + 1 DWORD, not " + std::to_string(description.totalDwords));

        const ResourceBinding* drawCb = build.binding.Find("DrawCB");
        Check(drawCb != nullptr && drawCb->kind == BindingKind::ConstantBuffer && drawCb->rootParameter == 0 && drawCb->rootConstants &&
                drawCb->sizeBytes == 160 && drawCb->sizeDwords == 40,
            "the binding of DrawCB");
        const ResourceBinding* texture = build.binding.Find("gTexture");
        Check(texture != nullptr && texture->kind == BindingKind::Texture && texture->rootParameter == 1, "the binding of gTexture");
        const ResourceBinding* sampler = build.binding.Find("gSampler");
        Check(sampler != nullptr && sampler->kind == BindingKind::Sampler && sampler->rootParameter == kNoRootParameter, "a static sampler has no root parameter");
        Check(build.binding.Find("gNothing") == nullptr, "an unknown name has no binding");

        CheckSerializes(build, "scene");
    }

    void TestUiAndDebugPrograms()
    {
        ShaderLayout ui;
        AddBuffer(ui, "UiConstants", 0, 16, kVs);
        ui.resources.push_back(Resource("uiTexture", ShaderResourceType::Texture, 0, kPs));
        ui.resources.push_back(Resource("uiSampler", ShaderResourceType::Sampler, 0, kPs));
        AddInput(ui);
        const RootSignatureBuild uiBuild = BuildRootSignatureDescription(ui, {{"uiSampler", "linear_clamp"}}, "ui");
        Check(uiBuild.ok, "the UI layout builds:" + Diagnostics(uiBuild));
        Check(uiBuild.description.parameters.size() == 2 && uiBuild.description.parameters[0].dwords == 4 &&
                uiBuild.description.parameters[0].visibility == RootVisibility::Vertex,
            "UiConstants: 4 root constants for the vertex stage");
        Check(uiBuild.description.samplers.size() == 1 && uiBuild.description.samplers[0].preset == SamplerPreset::LinearClamp,
            "the sampler state comes from the descriptor: linear_clamp");
        CheckSerializes(uiBuild, "ui");

        // The debug lines have no resources at all: an empty signature with the input assembler flag
        ShaderLayout debug;
        AddInput(debug);
        const RootSignatureBuild debugBuild = BuildRootSignatureDescription(debug, {}, "debug");
        Check(debugBuild.ok && debugBuild.description.parameters.empty() && debugBuild.description.samplers.empty() &&
                debugBuild.description.allowInputAssembler && debugBuild.description.totalDwords == 0,
            "debug lines: an empty root signature");
        CheckSerializes(debugBuild, "debug");

        // No vertex inputs (a pixel-only test shader): no flag
        const RootSignatureBuild noInputs = BuildRootSignatureDescription(ShaderLayout{}, {}, "empty");
        Check(noInputs.ok && !noInputs.description.allowInputAssembler, "no inputs, no input assembler flag");
        CheckSerializes(noInputs, "empty");
    }

    // ---- constants or CBVs: the threshold of 64 DWORD ----

    void TestConstantsThreshold()
    {
        // 64 DWORD together: root constants
        ShaderLayout fits;
        AddBuffer(fits, "A", 0, 128, kVs | kPs);
        AddBuffer(fits, "B", 1, 128, kVs | kPs);
        const RootSignatureBuild fitsBuild = BuildRootSignatureDescription(fits, {}, "fits");
        Check(fitsBuild.ok, "64 DWORD of cbuffers build:" + Diagnostics(fitsBuild));
        Check(fitsBuild.description.parameters.size() == 2 && fitsBuild.description.parameters[0].kind == RootParameterKind::Constants &&
                fitsBuild.description.parameters[1].kind == RootParameterKind::Constants && fitsBuild.description.totalDwords == 64,
            "two cbuffers of 32 DWORD are root constants, one parameter each");
        Check(fitsBuild.binding.Find("B")->rootParameter == 1 && fitsBuild.binding.Find("B")->rootConstants, "the binding of B");
        CheckSerializes(fitsBuild, "64 DWORD");

        // 65 DWORD together: root CBVs for every cbuffer
        ShaderLayout over;
        AddBuffer(over, "A", 0, 128, kVs | kPs);
        AddBuffer(over, "B", 1, 132, kVs | kPs);
        const RootSignatureBuild overBuild = BuildRootSignatureDescription(over, {}, "over");
        Check(overBuild.ok, "65 DWORD of cbuffers build:" + Diagnostics(overBuild));
        Check(overBuild.description.parameters.size() == 2 && overBuild.description.parameters[0].kind == RootParameterKind::ConstantBufferView &&
                overBuild.description.parameters[1].kind == RootParameterKind::ConstantBufferView && overBuild.description.totalDwords == 4,
            "above 64 DWORD the cbuffers are root CBVs (2 DWORD each)");
        Check(!overBuild.binding.Find("A")->rootConstants && overBuild.binding.Find("B")->sizeBytes == 132 && overBuild.binding.Find("B")->sizeDwords == 33,
            "the binding of a root CBV keeps the size of the data");
        CheckSerializes(overBuild, "65 DWORD");

        // The option moves the threshold (the engine's switch MYENGINE_DX12_ROOT_CBV sets it to 0)
        RootSignatureOptions alwaysCbv;
        alwaysCbv.rootConstantsLimitDwords = 0;
        const RootSignatureBuild forced = BuildRootSignatureDescription(SceneLayout(), {}, "forced", alwaysCbv);
        Check(forced.ok && forced.description.parameters[0].kind == RootParameterKind::ConstantBufferView && !forced.binding.Find("DrawCB")->rootConstants,
            "limit 0 makes every cbuffer a root CBV");
        CheckSerializes(forced, "forced CBV");

        // A cbuffer is rounded up to whole DWORD
        ShaderLayout odd;
        AddBuffer(odd, "Odd", 0, 18, kPs);
        const RootSignatureBuild oddBuild = BuildRootSignatureDescription(odd, {}, "odd");
        Check(oddBuild.ok && oddBuild.description.parameters[0].dwords == 5, "18 bytes are 5 DWORD");
    }

    // ---- visibility by the stages of the layout ----

    void TestVisibility()
    {
        ShaderLayout layout;
        AddBuffer(layout, "VertexOnly", 0, 16, kVs);
        AddBuffer(layout, "PixelOnly", 1, 16, kPs);
        AddBuffer(layout, "Both", 2, 16, kVs | kPs);
        layout.resources.push_back(Resource("gVertexTexture", ShaderResourceType::Texture, 0, kVs));
        layout.resources.push_back(Resource("gPixelTexture", ShaderResourceType::Texture, 1, kPs));
        layout.resources.push_back(Resource("gBothTexture", ShaderResourceType::Texture, 2, kVs | kPs));
        layout.resources.push_back(Resource("gSampler", ShaderResourceType::Sampler, 0, kVs | kPs));
        const RootSignatureBuild build = BuildRootSignatureDescription(layout, {}, "visibility");
        Check(build.ok, "the layout builds:" + Diagnostics(build));
        const auto& parameters = build.description.parameters;
        Check(parameters.size() == 6, "three cbuffers and three tables");
        Check(parameters[0].visibility == RootVisibility::Vertex && parameters[1].visibility == RootVisibility::Pixel && parameters[2].visibility == RootVisibility::All,
            "cbuffers: visibility from the stages");
        Check(parameters[3].visibility == RootVisibility::Vertex && parameters[4].visibility == RootVisibility::Pixel && parameters[5].visibility == RootVisibility::All,
            "textures: visibility from the stages");
        Check(parameters[4].registerIndex == 1 && parameters[5].registerIndex == 2, "textures keep their registers");
        Check(build.description.samplers[0].visibility == RootVisibility::All, "a sampler of both stages is visible to all");
        CheckSerializes(build, "visibility");
    }

    // ---- samplers: the descriptor, the default, mistakes ----

    void TestSamplers()
    {
        ShaderLayout layout = SceneLayout();
        layout.resources.push_back(Resource("gDetailSampler", ShaderResourceType::Sampler, 1, kPs));

        const RootSignatureBuild build = BuildRootSignatureDescription(layout, {{"gDetailSampler", "point_clamp"}}, "samplers");
        Check(build.ok, "samplers build:" + Diagnostics(build));
        Check(build.description.samplers.size() == 2, "two samplers");
        Check(build.description.samplers[0].name == "gSampler" && build.description.samplers[0].preset == SamplerPreset::LinearWrap, "a sampler that the descriptor does not name is linear wrap");
        Check(build.description.samplers[1].name == "gDetailSampler" && build.description.samplers[1].registerIndex == 1 &&
                build.description.samplers[1].preset == SamplerPreset::PointClamp,
            "a named sampler gets its state at its register");
        CheckSerializes(build, "two samplers");

        for (const char* name : {"linear_wrap", "linear_clamp", "point_wrap", "point_clamp", "anisotropic_wrap"})
        {
            SamplerPreset preset{};
            Check(ParseSamplerPreset(name, preset) && ToString(preset) == name, std::string("the preset ") + name + " round-trips");
        }
        SamplerPreset ignored{};
        Check(!ParseSamplerPreset("trilinear", ignored), "an unknown state name is not parsed");

        const RootSignatureBuild unknown = BuildRootSignatureDescription(layout, {{"gSampler", "trilinear"}}, "unknown");
        Check(!unknown.ok && HasDiagnostic(unknown, "unknown state 'trilinear'", false) && HasDiagnostic(unknown, "gSampler", false),
            "an unknown sampler state is an error that names the sampler:" + Diagnostics(unknown));

        const RootSignatureBuild stale = BuildRootSignatureDescription(layout, {{"gGone", "point_wrap"}}, "stale");
        Check(stale.ok && HasDiagnostic(stale, "'gGone'", true), "a sampler of the descriptor that the shader does not have is a warning:" + Diagnostics(stale));
    }

    // ---- what the builder does not support yet: a diagnostic, not a silent omission ----

    void TestUnsupportedResources()
    {
        ShaderLayout uav = SceneLayout();
        uav.resources.push_back(Resource("gOutput", ShaderResourceType::RWResource, 1, kPs));
        const RootSignatureBuild uavBuild = BuildRootSignatureDescription(uav, {}, "uav");
        Check(!uavBuild.ok && HasDiagnostic(uavBuild, "'gOutput'", false) && HasDiagnostic(uavBuild, "UAV", false) && HasDiagnostic(uavBuild, "u1", false),
            "a UAV is an error with its name and register:" + Diagnostics(uavBuild));
        Check(uavBuild.description.parameters.empty() && uavBuild.binding.bindings.empty(), "nothing is built for an unsupported layout");

        ShaderLayout buffer = SceneLayout();
        buffer.resources.push_back(Resource("gItems", ShaderResourceType::Buffer, 3, kPs));
        const RootSignatureBuild bufferBuild = BuildRootSignatureDescription(buffer, {}, "buffer");
        Check(!bufferBuild.ok && HasDiagnostic(bufferBuild, "'gItems'", false) && HasDiagnostic(bufferBuild, "buffer", false) && HasDiagnostic(bufferBuild, "t3", false),
            "a buffer SRV is an error with its name and register:" + Diagnostics(bufferBuild));

        ShaderLayout array = SceneLayout();
        array.resources.push_back(Resource("gLayers", ShaderResourceType::Texture, 4, kPs, 3));
        const RootSignatureBuild arrayBuild = BuildRootSignatureDescription(array, {}, "array");
        Check(!arrayBuild.ok && HasDiagnostic(arrayBuild, "'gLayers'", false) && HasDiagnostic(arrayBuild, "array of 3", false),
            "an array of resources is an error with its name and size:" + Diagnostics(arrayBuild));
        Check(arrayBuild.diagnostics[0].file == "array", "the diagnostic names the program");
    }

    void TestLimitOfTheSignature()
    {
        // 70 textures: 70 DWORD of descriptor tables
        ShaderLayout layout;
        for (std::uint32_t i = 0; i < 70; ++i)
        {
            layout.resources.push_back(Resource("gTexture" + std::to_string(i), ShaderResourceType::Texture, i, kPs));
        }
        const RootSignatureBuild build = BuildRootSignatureDescription(layout, {}, "too_big");
        Check(!build.ok && HasDiagnostic(build, "70 DWORD", false) && HasDiagnostic(build, "limit is 64", false),
            "more than 64 DWORD is an error that says how much:" + Diagnostics(build));

        // Exactly 64 is fine
        layout.resources.resize(64);
        const RootSignatureBuild exact = BuildRootSignatureDescription(layout, {}, "exact");
        Check(exact.ok && exact.description.totalDwords == 64, "64 tables fit");
    }

    // ---- the shape: the same structure, the same hash ----

    void TestShape()
    {
        const RootSignatureBuild scene = BuildRootSignatureDescription(SceneLayout(), {}, "scene");

        // The same structure under other names: the same shape
        ShaderLayout renamed;
        AddBuffer(renamed, "OtherCB", 0, 160, kVs | kPs);
        renamed.resources.push_back(Resource("gAlbedo", ShaderResourceType::Texture, 0, kPs));
        renamed.resources.push_back(Resource("gLinear", ShaderResourceType::Sampler, 0, kPs));
        AddInput(renamed);
        const RootSignatureBuild other = BuildRootSignatureDescription(renamed, {}, "other");
        Check(scene.description.shapeHash == other.description.shapeHash, "the same structure under other names has the same hash");

        // Anything that changes the signature changes the hash
        const auto hashOf = [](const ShaderLayout& layout, const Samplers& samplers)
        {
            return BuildRootSignatureDescription(layout, samplers, "x").description.shapeHash;
        };
        ShaderLayout biggerBuffer = SceneLayout();
        biggerBuffer.constantBuffers[0].sizeBytes = 176;
        ShaderLayout otherRegister = SceneLayout();
        otherRegister.resources[1].registerIndex = 1;
        ShaderLayout otherStages = SceneLayout();
        otherStages.resources[1].stages = kVs | kPs;
        ShaderLayout noInput = SceneLayout();
        noInput.vertexInputs.clear();
        const std::uint64_t reference = scene.description.shapeHash;
        Check(hashOf(biggerBuffer, {}) != reference, "another cbuffer size: another shape");
        Check(hashOf(otherRegister, {}) != reference, "another texture register: another shape");
        Check(hashOf(otherStages, {}) != reference, "other stages: another shape");
        Check(hashOf(noInput, {}) != reference, "no input assembler flag: another shape");
        Check(hashOf(SceneLayout(), {{"gSampler", "point_wrap"}}) != reference, "another sampler state: another shape");
        Check(hashOf(SceneLayout(), {{"gSampler", "linear_wrap"}}) == reference, "the default sampler state named explicitly is the same shape");
        Check(!FormatRootSignature(scene.description).empty() && FormatRootSignature(scene.description).find("DrawCB") != std::string::npos, "the text of the root signature names the cbuffer");
    }

    // ---- the descriptor file: samplers as data ----

    void TestDescriptors()
    {
        const fs::path shaders = fs::u8path(MYENGINE_SOURCE_DIR) / "assets/shaders";

        ShaderProgramData scene;
        std::string error;
        Check(LoadShaderDescriptor(shaders / "textured_lit.shader.json", scene, error), "textured_lit.shader.json loads: " + error);
        Check(scene.sourcePath.filename() == "textured_lit.hlsl" && scene.vertexEntry == "VSMain" && scene.pixelEntry == "PSMain", "the entry points of the scene program");
        Check(scene.samplers.size() == 1 && scene.samplers[0].first == "gSampler" && scene.samplers[0].second == "linear_wrap", "the scene sampler is linear_wrap");

        ShaderProgramData ui;
        Check(LoadShaderDescriptor(shaders / "ui_overlay.shader.json", ui, error), "ui_overlay.shader.json loads: " + error);
        Check(ui.samplers.size() == 1 && ui.samplers[0].first == "uiSampler" && ui.samplers[0].second == "linear_clamp", "the UI sampler is linear_clamp");

        ShaderProgramData debug;
        Check(LoadShaderDescriptor(shaders / "debug_line.shader.json", debug, error), "debug_line.shader.json loads: " + error);
        Check(debug.samplers.empty() && debug.sourcePath.filename() == "debug_line.hlsl", "debug_line has no samplers");

        const fs::path directory = fs::temp_directory_path() / "myengine_root_signature_test";
        fs::create_directories(directory);
        const auto write = [&directory](const std::string& name, const std::string& text)
        {
            const fs::path path = directory / name;
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << text;
            return path;
        };
        ShaderProgramData broken;
        Check(!LoadShaderDescriptor(write("no_source.shader.json", "{}"), broken, error) && error.find("source") != std::string::npos, "no source: an error");
        Check(!LoadShaderDescriptor(write("bad_samplers.shader.json", R"({"source":"a.hlsl","samplers":[1]})"), broken, error) && error.find("samplers") != std::string::npos,
            "samplers that is not an object: an error");
        Check(!LoadShaderDescriptor(write("bad_state.shader.json", R"({"source":"a.hlsl","samplers":{"gSampler":3}})"), broken, error) && error.find("gSampler") != std::string::npos,
            "a state that is not a string: an error");
        Check(!LoadShaderDescriptor(write("not_json.shader.json", "{ nope"), broken, error) && !error.empty(), "broken json: an error");
        Check(!LoadShaderDescriptor(directory / "missing.shader.json", broken, error), "a missing file: an error");
        Check(broken.sourcePath.empty(), "a failed load leaves the program untouched");
        std::error_code ec;
        fs::remove_all(directory, ec);
    }
}

int main()
{
    struct Case
    {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"scene program", TestSceneProgram},
        {"ui and debug programs", TestUiAndDebugPrograms},
        {"constants or CBV threshold", TestConstantsThreshold},
        {"visibility by stages", TestVisibility},
        {"samplers", TestSamplers},
        {"unsupported resources", TestUnsupportedResources},
        {"64 DWORD limit", TestLimitOfTheSignature},
        {"shape hash", TestShape},
        {"shader descriptors", TestDescriptors},
    };

    int failed = 0;
    for (const Case& test : cases)
    {
        try
        {
            test.run();
            std::cout << "[ OK ] " << test.name << "\n";
        }
        catch (const std::exception& ex)
        {
            ++failed;
            std::cout << "[FAIL] " << test.name << ": " << ex.what() << "\n";
        }
    }
    std::cout << (failed == 0 ? "root_signature: all passed\n" : "root_signature: FAILED\n");
    return failed == 0 ? 0 : 1;
}
