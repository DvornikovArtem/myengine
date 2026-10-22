// B1.1 (LR3): the parameters of a material by the names of its shader, without a GPU.
// The layouts come from the reflection of real HLSL (D3DCompile / D3DReflect need no device), the materials from the
// resource manager on a recording adapter.

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <myengine/core/Logger.h>
#include <myengine/jobs/JobSystem.h>
#include <myengine/render/IRenderAdapter.h>
#include <myengine/render/MaterialParams.h>
#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderLayout.h>
#include <myengine/resource/ResourceManager.h>

#include "ShaderReflector.h"

namespace
{
    namespace fs = std::filesystem;
    using namespace myengine;
    using namespace myengine::render;
    using json = nlohmann::json;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool Contains(const std::string& text, const std::string& part)
    {
        return text.find(part) != std::string::npos;
    }

    // ---- layouts ----

    ShaderProgramData Entries()
    {
        ShaderProgramData program;
        program.vertexEntry = "VSMain";
        program.pixelEntry = "PSMain";
        return program;
    }

    // A shader with every kind of variable a material can set, and three textures declared out of order of their registers
    const char* const kWideShader = R"(
cbuffer DrawCB : register(b0)
{
    float4x4 gModel;
    float4x4 gViewProjection;
    float4 gColor;
    float4 gLightDirection;
    float gRoughness;
    float2 gUvScale;
    float3 gTint3;
    int gMode;
    uint gCount;
    bool gFlag;
    float4x4 gExtra;
    float gWeights[2];
};
Texture2D gDetail : register(t1);
Texture2D gTexture : register(t0);
Texture2D gMask : register(t2);
SamplerState gSampler : register(s0);
float4 VSMain(float3 position : POSITION) : SV_POSITION { return mul(mul(float4(position, 1), gModel), gViewProjection); }
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET
{
    float4 sum = gColor * gRoughness * gUvScale.x + gTexture.Sample(gSampler, position.xy) + gDetail.Sample(gSampler, position.xy) + gMask.Sample(gSampler, position.xy);
    sum.xyz += gTint3 + gLightDirection.xyz + gExtra[0].xyz + gWeights[1] + gMode + gCount + (gFlag ? 1.0f : 0.0f);
    return sum;
}
)";

    // A shader without gColor and without textures
    const char* const kPlainShader = R"(
cbuffer Frame : register(b0) { float4 gTint; float gStrength; };
float4 VSMain(float3 position : POSITION) : SV_POSITION { return float4(position, 1) * gTint; }
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return gTint * gStrength; }
)";

    ShaderLayout LayoutOf(const std::string& name, const std::string& source)
    {
        static dx12::ShaderReflector reflector;
        const dx12::ShaderCompileResult result = reflector.CompileProgramFromSource(name, source, Entries());
        std::string messages;
        for (const ShaderDiagnostic& diagnostic : result.diagnostics)
        {
            messages += "\n  " + FormatShaderDiagnostic(diagnostic);
        }
        Check(result.ok && result.program != nullptr, "the shader '" + name + "' did not compile:" + messages);
        return result.program->layout;
    }

    fs::path SourceDirectory()
    {
        return fs::u8path(MYENGINE_SOURCE_DIR);
    }

    // The shader of the engine that all the materials of the project use
    ShaderLayout TexturedLitLayout()
    {
        static dx12::ShaderReflector reflector;
        ShaderProgramData program = Entries();
        program.sourcePath = SourceDirectory() / "assets/shaders/textured_lit.hlsl";
        const dx12::ShaderCompileResult result = reflector.CompileProgram(program);
        Check(result.ok && result.program != nullptr, "textured_lit.hlsl compiles");
        return result.program->layout;
    }

    // ---- the bytes of a constant buffer ----

    const ShaderVariable& Variable(const MaterialParams& params, const std::string& name)
    {
        const ShaderVariableRef found = params.Layout().FindVariable(name);
        Check(found.variable != nullptr, "the layout has no variable '" + name + "':\n" + FormatShaderLayout(params.Layout()));
        return *found.variable;
    }

    const std::uint8_t* BytesOf(const MaterialParams& params, const std::string& name)
    {
        const ShaderVariableRef found = params.Layout().FindVariable(name);
        Check(found.variable != nullptr, "the layout has no variable '" + name + "'");
        const std::size_t index = static_cast<std::size_t>(found.buffer - params.Layout().constantBuffers.data());
        return params.ConstantBufferBytes(index).data() + found.variable->offset;
    }

    template <class T>
    T ReadAt(const MaterialParams& params, const std::string& name, const std::size_t element = 0)
    {
        T value;
        std::memcpy(&value, BytesOf(params, name) + element * sizeof(T), sizeof(T));
        return value;
    }

    bool AllZero(const std::vector<std::uint8_t>& bytes)
    {
        for (const std::uint8_t byte : bytes)
        {
            if (byte != 0)
            {
                return false;
            }
        }
        return true;
    }

    void CheckOk(const MaterialParamResult& result, const std::string& what)
    {
        Check(result.IsOk(), what + ": " + result.message);
    }

    void CheckError(const MaterialParamResult& result, const std::vector<std::string>& parts, const std::string& what)
    {
        Check(result.IsError(), what + ": an error is expected");
        for (const std::string& part : parts)
        {
            Check(Contains(result.message, part), what + ": the message lacks '" + part + "': " + result.message);
        }
    }

    // ---- setters, bytes, errors ----

    void TestSettersWriteBytesAtTheOffsets()
    {
        MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
        Check(params.ConstantBufferCount() == 1, "one constant buffer");
        Check(params.ConstantBufferBytes(0).size() == params.Layout().constantBuffers[0].sizeBytes, "the shadow copy has the size of the buffer");
        Check(AllZero(params.ConstantBufferBytes(0)), "nothing set: zero");

        // textured_lit is the contract: gColor sits at 128, the offsets come from the layout
        CheckOk(params.SetFloat4("gColor", 1.0f, 0.75f, 0.12f, 0.5f), "SetFloat4");
        Check(Variable(params, "gColor").offset == 128, "gColor at 128");
        Check(ReadAt<float>(params, "gColor", 0) == 1.0f && ReadAt<float>(params, "gColor", 1) == 0.75f &&
              ReadAt<float>(params, "gColor", 2) == 0.12f && ReadAt<float>(params, "gColor", 3) == 0.5f, "gColor: four floats");

        CheckOk(params.SetFloat("gRoughness", 0.4f), "SetFloat");
        Check(ReadAt<float>(params, "gRoughness") == 0.4f, "gRoughness");
        CheckOk(params.SetFloat2("gUvScale", 2.0f, 3.0f), "SetFloat2");
        Check(ReadAt<float>(params, "gUvScale", 0) == 2.0f && ReadAt<float>(params, "gUvScale", 1) == 3.0f, "gUvScale");
        CheckOk(params.SetFloat3("gTint3", 0.1f, 0.2f, 0.3f), "SetFloat3");
        Check(ReadAt<float>(params, "gTint3", 0) == 0.1f && ReadAt<float>(params, "gTint3", 2) == 0.3f, "gTint3");
        CheckOk(params.SetInt("gMode", -7), "SetInt");
        Check(ReadAt<std::int32_t>(params, "gMode") == -7, "gMode");
        CheckOk(params.SetUInt("gCount", 4000000000u), "SetUInt");
        Check(ReadAt<std::uint32_t>(params, "gCount") == 4000000000u, "gCount");
        CheckOk(params.SetBool("gFlag", true), "SetBool");
        Check(ReadAt<std::uint32_t>(params, "gFlag") == 1u, "a bool is a DWORD with 1");
        CheckOk(params.SetBool("gFlag", false), "SetBool false");
        Check(ReadAt<std::uint32_t>(params, "gFlag") == 0u, "false is 0");

        // A matrix is given by rows and stored by columns, like gModel is by the renderer
        std::array<float, 16> rows{};
        for (std::size_t i = 0; i < 16; ++i)
        {
            rows[i] = static_cast<float>(i + 1);
        }
        CheckOk(params.SetMatrix("gExtra", rows), "SetMatrix");
        Check(ReadAt<float>(params, "gExtra", 0) == 1.0f && ReadAt<float>(params, "gExtra", 1) == 5.0f &&
              ReadAt<float>(params, "gExtra", 4) == 2.0f && ReadAt<float>(params, "gExtra", 15) == 16.0f, "the matrix is transposed into the buffer");

        // Only the bytes of the variable that was set are touched
        MaterialParams alone(LayoutOf("wide.hlsl", kWideShader));
        CheckOk(alone.SetFloat("gRoughness", 1.0f), "alone");
        const ShaderVariable& roughness = Variable(alone, "gRoughness");
        std::size_t nonZero = 0;
        for (const std::uint8_t byte : alone.ConstantBufferBytes(0))
        {
            nonZero += byte != 0 ? 1 : 0;
        }
        Check(nonZero == 2 && alone.ConstantBufferBytes(0)[roughness.offset + 3] == 0x3F, "1.0f is two non-zero bytes, at the offset of gRoughness");
    }

    void TestTextures()
    {
        MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
        Check(params.Textures().size() == 3, "three textures in the layout");
        Check(!params.FindTexture("gTexture")->texture.IsValid(), "no texture yet");
        CheckOk(params.SetTexture("gDetail", TextureHandle{7}), "SetTexture");
        Check(params.FindTexture("gDetail")->texture.value == 7 && params.FindTexture("gDetail")->registerIndex == 1, "the texture and its register");
        Check(params.FindTexture("gMissing") == nullptr, "an unknown texture is not found");
        Check(params.FirstTexture() != nullptr && params.FirstTexture()->name == "gTexture" && params.FirstTexture()->registerIndex == 0,
            "the first texture is the one with the smallest register, whatever the order of the declarations");

        CheckError(params.SetTexture("gNoSuchTexture", TextureHandle{1}), {"gNoSuchTexture", "gTexture", "gDetail", "gMask"}, "an unknown texture");
        CheckError(params.SetTexture("gColor", TextureHandle{1}), {"gColor", "expected a Texture", "Float4"}, "a variable is not a texture");
        CheckError(params.SetTexture("gSampler", TextureHandle{1}), {"gSampler", "expected a Texture", "Sampler"}, "a sampler is not a texture");
    }

    void TestNameAndTypeErrors()
    {
        MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
        const MaterialParamResult unknown = params.SetFloat("gRougness", 0.4f);
        CheckError(unknown, {"gRougness", "gRoughness", "gColor", "gTint3", "gTexture", "Float4"}, "a misspelled name lists the existing names");
        Check(AllZero(params.ConstantBufferBytes(0)), "a failed setter writes nothing");

        CheckError(params.SetFloat("gColor", 1.0f), {"gColor", "expected Float4", "got Float"}, "a float into Float4");
        CheckError(params.SetFloat4("gRoughness", 1.0f, 2.0f, 3.0f, 4.0f), {"gRoughness", "expected Float", "got Float4"}, "Float4 into a float");
        CheckError(params.SetInt("gRoughness", 1), {"expected Float", "got Int"}, "an int into a float");
        CheckError(params.SetUInt("gMode", 1u), {"expected Int", "got UInt"}, "a uint into an int");
        CheckError(params.SetBool("gMode", true), {"expected Int", "got Bool"}, "a bool into an int");
        CheckError(params.SetMatrix("gColor", std::array<float, 16>{}), {"expected Float4", "got Float4x4"}, "a matrix into Float4");
        CheckError(params.SetFloat("gWeights", 1.0f), {"gWeights", "arrays"}, "an array is not supported yet");
        Check(AllZero(params.ConstantBufferBytes(0)), "no failed setter wrote anything");

        // A buffer name is not a variable
        CheckError(params.SetFloat("DrawCB", 1.0f), {"DrawCB"}, "the name of a buffer");
    }

    void TestEngineSemanticsAreNotWrittenByMaterials()
    {
        MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
        std::array<float, 16> rows{};
        rows.fill(9.0f);
        const MaterialParamResult model = params.SetMatrix("gModel", rows);
        Check(model.IsWarning() && Contains(model.message, "gModel") && Contains(model.message, "renderer"), "gModel: a warning, " + model.message);
        Check(params.SetMatrix("gViewProjection", rows).IsWarning(), "gViewProjection: a warning");
        Check(params.SetFloat4("gLightDirection", 1.0f, 1.0f, 1.0f, 1.0f).IsWarning(), "gLightDirection: a warning");
        Check(params.SetValue("gModel", MaterialValue::FromMatrix(std::vector<double>(16, 1.0))).IsWarning(), "a raw value for gModel: a warning");
        Check(AllZero(params.ConstantBufferBytes(0)), "the semantics of the engine stay untouched by a material");
        Check(MaterialParams::IsEngineSemantic("gModel") && MaterialParams::IsEngineSemantic("gViewProjection") && MaterialParams::IsEngineSemantic("gLightDirection") &&
              !MaterialParams::IsEngineSemantic("gColor"), "gColor is an ordinary parameter of a material");
        CheckOk(params.SetFloat4("gColor", 1.0f, 2.0f, 3.0f, 4.0f), "gColor is set by a material");
        Check(Contains(params.DescribeNames(), "set by the renderer"), "the list of the names marks the semantics");
    }

    void TestRawValues()
    {
        MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
        CheckOk(params.SetValue("gRoughness", MaterialValue::FromNumber(0.4)), "a number into a float");
        Check(ReadAt<float>(params, "gRoughness") == 0.4f, "gRoughness 0.4");
        CheckOk(params.SetValue("gMode", MaterialValue::FromNumber(3)), "a number into an int");
        Check(ReadAt<std::int32_t>(params, "gMode") == 3, "gMode 3");
        CheckOk(params.SetValue("gCount", MaterialValue::FromNumber(12)), "a number into a uint");
        Check(ReadAt<std::uint32_t>(params, "gCount") == 12u, "gCount 12");
        CheckOk(params.SetValue("gFlag", MaterialValue::FromBool(true)), "a bool");
        Check(ReadAt<std::uint32_t>(params, "gFlag") == 1u, "gFlag true");
        CheckOk(params.SetValue("gUvScale", MaterialValue::FromVector({1.5, 2.5})), "two numbers into Float2");
        CheckOk(params.SetValue("gTint3", MaterialValue::FromVector({1.0, 2.0, 3.0})), "three numbers into Float3");
        CheckOk(params.SetValue("gColor", MaterialValue::FromVector({1.0, 0.75, 0.12, 1.0})), "four numbers into Float4");
        Check(ReadAt<float>(params, "gColor", 1) == 0.75f, "gColor.y");
        std::vector<double> identity(16, 0.0);
        identity[0] = identity[5] = identity[10] = identity[15] = 1.0;
        identity[3] = 5.0; // the row-major element (0, 3)
        CheckOk(params.SetValue("gExtra", MaterialValue::FromMatrix(identity)), "sixteen numbers into Float4x4");
        Check(ReadAt<float>(params, "gExtra", 12) == 5.0f, "the matrix is stored by columns: the element (0, 3) is at 12");

        CheckError(params.SetValue("gColor", MaterialValue::FromNumber(1.0)), {"gColor", "expected Float4", "got Float"}, "a number into Float4");
        CheckError(params.SetValue("gColor", MaterialValue::FromVector({1.0, 2.0, 3.0})), {"expected Float4", "got Float3"}, "Float3 into Float4");
        CheckError(params.SetValue("gRoughness", MaterialValue::FromBool(true)), {"expected Float", "got Bool"}, "a bool into a float");
        CheckError(params.SetValue("gMode", MaterialValue::FromNumber(2.5)), {"expected Int"}, "a fraction into an int");
        CheckError(params.SetValue("gCount", MaterialValue::FromNumber(-1)), {"expected UInt"}, "a negative number into a uint");
        CheckError(params.SetValue("gNope", MaterialValue::FromNumber(1)), {"gNope", "gRoughness", "gTexture"}, "an unknown name");
    }

    // ---- the compatibility of the old keys ----

    MaterialTextureResolver Resolver()
    {
        return [](const std::string& path) -> TextureHandle
        {
            if (path == "missing.bmp")
            {
                return {};
            }
            return TextureHandle{static_cast<std::uint32_t>(std::hash<std::string>{}(path) % 1000 + 1)};
        };
    }

    std::uint32_t HandleOf(const std::string& path)
    {
        return Resolver()(path).value;
    }

    std::string Joined(const std::vector<MaterialParamResult>& messages)
    {
        std::string text;
        for (const MaterialParamResult& message : messages)
        {
            text += (message.IsError() ? "error: " : "warning: ") + message.message + "\n";
        }
        return text;
    }

    void TestOldKeysMeanTheFirstTextureAndGColor()
    {
        MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
        MaterialValues values;
        values.texture = "old.bmp";
        values.tint = {0.5f, 0.25f, 0.75f, 1.0f};
        const auto messages = ApplyMaterialValues(params, values, Resolver());
        Check(messages.empty(), "the old keys apply without messages: " + Joined(messages));
        Check(params.FindTexture("gTexture")->texture.value == HandleOf("old.bmp"), "'texture' goes to the texture with the smallest register (t0, gTexture), not to the first declared (gDetail)");
        Check(!params.FindTexture("gDetail")->texture.IsValid(), "the other textures stay empty");
        Check(ReadAt<float>(params, "gColor", 0) == 0.5f && ReadAt<float>(params, "gColor", 2) == 0.75f, "'tint' goes to gColor");

        // The default tint (white) of a file without the key is applied the same way: the colour is not black
        MaterialParams plainTint(LayoutOf("wide.hlsl", kWideShader));
        MaterialValues defaults;
        Check(ApplyMaterialValues(plainTint, defaults, Resolver()).empty(), "an empty material applies");
        Check(ReadAt<float>(plainTint, "gColor", 3) == 1.0f, "the default tint is white");
    }

    void TestNewKeysWinOverTheOldOnes()
    {
        // 'textures' sets the first texture too: the new key wins, with a warning
        {
            MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
            MaterialValues values;
            values.texture = "old.bmp";
            values.textures["gTexture"] = "new.bmp";
            values.textures["gDetail"] = "detail.bmp";
            const auto messages = ApplyMaterialValues(params, values, Resolver());
            Check(messages.size() == 1 && messages[0].IsWarning() && Contains(messages[0].message, "texture") && Contains(messages[0].message, "gTexture"),
                "one warning about 'texture': " + Joined(messages));
            Check(params.FindTexture("gTexture")->texture.value == HandleOf("new.bmp"), "the new key wins");
            Check(params.FindTexture("gDetail")->texture.value == HandleOf("detail.bmp"), "the other texture is set by its name");
        }
        // 'textures' sets another texture: 'texture' still means the first one, no conflict
        {
            MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
            MaterialValues values;
            values.texture = "old.bmp";
            values.textures["gDetail"] = "detail.bmp";
            const auto messages = ApplyMaterialValues(params, values, Resolver());
            Check(messages.empty(), "no conflict: " + Joined(messages));
            Check(params.FindTexture("gTexture")->texture.value == HandleOf("old.bmp") && params.FindTexture("gDetail")->texture.value == HandleOf("detail.bmp"), "both set");
        }
        // 'texture' that only repeats a path of 'textures' (the resource manager mirrors one into the other): nothing to say
        {
            MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
            MaterialValues values;
            values.texture = "detail.bmp";
            values.textures["gDetail"] = "detail.bmp";
            Check(ApplyMaterialValues(params, values, Resolver()).empty(), "a mirror is not a conflict");
            Check(!params.FindTexture("gTexture")->texture.IsValid() && params.FindTexture("gDetail")->texture.value == HandleOf("detail.bmp"), "only the named texture is set");
        }
        // 'params' sets gColor and 'tint' says something else: gColor wins, with a warning
        {
            MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
            MaterialValues values;
            values.tint = {0.5f, 0.5f, 0.5f, 1.0f};
            values.params["gColor"] = MaterialValue::FromVector({1.0, 0.0, 0.0, 1.0});
            const auto messages = ApplyMaterialValues(params, values, Resolver());
            Check(messages.size() == 1 && messages[0].IsWarning() && Contains(messages[0].message, "tint") && Contains(messages[0].message, "gColor"), "one warning about 'tint': " + Joined(messages));
            Check(ReadAt<float>(params, "gColor", 0) == 1.0f && ReadAt<float>(params, "gColor", 1) == 0.0f, "the new key wins");
        }
        // 'tint' that is the mirror of gColor: no message
        {
            MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
            MaterialValues values;
            values.tint = {0.8f, 0.4f, 0.2f, 1.0f};
            values.params["gColor"] = MaterialValue::FromVector({0.8, 0.4, 0.2, 1.0});
            Check(ApplyMaterialValues(params, values, Resolver()).empty(), "the mirror of gColor is not a conflict");
        }
    }

    void TestOldKeysWithoutAPlaceInTheShader()
    {
        // No gColor in the shader: a tint that says something gives one warning, the default white says nothing
        MaterialParams plain(LayoutOf("plain.hlsl", kPlainShader));
        MaterialValues white;
        Check(ApplyMaterialValues(plain, white, Resolver()).empty(), "a white tint needs no gColor");
        MaterialValues tinted;
        tinted.tint = {0.1f, 0.2f, 0.3f, 1.0f};
        const auto messages = ApplyMaterialValues(plain, tinted, Resolver());
        Check(messages.size() == 1 && messages[0].IsWarning() && Contains(messages[0].message, "gColor"), "one warning: the shader has no gColor: " + Joined(messages));
        Check(AllZero(plain.ConstantBufferBytes(0)), "and nothing is written");

        // No textures in the shader: 'texture' gets a warning
        MaterialValues textured;
        textured.texture = "old.bmp";
        const auto noTexture = ApplyMaterialValues(plain, textured, Resolver());
        Check(noTexture.size() == 1 && noTexture[0].IsWarning() && Contains(noTexture[0].message, "no texture"), "'texture' without a texture in the shader: " + Joined(noTexture));
    }

    void TestMessagesOfARoughMaterial()
    {
        MaterialParams params(LayoutOf("wide.hlsl", kWideShader));
        MaterialValues values;
        values.textures["gDetail"] = "missing.bmp";
        values.textures["gNoSuchTexture"] = "detail.bmp";
        values.params["gRoughness"] = MaterialValue::FromNumber(0.4);
        values.params["gNoSuchName"] = MaterialValue::FromNumber(1);
        values.params["gColor"] = MaterialValue::FromNumber(1); // a number where Float4 is needed
        values.params["gModel"] = MaterialValue::FromMatrix(std::vector<double>(16, 1.0));
        values.tint = {1.0f, 1.0f, 1.0f, 1.0f};
        const auto messages = ApplyMaterialValues(params, values, Resolver(), "probe.shader.json");
        const std::string text = Joined(messages);
        Check(Contains(text, "missing.bmp"), "a texture that cannot be loaded is named: " + text);
        Check(Contains(text, "gNoSuchTexture") && Contains(text, "gNoSuchName"), "unknown names are named: " + text);
        Check(Contains(text, "expected Float4, got Float"), "the type error says what was expected and what came: " + text);
        Check(Contains(text, "gModel") && Contains(text, "renderer"), "the engine semantic gets its warning: " + text);
        Check(ReadAt<float>(params, "gRoughness") == 0.4f, "what fits is applied nevertheless");
        std::size_t errors = 0;
        for (const MaterialParamResult& message : messages)
        {
            errors += message.IsError() ? 1 : 0;
        }
        Check(errors == 2, "two errors (the texture file, the type): " + text);
        // A name that the shader does not have is a warning with the list of the names, the program is named
        Check(Contains(text, "warning: 'gNoSuchName' is not in probe.shader.json; names: ") && Contains(text, "gRoughness") && Contains(text, "gTexture"),
            "an unknown name is a warning that names the shader and lists the names: " + text);
        Check(Contains(text, "warning: 'gNoSuchTexture' is not in probe.shader.json"), "an unknown texture is a warning too: " + text);
    }

    // ---- the materials of the project and their files ----

    class FakeAdapter final : public render::IRenderAdapter
    {
    public:
        bool Initialize() override { return true; }
        render::MeshHandle UploadMesh(const render::MeshData&) override { return {nextMesh_++}; }
        render::TextureHandle CreateTexture(const render::TextureData&) override { return {nextTexture_++}; }
        void DestroyTexture(render::TextureHandle) override {}
        render::RenderSurfaceHandle CreateSurface(HWND, std::uint32_t, std::uint32_t) override { return {1}; }
        void ResizeSurface(render::RenderSurfaceHandle, std::uint32_t, std::uint32_t) override {}
        bool BeginFrame(render::RenderSurfaceHandle, const core::Color&) override { return true; }
        void SetRenderRegion(render::RenderSurfaceHandle, const render::IntRect*) override {}
        void SetViewProjection(render::RenderSurfaceHandle, const render::Matrix4&, const render::Matrix4&) override {}
        void Draw(render::RenderSurfaceHandle, const render::DrawItem&) override {}
        void DrawDebugLines(render::RenderSurfaceHandle, const std::vector<render::DebugLine>&) override {}
        void DrawUiGeometry(render::RenderSurfaceHandle, const render::UiDrawData&) override {}
        void EndFrame(render::RenderSurfaceHandle) override {}
        void Shutdown() override {}
        render::ShaderHandle CreateShaderProgram(const render::ShaderProgramData&) override { return {nextShader_++}; }
        void DestroyShader(render::ShaderHandle) override {}
        bool ReloadShaderProgram(render::ShaderHandle, const render::ShaderProgramData&) override { return true; }
        std::uint32_t GetShaderVersion(render::ShaderHandle) const override { return 1; }

    private:
        std::uint32_t nextMesh_ = 1;
        std::uint32_t nextTexture_ = 1;
        std::uint32_t nextShader_ = 1;
    };

    struct Harness
    {
        Harness() : resources(adapter, logger) {}

        core::Logger logger;
        FakeAdapter adapter;
        resource::ResourceManager resources;
    };

    json ReadJson(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        Check(stream.is_open(), "cannot read " + path.u8string());
        json value;
        stream >> value;
        return value;
    }

    void WriteText(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
    }

    // The lit shader of the scene passes (G7b): the materials of the RL pack use its parameters by name
    ShaderLayout LitLayout()
    {
        static dx12::ShaderReflector reflector;
        ShaderProgramData program = Entries();
        program.sourcePath = SourceDirectory() / "assets/shaders/lit.hlsl";
        const dx12::ShaderCompileResult result = reflector.CompileProgram(program);
        Check(result.ok && result.program != nullptr, "lit.hlsl compiles");
        return result.program->layout;
    }

    // The particle shader of the fx module (G7c): its materials use its parameters by name
    ShaderLayout ParticleLayout()
    {
        static dx12::ShaderReflector reflector;
        ShaderProgramData program = Entries();
        program.sourcePath = SourceDirectory() / "assets/shaders/particle.hlsl";
        const dx12::ShaderCompileResult result = reflector.CompileProgram(program);
        Check(result.ok && result.program != nullptr, "particle.hlsl compiles");
        return result.program->layout;
    }

    bool UsesParticleShader(const std::string& shaderPath)
    {
        const std::size_t length = std::strlen("/particle.shader.json");
        return shaderPath.size() >= length && shaderPath.compare(shaderPath.size() - length, length, "/particle.shader.json") == 0;
    }

    bool UsesLitShader(const std::string& shaderPath)
    {
        const auto ends = [&](const char* suffix) { return shaderPath.size() >= std::strlen(suffix) && shaderPath.compare(shaderPath.size() - std::strlen(suffix), std::strlen(suffix), suffix) == 0; };
        return ends("/lit.shader.json") || ends("/lit_clamp.shader.json");
    }

    void TestEveryMaterialOfTheProjectReads()
    {
        const ShaderLayout layout = TexturedLitLayout();
        const ShaderLayout litLayout = LitLayout();
        const ShaderLayout particleLayout = ParticleLayout();
        Harness harness;
        std::size_t count = 0;
        for (const auto& entry : fs::recursive_directory_iterator(SourceDirectory() / "assets/materials"))
        {
            if (!entry.is_regular_file() || entry.path().filename().u8string().find(".material.json") == std::string::npos)
            {
                continue;
            }
            const std::string name = entry.path().u8string();
            const auto material = harness.resources.Load<resource::MaterialAsset>(entry.path());
            Check(material != nullptr && !harness.resources.LoadFailed(entry.path()), "the material reads: " + name);
            const resource::MaterialAsset& asset = material->asset;
            if (UsesLitShader(asset.shaderPath))
            {
                // A material of the scene passes: its parameters are names of the lit shader, all of them apply without a message
                MaterialParams litParams(litLayout);
                const auto litMessages = ApplyMaterialValues(litParams, resource::ToMaterialValues(asset), Resolver());
                Check(litMessages.empty(), "a lit material applies to lit.hlsl without a message: " + name + "\n" + Joined(litMessages));
                Check(!asset.texturePath.empty() && litParams.FindTexture("gTexture")->texture.IsValid(), "'texture' reached gTexture of lit: " + name);
                Check(!asset.params.empty(), "a lit material sets its parameters: " + name);
                ++count;
                continue;
            }
            if (UsesParticleShader(asset.shaderPath))
            {
                // A particle material (G7c): its parameters are names of particle.hlsl, all of them apply without a message
                MaterialParams particleParams(particleLayout);
                const auto particleMessages = ApplyMaterialValues(particleParams, resource::ToMaterialValues(asset), Resolver());
                Check(particleMessages.empty(), "a particle material applies to particle.hlsl without a message: " + name + "\n" + Joined(particleMessages));
                Check(!asset.texturePath.empty() && particleParams.FindTexture("gTexture")->texture.IsValid(), "'texture' reached gTexture of particle: " + name);
                ++count;
                continue;
            }
            Check(!asset.texturePath.empty() && asset.textures.empty() && asset.params.empty(), "an old material has the old keys only: " + name);

            MaterialParams params(layout);
            const auto messages = ApplyMaterialValues(params, resource::ToMaterialValues(asset), Resolver());
            Check(messages.empty(), "an old material applies to textured_lit without a message: " + name + "\n" + Joined(messages));
            Check(params.FindTexture("gTexture")->texture.IsValid(), "'texture' reached gTexture: " + name);
            Check(ReadAt<float>(params, "gColor", 0) == asset.tint.r && ReadAt<float>(params, "gColor", 3) == asset.tint.a, "'tint' reached gColor: " + name);
            Check(AllZero(std::vector<std::uint8_t>(params.ConstantBufferBytes(0).begin(), params.ConstantBufferBytes(0).begin() + 128)),
                "the semantics of the engine are not touched: " + name);
            ++count;
        }
        Check(count >= 40, "the project has its materials (" + std::to_string(count) + " found)");
    }

    // The keys of a material file after a save
    std::vector<std::string> KeysOf(const json& value)
    {
        std::vector<std::string> keys;
        for (const auto& entry : value.items())
        {
            keys.push_back(entry.key());
        }
        return keys;
    }

    void TestSaveKeepsTheOldFormatAsItWas(const fs::path& directory)
    {
        Harness harness;
        const fs::path original = SourceDirectory() / "assets/materials/warm.material.json";
        const auto material = harness.resources.Load<resource::MaterialAsset>(original);
        Check(material != nullptr, "warm.material.json loads");
        const fs::path saved = directory / "old" / "warm.material.json";
        Check(harness.resources.SaveMaterial(saved, material->asset), "an old material saves");
        const json before = ReadJson(original);
        const json after = ReadJson(saved);
        Check(KeysOf(after) == KeysOf(before), "the same keys as before: shader, texture, tint (no new keys)");
        Check(after["tint"].size() == 4 && std::abs(after["tint"][0].get<double>() - before["tint"][0].get<double>()) < 1e-6 &&
              std::abs(after["tint"][2].get<double>() - before["tint"][2].get<double>()) < 1e-6, "the tint is the same");

        const auto loaded = harness.resources.Load<resource::MaterialAsset>(saved);
        Check(loaded != nullptr, "and it loads");
        Check(loaded->asset.textures.empty() && loaded->asset.params.empty(), "with no new keys");
        Check(loaded->asset.texturePath == material->asset.texturePath, "and the same texture: '" + loaded->asset.texturePath + "' instead of '" + material->asset.texturePath + "'; file: " + after.dump());
    }

    void TestSaveAndLoadKeepTexturesAndParams(const fs::path& directory)
    {
        Harness harness;
        const auto base = harness.resources.Load<resource::MaterialAsset>(SourceDirectory() / "assets/materials/warm.material.json");
        Check(base != nullptr, "warm.material.json loads");
        const std::string debugTexture = resource::ToMaterialValues(base->asset).texture;

        resource::MaterialAsset asset = base->asset;
        asset.textures["gTexture"] = debugTexture;
        asset.textures["gDetail"] = debugTexture;
        asset.params["gRoughness"] = MaterialValue::FromNumber(0.4);
        asset.params["gMode"] = MaterialValue::FromNumber(2);
        asset.params["gFlag"] = MaterialValue::FromBool(true);
        asset.params["gUvScale"] = MaterialValue::FromVector({2.0, 3.0});
        asset.params["gExtra"] = MaterialValue::FromMatrix({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1});
        asset.params["gColor"] = MaterialValue::FromVector({1.0, 0.75, 0.125, 1.0});
        asset.tint = {1.0f, 0.75f, 0.125f, 1.0f};

        const fs::path saved = directory / "new" / "full.material.json";
        Check(harness.resources.SaveMaterial(saved, asset), "a material with textures and params saves");
        const json file = ReadJson(saved);
        Check(file.contains("textures") && file["textures"].size() == 2 && file["textures"].contains("gDetail"), "the file has 'textures'");
        Check(file.contains("params") && file["params"].size() == 6 && file["params"]["gMode"] == 2 && file["params"]["gFlag"] == true &&
              file["params"]["gRoughness"] == 0.4 && file["params"]["gUvScale"].size() == 2 && file["params"]["gExtra"].size() == 16, "the file has 'params' with the types kept");
        Check(!file.contains("tint"), "gColor is in 'params': the tint is not written next to it");
        Check(!file.contains("texture"), "'texture' only mirrored 'textures': it is not written");

        // Load it again, in a fresh manager
        Harness again;
        const auto loaded = again.resources.Load<resource::MaterialAsset>(saved);
        Check(loaded != nullptr, "the saved material loads");
        const resource::MaterialAsset& back = loaded->asset;
        Check(back.params == asset.params, "the params survived the save and the load");
        Check(back.textures == asset.textures, "the textures survived");
        Check(back.tint.r == 1.0f && back.tint.g == 0.75f && back.tint.b == 0.125f, "the tint mirrors gColor");
        Check(!back.texturePath.empty(), "the mirror of the texture is there for the code that knows one texture");

        // An edit of the tint (the Material Editor does that) edits gColor
        resource::MaterialAsset edited = back;
        edited.tint = {0.25f, 0.5f, 1.0f, 1.0f};
        Check(again.resources.SaveMaterial(saved, edited), "the edited material saves");
        const json editedFile = ReadJson(saved);
        Check(editedFile["params"]["gColor"][0].get<double>() == 0.25 && editedFile["params"]["gColor"][2].get<double>() == 1.0 && !editedFile.contains("tint"), "gColor follows the tint");
        Check(editedFile["params"]["gRoughness"] == 0.4 && editedFile["textures"].size() == 2, "and nothing else is lost");
    }


    std::string ReadRaw(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        Check(stream.is_open(), "cannot read " + path.u8string());
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    // B3.1: a tint that the material never said is not written; a float is written the short way
    void TestSaveWritesTheTintOnlyWhenItIsSet(const fs::path& directory)
    {
        Harness harness;
        const auto base = harness.resources.Load<resource::MaterialAsset>(SourceDirectory() / "assets/materials/warm.material.json");
        Check(base != nullptr && base->asset.tintSet, "an old material with a tint in the file: tintSet");

        resource::MaterialAsset asset = base->asset;
        asset.tintSet = false;
        asset.tint = {0.5f, 0.5f, 0.5f, 1.0f};
        Check(harness.resources.SaveMaterial(directory / "tint" / "unset.material.json", asset), "a material with no tint saves");
        const json unset = ReadJson(directory / "tint" / "unset.material.json");
        Check(!unset.contains("tint") && unset.contains("texture") && unset.contains("shader"), "tintSet false and no gColor in params: no 'tint' in the file");
        const auto reloaded = harness.resources.Load<resource::MaterialAsset>(directory / "tint" / "unset.material.json");
        Check(reloaded != nullptr && !reloaded->asset.tintSet, "and it reads back as not set");

        asset.tintSet = true;
        Check(harness.resources.SaveMaterial(directory / "tint" / "set.material.json", asset), "a material with a tint saves");
        Check(ReadJson(directory / "tint" / "set.material.json").contains("tint"), "tintSet: 'tint' is written");

        // An old material that has its tint in the file is saved as it was
        Check(harness.resources.SaveMaterial(directory / "tint" / "warm.material.json", base->asset), "the old material saves");
        Check(ReadJson(directory / "tint" / "warm.material.json").contains("tint"), "and keeps its tint");
    }

    void TestSavedFloatsAreShort(const fs::path& directory)
    {
        Harness harness;
        const auto base = harness.resources.Load<resource::MaterialAsset>(SourceDirectory() / "assets/materials/warm.material.json");
        Check(base != nullptr, "warm.material.json loads");
        resource::MaterialAsset asset = base->asset;
        const float third = 191.0f / 255.0f; // 0.7490196...
        asset.params["gRoughness"] = MaterialValue::FromNumber(static_cast<double>(0.4f));
        asset.params["gUvScale"] = MaterialValue::FromVector({static_cast<double>(0.3f), static_cast<double>(third)});
        asset.params["gHand"] = MaterialValue::FromNumber(0.1); // a double from a file: written as it is
        asset.tint = {third, 0.1f, 0.25f, 1.0f};
        asset.tintSet = true;
        const fs::path path = directory / "short" / "floats.material.json";
        Check(harness.resources.SaveMaterial(path, asset), "the material saves");
        const std::string text = ReadRaw(path);
        Check(Contains(text, "\"gRoughness\": 0.4") && !Contains(text, "0.4000000"), "0.4f is written as 0.4: " + text);
        Check(Contains(text, "0.3,") || Contains(text, "0.3\n"), "the components of a vector too");
        Check(Contains(text, "0.7490196") && !Contains(text, "0.74901960"), "a channel of the tint: 0.7490196, not the digits of a double");
        Check(Contains(text, "\"gHand\": 0.1"), "a double that is not a float stays as it was");
        // ...and it reads back as the same floats
        const auto back = harness.resources.Load<resource::MaterialAsset>(path);
        Check(back != nullptr && static_cast<float>(back->asset.params.at("gRoughness").numbers[0]) == 0.4f && back->asset.tint.r == third, "the same floats come back");

        // gColor in params is written from the tint, the short way too
        resource::MaterialAsset inParams = asset;
        inParams.params["gColor"] = MaterialValue::FromVector({0.0, 0.0, 0.0, 1.0});
        Check(harness.resources.SaveMaterial(directory / "short" / "params.material.json", inParams), "gColor in params saves");
        const std::string paramsText = ReadRaw(directory / "short" / "params.material.json");
        Check(Contains(paramsText, "0.7490196") && !Contains(paramsText, "\"tint\""), "gColor of params is the tint, in the short form");
    }

    // B3.1: a material needs nothing but a shader; the texture that is not set is white
    void TestAMaterialWithoutTexturesLoads(const fs::path& directory)
    {
        const fs::path shader = SourceDirectory() / "assets/shaders/textured_lit.shader.json";
        json bare = {{"shader", fs::relative(shader, directory / "bare").generic_u8string()}};
        WriteText(directory / "bare" / "bare.material.json", bare.dump(2));
        Harness harness;
        const auto material = harness.resources.Load<resource::MaterialAsset>(directory / "bare" / "bare.material.json");
        Check(material != nullptr && !harness.resources.LoadFailed(directory / "bare" / "bare.material.json"), "a material with a shader and nothing else loads");
        Check(material->asset.texturePath.empty() && material->asset.textures.empty() && !material->asset.tintSet, "no texture, no tint");
        Check(harness.resources.GetWhiteTextureHandle().IsValid(), "the code that needs one texture has the white one");
        MaterialParams params(TexturedLitLayout());
        const auto messages = ApplyMaterialValues(params, resource::ToMaterialValues(material->asset), Resolver());
        Check(messages.empty() && !params.FindTexture("gTexture")->texture.IsValid(), "gTexture is not set: the renderer draws it white");
        Check(ReadAt<float>(params, "gColor", 0) == 1.0f, "and the colour is the @default of the shader, white");

        // Saved as it is: no "texture" key with an empty path, and the file loads again
        Check(harness.resources.SaveMaterial(directory / "bare" / "saved.material.json", material->asset), "a material without a texture saves");
        const json saved = ReadJson(directory / "bare" / "saved.material.json");
        Check(!saved.contains("texture") && !saved.contains("textures") && saved.contains("shader"), "no texture key for no texture: " + saved.dump());
        const auto again = harness.resources.Load<resource::MaterialAsset>(directory / "bare" / "saved.material.json");
        Check(again != nullptr && !harness.resources.LoadFailed(directory / "bare" / "saved.material.json") && again->asset.texturePath.empty(),
            "and it reads back without a texture");

        // The last texture is reset: the material that had one is saved without it
        const auto warm = harness.resources.Load<resource::MaterialAsset>(SourceDirectory() / "assets/materials/warm.material.json");
        Check(warm != nullptr && !warm->asset.texturePath.empty(), "warm.material.json has a texture");
        resource::MaterialAsset reset = warm->asset;
        reset.texturePath.clear();
        reset.textures.clear();
        Check(harness.resources.SaveMaterial(directory / "bare" / "reset.material.json", reset), "a material whose texture was reset saves");
        Check(!ReadJson(directory / "bare" / "reset.material.json").contains("texture"), "without a texture key");
    }

    void TestNewFormatOnlyAndBadEntries(const fs::path& directory)
    {
        const fs::path shader = SourceDirectory() / "assets/shaders/textured_lit.shader.json";
        const fs::path texture = SourceDirectory() / "assets/textures/debug.bmp";
        const auto relative = [&directory](const fs::path& path) { return fs::relative(path, directory / "mixed").generic_u8string(); };

        // Only the new keys: 'textures' and 'params', no 'texture' and no 'tint'
        json onlyNew = {{"shader", relative(shader)}, {"textures", {{"gTexture", relative(texture)}}}, {"params", {{"gColor", {1.0, 0.5, 0.25, 1.0}}, {"gRoughness", 0.4}}}};
        WriteText(directory / "mixed" / "new.material.json", onlyNew.dump(2));
        // The old and the new keys together, and entries that do not read
        json mixed = {{"shader", relative(shader)}, {"texture", relative(texture)}, {"tint", {0.1, 0.2, 0.3, 1.0}},
            {"textures", {{"gDetail", relative(SourceDirectory() / "assets/textures/african_head_diffuse.dds")}, {"gBad", 5}}},
            {"params", {{"gColor", {1.0, 0.0, 0.0, 1.0}}, {"gRoughness", "high"}, {"gSeven", {1, 2, 3, 4, 5, 6, 7}}, {"gOk", true}}}};
        WriteText(directory / "mixed" / "mixed.material.json", mixed.dump(2));

        Harness harness;
        const auto fresh = harness.resources.Load<resource::MaterialAsset>(directory / "mixed" / "new.material.json");
        Check(fresh != nullptr, "a material of the new keys only loads (no 'texture' key is needed)");
        Check(fresh->asset.textures.size() == 1 && !fresh->asset.texturePath.empty() && fresh->asset.texturePath == fresh->asset.textures.begin()->second,
            "'texture' mirrors the first of 'textures'");
        Check(fresh->asset.tint.r == 1.0f && fresh->asset.tint.g == 0.5f && fresh->asset.tint.b == 0.25f, "the tint mirrors gColor");
        Check(fresh->asset.params.size() == 2, "both params");

        const auto both = harness.resources.Load<resource::MaterialAsset>(directory / "mixed" / "mixed.material.json");
        Check(both != nullptr, "a material with entries that do not read still loads");
        Check(both->asset.params.count("gRoughness") == 0 && both->asset.params.count("gSeven") == 0 && both->asset.params.count("gOk") == 1 && both->asset.params.count("gColor") == 1,
            "a string and an array of seven numbers are skipped, the rest is kept");
        Check(both->asset.textures.count("gBad") == 0 && both->asset.textures.count("gDetail") == 1, "a texture that is not a path is skipped");
        Check(both->asset.tint.r == 1.0f && both->asset.tint.g == 0.0f, "gColor wins over the tint of the same file");

        // Applied to textured_lit: 'texture' means gTexture, gDetail is not in that shader, gOk neither
        MaterialParams params(TexturedLitLayout());
        const auto messages = ApplyMaterialValues(params, resource::ToMaterialValues(both->asset), Resolver());
        const std::string text = Joined(messages);
        Check(Contains(text, "gDetail") && Contains(text, "gOk"), "names that textured_lit does not have are reported: " + text);
        Check(params.FindTexture("gTexture")->texture.IsValid(), "'texture' reached gTexture next to the new key");
    }
}

int main()
{
    int result = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path directory = fs::temp_directory_path() / fs::u8path("myengine B1 материалы " + std::to_string(stamp));
    try
    {
        jobs::Initialize(2);
        fs::create_directories(directory);

        TestSettersWriteBytesAtTheOffsets();
        TestTextures();
        TestNameAndTypeErrors();
        TestEngineSemanticsAreNotWrittenByMaterials();
        TestRawValues();
        TestOldKeysMeanTheFirstTextureAndGColor();
        TestNewKeysWinOverTheOldOnes();
        TestOldKeysWithoutAPlaceInTheShader();
        TestMessagesOfARoughMaterial();
        TestEveryMaterialOfTheProjectReads();
        TestSaveKeepsTheOldFormatAsItWas(directory);
        TestSaveAndLoadKeepTexturesAndParams(directory);
        TestNewFormatOnlyAndBadEntries(directory);
        TestSaveWritesTheTintOnlyWhenItIsSet(directory);
        TestSavedFloatsAreShort(directory);
        TestAMaterialWithoutTexturesLoads(directory);

        std::cout << "OK: material params, setters by the names of the layout, the bytes at the offsets, errors, the old keys, the materials of the project, save and load\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        result = 1;
    }
    jobs::Shutdown();
    std::error_code ignored;
    fs::remove_all(directory, ignored);
    return result;
}
