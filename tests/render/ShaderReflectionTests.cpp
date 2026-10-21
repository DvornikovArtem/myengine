// Shader reflection without a GPU: HLSL -> DXBC -> render::ShaderLayout, the layout merge, the messages of the compiler,
// the cache, the shaders of the engine and the layout dump. D3DCompile / D3DReflect need no device.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderDescriptor.h>
#include <myengine/render/ShaderLayout.h>
#include <myengine/render/dx12/ShaderLayoutDump.h>

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

    ShaderProgramData Entries()
    {
        ShaderProgramData program;
        program.vertexEntry = "VSMain";
        program.pixelEntry = "PSMain";
        return program;
    }

    // A program from a text; the test fails with the messages of the compiler when it does not compile
    std::shared_ptr<const CompiledShaderProgram> Compile(ShaderReflector& reflector, const std::string& name, const std::string& source, const ShaderProgramData& program)
    {
        const ShaderCompileResult result = reflector.CompileProgramFromSource(name, source, program);
        std::string messages;
        for (const ShaderDiagnostic& diagnostic : result.diagnostics)
        {
            messages += "\n  " + FormatShaderDiagnostic(diagnostic);
        }
        Check(result.ok && result.program != nullptr, "the program '" + name + "' did not compile:" + messages);
        return result.program;
    }

    const ShaderResource& Resource(const ShaderLayout& layout, const std::string& name)
    {
        const ShaderResource* resource = layout.FindResourceByName(name);
        Check(resource != nullptr, "no resource '" + name + "' in the layout:\n" + FormatShaderLayout(layout));
        return *resource;
    }

    void CheckResource(const ShaderLayout& layout, const std::string& name, const ShaderResourceType type, const std::uint32_t registerIndex, const std::uint8_t stages)
    {
        const ShaderResource& resource = Resource(layout, name);
        Check(resource.type == type, "'" + name + "': type " + ToString(resource.type) + " instead of " + ToString(type));
        Check(resource.registerIndex == registerIndex, "'" + name + "': register " + std::to_string(resource.registerIndex) + " instead of " + std::to_string(registerIndex));
        Check(resource.space == 0, "'" + name + "': space " + std::to_string(resource.space));
        Check(resource.stages == stages, "'" + name + "': stages " + ToString(static_cast<ShaderStage>(resource.stages)) + " instead of " + ToString(static_cast<ShaderStage>(stages)));
    }

    void CheckVariable(const ShaderConstantBuffer& buffer, const std::string& name, const ShaderValueType type, const std::uint32_t offset, const std::uint32_t size, const std::uint32_t arrayCount = 1)
    {
        for (const ShaderVariable& variable : buffer.variables)
        {
            if (variable.name != name)
            {
                continue;
            }
            Check(variable.type == type, "'" + name + "': type " + ToString(variable.type) + " instead of " + ToString(type));
            Check(variable.offset == offset, "'" + name + "': offset " + std::to_string(variable.offset) + " instead of " + std::to_string(offset));
            Check(variable.size == size, "'" + name + "': size " + std::to_string(variable.size) + " instead of " + std::to_string(size));
            Check(variable.arrayCount == arrayCount, "'" + name + "': array " + std::to_string(variable.arrayCount) + " instead of " + std::to_string(arrayCount));
            return;
        }
        Check(false, "no variable '" + name + "' in '" + buffer.name + "'");
    }

    void CheckInput(const ShaderLayout& layout, const std::size_t index, const std::string& semantic, const std::uint32_t semanticIndex, const VertexFormat format)
    {
        Check(index < layout.vertexInputs.size(), "no vertex input " + std::to_string(index));
        const VertexInput& input = layout.vertexInputs[index];
        Check(input.semantic == semantic && input.semanticIndex == semanticIndex,
            "input " + std::to_string(index) + " is " + input.semantic + std::to_string(input.semanticIndex) + " instead of " + semantic + std::to_string(semanticIndex));
        Check(input.format == format, "input " + semantic + ": format " + ToString(input.format) + " instead of " + ToString(format));
    }

    // ---- the types of resources, the stages ----

    void TestResourceTypes()
    {
        const std::string source = R"(
cbuffer Frame : register(b0) { float4 gTint; };
Texture2D gAlbedo : register(t0);
TextureCube gSky : register(t1);
Texture3D gVolume : register(t2);
Buffer<float4> gTexels : register(t3);
StructuredBuffer<float4> gItems : register(t4);
SamplerState gLinear : register(s0);
SamplerState gPoint : register(s2);
RWTexture2D<float4> gOutput : register(u1);
RWStructuredBuffer<float4> gScratch : register(u2);

float4 VSMain(float3 position : POSITION) : SV_POSITION { return float4(position, 1) * gTint; }

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET
{
    float4 sum = gAlbedo.Sample(gLinear, position.xy) + gSky.Sample(gPoint, position.xyz) + gVolume.Sample(gLinear, position.xyz);
    sum += gTexels[0] + gItems[1];
    sum *= gTint;
    gOutput[uint2(0, 0)] = sum;
    gScratch[0] = sum;
    return sum;
}
)";
        ShaderReflector reflector;
        const auto program = Compile(reflector, "resource_types.hlsl", source, Entries());
        const ShaderLayout& layout = program->layout;
        const std::uint8_t vs = StageMask(ShaderStage::Vertex);
        const std::uint8_t ps = StageMask(ShaderStage::Pixel);

        CheckResource(layout, "Frame", ShaderResourceType::ConstantBuffer, 0, vs | ps); // both stages use the buffer
        CheckResource(layout, "gAlbedo", ShaderResourceType::Texture, 0, ps);
        CheckResource(layout, "gSky", ShaderResourceType::Texture, 1, ps);
        CheckResource(layout, "gVolume", ShaderResourceType::Texture, 2, ps);
        CheckResource(layout, "gTexels", ShaderResourceType::Buffer, 3, ps);
        CheckResource(layout, "gItems", ShaderResourceType::Buffer, 4, ps);
        CheckResource(layout, "gLinear", ShaderResourceType::Sampler, 0, ps);
        CheckResource(layout, "gPoint", ShaderResourceType::Sampler, 2, ps);
        CheckResource(layout, "gOutput", ShaderResourceType::RWResource, 1, ps);
        CheckResource(layout, "gScratch", ShaderResourceType::RWResource, 2, ps);
        Check(layout.resources.size() == 10, "10 resources expected, " + std::to_string(layout.resources.size()) + " found");
        Check(layout.constantBuffers.size() == 1, "one constant buffer expected");
        Check(Resource(layout, "gAlbedo").bindCount == 1, "bindCount of a single resource is 1");
    }

    void TestResourceArray()
    {
        const std::string source = R"(
Texture2D gLayers[3] : register(t4);
SamplerState gSampler : register(s0);
float4 VSMain(float3 position : POSITION) : SV_POSITION { return float4(position, 1); }
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET
{
    return gLayers[0].Sample(gSampler, position.xy) + gLayers[1].Sample(gSampler, position.xy) + gLayers[2].Sample(gSampler, position.xy);
}
)";
        ShaderReflector reflector;
        const auto program = Compile(reflector, "resource_array.hlsl", source, Entries());
        // fxc reflects an array that is indexed with constants as one binding per element: gLayers[0] at t4, gLayers[1] at t5...
        for (std::uint32_t element = 0; element < 3; ++element)
        {
            CheckResource(program->layout, "gLayers[" + std::to_string(element) + "]", ShaderResourceType::Texture, 4 + element, StageMask(ShaderStage::Pixel));
        }
        Check(program->layout.resources.size() == 4, "three elements and a sampler");
    }

    // ---- constant buffers: types, offsets, the packing into 16 bytes ----

    void TestConstantBufferPacking()
    {
        const std::string source = R"(
cbuffer Params : register(b1)
{
    float a;          // 0
    float3 b;         // 4: packs into the register of `a`
    float4 c;         // 16
    float2 d;         // 32
    float e;          // 40
    float f;          // 44
    float4x4 m;       // 48
    int i;            // 112
    uint u;           // 116
    bool flag;        // 120
    int2 i2;          // 128: does not fit into the 4 bytes left of the register
    float4 arr[3];    // 144
    float sarr[2];    // 192: every element starts a register
    float3x3 n;       // 224
};
float4 VSMain(float3 position : POSITION) : SV_POSITION
{
    return float4(position * b + a * e * f, c.x + d.x + m._11 + i + u + flag + i2.x + arr[1].x + sarr[1] + n._11);
}
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return position; }
)";
        ShaderReflector reflector;
        const auto program = Compile(reflector, "packing.hlsl", source, Entries());
        const ShaderConstantBuffer* buffer = program->layout.FindConstantBuffer("Params");
        Check(buffer != nullptr, "no constant buffer 'Params'");
        Check(buffer->registerIndex == 1 && buffer->space == 0, "Params is at b1");
        Check(buffer->stages == StageMask(ShaderStage::Vertex), "Params is used by the vertex shader only");
        Check(buffer->sizeBytes == 272, "size of Params: " + std::to_string(buffer->sizeBytes) + " instead of 272 (a multiple of 16)");
        Check(buffer->variables.size() == 14, "14 variables expected, " + std::to_string(buffer->variables.size()) + " found");

        CheckVariable(*buffer, "a", ShaderValueType::Float, 0, 4);
        CheckVariable(*buffer, "b", ShaderValueType::Float3, 4, 12);
        CheckVariable(*buffer, "c", ShaderValueType::Float4, 16, 16);
        CheckVariable(*buffer, "d", ShaderValueType::Float2, 32, 8);
        CheckVariable(*buffer, "e", ShaderValueType::Float, 40, 4);
        CheckVariable(*buffer, "f", ShaderValueType::Float, 44, 4);
        CheckVariable(*buffer, "m", ShaderValueType::Float4x4, 48, 64);
        CheckVariable(*buffer, "i", ShaderValueType::Int, 112, 4);
        CheckVariable(*buffer, "u", ShaderValueType::UInt, 116, 4);
        CheckVariable(*buffer, "flag", ShaderValueType::Bool, 120, 4);
        CheckVariable(*buffer, "i2", ShaderValueType::Int2, 128, 8);
        CheckVariable(*buffer, "arr", ShaderValueType::Float4, 144, 48, 3);
        CheckVariable(*buffer, "sarr", ShaderValueType::Float, 192, 20, 2);
        CheckVariable(*buffer, "n", ShaderValueType::Float3x3, 224, 44);

        const ShaderVariableRef found = program->layout.FindVariable("m");
        Check(found.buffer == buffer && found.variable != nullptr && found.variable->offset == 48, "FindVariable('m')");
        Check(program->layout.FindVariable("nothing").variable == nullptr, "FindVariable of a missing name");
    }

    void TestGlobalsBuffer()
    {
        // Variables outside a cbuffer go to the implicit $Globals
        const std::string source = R"(
float4 gColor;
float4 VSMain(float3 position : POSITION) : SV_POSITION { return float4(position, 1); }
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return gColor; }
)";
        ShaderReflector reflector;
        const auto program = Compile(reflector, "globals.hlsl", source, Entries());
        const ShaderConstantBuffer* globals = program->layout.FindConstantBuffer("$Globals");
        Check(globals != nullptr, "no $Globals buffer");
        CheckVariable(*globals, "gColor", ShaderValueType::Float4, 0, 16);
        CheckResource(program->layout, "$Globals", ShaderResourceType::ConstantBuffer, 0, StageMask(ShaderStage::Pixel));
    }

    // ---- vertex inputs ----

    void TestVertexInputs()
    {
        const std::string source = R"(
struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD1;
    uint4 bones : BLENDINDICES;
    int2 cell : TEXCOORD2;
    float4 color : COLOR0;
    float4 packedXyz : NORMAL;     // declared as float4, only xyz is read
    uint vertexId : SV_VertexID;   // not data from a vertex buffer
};
float4 VSMain(VSInput input) : SV_POSITION
{
    return float4(input.position + input.packedXyz.xyz, input.uv.x + input.bones.x + input.cell.x + input.color.x + input.vertexId);
}
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return position; }
)";
        ShaderReflector reflector;
        const auto program = Compile(reflector, "inputs.hlsl", source, Entries());
        const ShaderLayout& layout = program->layout;
        Check(layout.vertexInputs.size() == 6, "6 vertex inputs expected (SV_VertexID is not one), " + std::to_string(layout.vertexInputs.size()) + " found:\n" + FormatShaderLayout(layout));
        CheckInput(layout, 0, "POSITION", 0, VertexFormat::Float3);
        CheckInput(layout, 1, "TEXCOORD", 1, VertexFormat::Float2);
        CheckInput(layout, 2, "BLENDINDICES", 0, VertexFormat::UInt4);
        CheckInput(layout, 3, "TEXCOORD", 2, VertexFormat::Int2);
        CheckInput(layout, 4, "COLOR", 0, VertexFormat::Float4);
        CheckInput(layout, 5, "NORMAL", 0, VertexFormat::Float4);
    }

    // ---- two stages in one layout ----

    void TestStagesMerge()
    {
        const std::string source = R"(
cbuffer Shared : register(b0) { float4 gVertexOnly; float4 gPixelOnly; };
cbuffer Pixel : register(b2) { float4 gTint; };
Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);
float4 VSMain(float3 position : POSITION) : SV_POSITION { return float4(position, 1) + gVertexOnly; }
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return gTexture.Sample(gSampler, position.xy) * gTint + gPixelOnly; }
)";
        ShaderReflector reflector;
        const auto program = Compile(reflector, "stages.hlsl", source, Entries());
        const std::uint8_t vs = StageMask(ShaderStage::Vertex);
        const std::uint8_t ps = StageMask(ShaderStage::Pixel);
        CheckResource(program->layout, "Shared", ShaderResourceType::ConstantBuffer, 0, vs | ps);
        CheckResource(program->layout, "Pixel", ShaderResourceType::ConstantBuffer, 2, ps);
        CheckResource(program->layout, "gTexture", ShaderResourceType::Texture, 0, ps);
        Check(program->layout.constantBuffers.size() == 2, "two constant buffers");
        const ShaderConstantBuffer* shared = program->layout.FindConstantBuffer("Shared");
        Check(shared != nullptr && shared->stages == (vs | ps) && shared->variables.size() == 2, "Shared has both variables for both stages");
        Check(program->vertexLayout.FindResourceByName("gTexture") == nullptr, "the vertex layout does not see the texture");
        Check(program->pixelLayout.FindResourceByName("gTexture") != nullptr, "the pixel layout has the texture");
        Check(program->pixelLayout.vertexInputs.empty(), "no vertex inputs in the pixel layout");
    }

    ShaderResource MakeResource(const std::string& name, const ShaderResourceType type, const std::uint32_t registerIndex, const ShaderStage stage)
    {
        ShaderResource resource;
        resource.name = name;
        resource.type = type;
        resource.registerIndex = registerIndex;
        resource.stages = StageMask(stage);
        return resource;
    }

    void TestMergeConflicts()
    {
        std::vector<ShaderDiagnostic> diagnostics;

        // The same name, the same place: one resource with both stages
        ShaderLayout vertex;
        ShaderLayout pixel;
        vertex.resources.push_back(MakeResource("gTexture", ShaderResourceType::Texture, 0, ShaderStage::Vertex));
        pixel.resources.push_back(MakeResource("gTexture", ShaderResourceType::Texture, 0, ShaderStage::Pixel));
        ShaderLayout merged = MergeShaderLayouts(vertex, pixel, diagnostics);
        Check(diagnostics.empty() && merged.resources.size() == 1 && merged.resources[0].stages == 3, "the same texture in both stages is one resource");

        // The same name at different registers
        pixel.resources[0].registerIndex = 1;
        diagnostics.clear();
        MergeShaderLayouts(vertex, pixel, diagnostics);
        Check(diagnostics.size() == 1 && diagnostics[0].message.find("layout conflict") == 0 && diagnostics[0].message.find("'gTexture'") != std::string::npos,
            "a name at two registers is a layout conflict");

        // One register under different names
        pixel.resources[0] = MakeResource("gOther", ShaderResourceType::Texture, 0, ShaderStage::Pixel);
        diagnostics.clear();
        MergeShaderLayouts(vertex, pixel, diagnostics);
        Check(diagnostics.size() == 1 && diagnostics[0].message.find("register t0") != std::string::npos, "one register under two names is a layout conflict");

        // A texture and a sampler may share the index: they are in different register files
        pixel.resources[0] = MakeResource("gSampler", ShaderResourceType::Sampler, 0, ShaderStage::Pixel);
        diagnostics.clear();
        const ShaderLayout different = MergeShaderLayouts(vertex, pixel, diagnostics);
        Check(diagnostics.empty() && different.resources.size() == 2, "t0 and s0 do not conflict");

        // The same type at the same place but another type: a conflict
        pixel.resources[0] = MakeResource("gTexture", ShaderResourceType::Sampler, 0, ShaderStage::Pixel);
        diagnostics.clear();
        MergeShaderLayouts(vertex, pixel, diagnostics);
        Check(diagnostics.size() == 1, "a name with two types is a layout conflict");

        // A constant buffer of the same name with another size
        ShaderLayout vertexBuffers;
        ShaderLayout pixelBuffers;
        ShaderConstantBuffer buffer;
        buffer.name = "Frame";
        buffer.sizeBytes = 16;
        buffer.stages = StageMask(ShaderStage::Vertex);
        ShaderVariable variable;
        variable.name = "gColor";
        variable.type = ShaderValueType::Float4;
        variable.size = 16;
        buffer.variables.push_back(variable);
        vertexBuffers.constantBuffers.push_back(buffer);
        buffer.stages = StageMask(ShaderStage::Pixel);
        pixelBuffers.constantBuffers.push_back(buffer);
        diagnostics.clear();
        const ShaderLayout same = MergeShaderLayouts(vertexBuffers, pixelBuffers, diagnostics);
        Check(diagnostics.empty() && same.constantBuffers.size() == 1 && same.constantBuffers[0].stages == 3, "an identical buffer is one buffer");

        pixelBuffers.constantBuffers[0].sizeBytes = 32;
        diagnostics.clear();
        MergeShaderLayouts(vertexBuffers, pixelBuffers, diagnostics);
        Check(diagnostics.size() == 1 && diagnostics[0].message.find("'Frame'") != std::string::npos, "a buffer with two sizes is a layout conflict");
    }

    // ---- the messages of the compiler ----

    void TestParseMessages()
    {
        const std::string text =
            "C:\\Users\\dev (copy)\\shaders\\lit.hlsl(12,5-9): error X3004: undeclared identifier 'gFoo'\r\n"
            "lit.hlsl(30): warning X3206: implicit truncation of vector type\r\n"
            "error X3501: 'PSMain': entrypoint not found\r\n";
        const std::vector<ShaderDiagnostic> messages = ParseCompilerMessages(text, "fallback.hlsl", ShaderStage::Pixel);
        Check(messages.size() == 3, "3 messages expected, " + std::to_string(messages.size()) + " parsed");
        Check(messages[0].file == "C:\\Users\\dev (copy)\\shaders\\lit.hlsl" && messages[0].line == 12 && messages[0].column == 5 &&
                messages[0].code == "X3004" && !messages[0].warning && messages[0].message == "undeclared identifier 'gFoo'",
            "the first message: " + FormatShaderDiagnostic(messages[0]));
        Check(messages[1].file == "lit.hlsl" && messages[1].line == 30 && messages[1].column == 0 && messages[1].warning && messages[1].code == "X3206",
            "the second message: " + FormatShaderDiagnostic(messages[1]));
        Check(messages[2].file == "fallback.hlsl" && messages[2].line == 0 && messages[2].code == "X3501" && !messages[2].warning &&
                messages[2].stage == StageMask(ShaderStage::Pixel),
            "the third message: " + FormatShaderDiagnostic(messages[2]));
        Check(FormatShaderDiagnostic(messages[0]) == "C:\\Users\\dev (copy)\\shaders\\lit.hlsl(12,5): error X3004: undeclared identifier 'gFoo'", "the formatted message");
    }

    void TestBrokenShaderHasFileAndLine()
    {
        const std::string source =
            "cbuffer Frame : register(b0) { float4 gTint; };\n"   // 1
            "\n"                                                   // 2
            "float4 VSMain(float3 position : POSITION) : SV_POSITION\n"  // 3
            "{\n"                                                  // 4
            "    return float4(position, 1) * gMissing;\n"         // 5
            "}\n"
            "float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return gTint; }\n";
        ShaderReflector reflector;
        const ShaderCompileResult result = reflector.CompileProgramFromSource("broken_vs.hlsl", source, Entries());
        Check(!result.ok && result.program == nullptr, "a broken shader must not give a program");
        Check(!result.diagnostics.empty(), "a broken shader must give a message");
        const ShaderDiagnostic& first = result.diagnostics.front();
        // fxc makes a relative name absolute; the engine passes absolute paths
        Check(first.file.size() >= 14 && first.file.compare(first.file.size() - 14, 14, "broken_vs.hlsl") == 0, "the file of the message: '" + first.file + "'");
        Check(first.line == 5 && first.column > 0, "the position of the message: " + std::to_string(first.line) + "," + std::to_string(first.column));
        Check(!first.warning && first.code.size() > 1 && first.code[0] == 'X', "the error code: '" + first.code + "'");
        // The compiler checks the whole file for every entry: the same message comes from both stages and is told once, for both
        // The compiler follows an undeclared name with a second message of its own (the function that has no value to return); no message
        // is told twice
        std::string all;
        for (std::size_t i = 0; i < result.diagnostics.size(); ++i)
        {
            all += "\n  " + FormatShaderDiagnostic(result.diagnostics[i]);
            for (std::size_t j = i + 1; j < result.diagnostics.size(); ++j)
            {
                Check(FormatShaderDiagnostic(result.diagnostics[i]) != FormatShaderDiagnostic(result.diagnostics[j]), "a message is told once, for both stages:" + all);
            }
        }
        Check(first.stage == (StageMask(ShaderStage::Vertex) | StageMask(ShaderStage::Pixel)), "the message belongs to both stages");
        Check(first.message.find("gMissing") != std::string::npos, "the text of the message: " + first.message);
        Check(reflector.Stats().failures == 1, "the failure is counted");
    }

    void TestMissingEntryAndInclude()
    {
        ShaderReflector reflector;
        const std::string source = "float4 VSMain(float3 p : POSITION) : SV_POSITION { return float4(p, 1); }\n";

        ShaderProgramData program = Entries(); // there is no PSMain
        const ShaderCompileResult noEntry = reflector.CompileProgramFromSource("no_entry.hlsl", source, program);
        Check(!noEntry.ok, "a missing entry point must fail");
        bool mentionsEntry = false;
        for (const ShaderDiagnostic& diagnostic : noEntry.diagnostics)
        {
            mentionsEntry = mentionsEntry || (diagnostic.message.find("PSMain") != std::string::npos && diagnostic.file == "no_entry.hlsl" &&
                                              diagnostic.stage == StageMask(ShaderStage::Pixel) && !diagnostic.warning);
        }
        Check(mentionsEntry, "the message names the entry point and the file");

        const std::string withInclude = "#include \"missing_file.hlsli\"\n" + source +
            "float4 PSMain(float4 p : SV_POSITION) : SV_TARGET { return p; }\n";
        const ShaderCompileResult noInclude = reflector.CompileProgramFromSource("with_include.hlsl", withInclude, Entries());
        Check(!noInclude.ok && !noInclude.diagnostics.empty(), "a missing include must fail");
        Check(noInclude.diagnostics.front().line == 1, "the message points at the #include line: " + FormatShaderDiagnostic(noInclude.diagnostics.front()));

        // A file that does not exist: a message with the path
        ShaderProgramData missing = Entries();
        missing.sourcePath = std::filesystem::temp_directory_path() / "myengine_no_such_shader.hlsl";
        const ShaderCompileResult noFile = reflector.CompileProgram(missing);
        Check(!noFile.ok && noFile.diagnostics.size() == 1 && noFile.diagnostics[0].file == missing.sourcePath.string(), "a missing file gives one message with the path");
    }

    // ---- the cache ----

    void TestCache()
    {
        const std::string v1 = R"(
cbuffer Frame : register(b0) { float4 gTint; };
float4 VSMain(float3 position : POSITION) : SV_POSITION { return float4(position, 1) * gTint; }
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return position; }
)";
        const std::string v2 = R"(
cbuffer Frame : register(b0) { float4 gTint; float gScale; };
float4 VSMain(float3 position : POSITION) : SV_POSITION { return float4(position, 1) * gTint * gScale; }
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return position; }
)";
        const std::string broken = v2 + "float4 oops = ;\n";

        ShaderReflector reflector;
        const auto first = reflector.CompileProgramFromSource("cache.hlsl", v1, Entries());
        Check(first.ok && !first.fromCache, "the first compile is a miss");
        const auto second = reflector.CompileProgramFromSource("cache.hlsl", v1, Entries());
        Check(second.ok && second.fromCache && second.program == first.program, "the same text is served from the cache, the same object");
        Check(reflector.Stats().compiles == 1 && reflector.Stats().cacheHits == 1, "one compile, one hit");

        const auto changed = reflector.CompileProgramFromSource("cache.hlsl", v2, Entries());
        Check(changed.ok && !changed.fromCache && changed.program != first.program, "a new text is compiled again");
        Check(changed.program->layout.FindVariable("gScale").variable != nullptr, "the new layout has the new variable");
        Check(first.program->layout.FindVariable("gScale").variable == nullptr, "the old layout is untouched");
        Check(reflector.CachedProgramCount() == 1, "one entry per program: the old version is replaced");

        const auto bad = reflector.CompileProgramFromSource("cache.hlsl", broken, Entries());
        Check(!bad.ok, "a broken text fails");
        const auto again = reflector.CompileProgramFromSource("cache.hlsl", v2, Entries());
        Check(again.ok && again.fromCache && again.program == changed.program, "a failed rebuild leaves the last good program in the cache");

        // Defines are a part of the identity
        const std::string withDefine = R"(
cbuffer Frame : register(b0) { float4 gTint;
#ifdef WITH_SCALE
    float gScale;
#endif
};
float4 VSMain(float3 position : POSITION) : SV_POSITION { return float4(position, 1) * gTint; }
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET { return position; }
)";
        ShaderProgramData plain = Entries();
        ShaderProgramData scaled = Entries();
        scaled.defines.emplace_back("WITH_SCALE", "1");
        const auto plainResult = reflector.CompileProgramFromSource("defines.hlsl", withDefine, plain);
        const auto scaledResult = reflector.CompileProgramFromSource("defines.hlsl", withDefine, scaled);
        Check(plainResult.ok && scaledResult.ok && !scaledResult.fromCache, "another set of defines is another program");
        Check(plainResult.program->layout.FindVariable("gScale").variable == nullptr && scaledResult.program->layout.FindVariable("gScale").variable != nullptr,
            "the define changes the layout");

        reflector.Invalidate("cache.hlsl");
        const auto afterInvalidate = reflector.CompileProgramFromSource("cache.hlsl", v2, Entries());
        Check(afterInvalidate.ok && !afterInvalidate.fromCache, "Invalidate drops the cached program");
        reflector.Clear();
        Check(reflector.CachedProgramCount() == 0, "Clear empties the cache");
    }

    void TestCacheFollowsIncludes()
    {
        namespace fs = std::filesystem;
        const fs::path directory = fs::temp_directory_path() / "myengine_shader_reflection_test";
        fs::create_directories(directory);
        const fs::path include = directory / "common.hlsli";
        const fs::path main = directory / "with_include.hlsl";
        const auto write = [](const fs::path& path, const std::string& text)
        {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream << text;
        };
        write(include, "cbuffer Frame : register(b0) { float4 gTint; };\n");
        write(main, "#include \"common.hlsli\"\n"
                    "float4 VSMain(float3 p : POSITION) : SV_POSITION { return float4(p, 1) * gTint; }\n"
                    "float4 PSMain(float4 p : SV_POSITION) : SV_TARGET { return p; }\n");

        ShaderProgramData program = Entries();
        program.sourcePath = main;
        ShaderReflector reflector;
        const ShaderCompileResult first = reflector.CompileProgram(program);
        Check(first.ok, "the program with an include compiles");
        Check(reflector.CompileProgram(program).fromCache, "the second call is a hit");

        write(include, "cbuffer Frame : register(b0) { float4 gTint; float gScale; };\n"); // only the include changed
        const ShaderCompileResult changed = reflector.CompileProgram(program);
        Check(changed.ok && !changed.fromCache, "a changed include invalidates the program");
        Check(changed.program->layout.FindVariable("gScale").variable != nullptr, "the new variable of the include is in the layout");

        std::error_code ec;
        fs::remove_all(directory, ec);
    }

    // ---- the includes: the list for the dependencies of a program, Invalidate ----

    void WriteFile(const fs::path& path, const std::string& text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
    }

    void TestScanShaderIncludes()
    {
        const fs::path directory = fs::temp_directory_path() / fs::u8path("myengine_scan_includes_\u0442\u0435\u0441\u0442");
        fs::remove_all(directory);
        WriteFile(directory / "main.hlsl",
            "#include \"a.hlsli\"\n"
            "# include \"sub/b.hlsli\"\n"
            "#include \"a.hlsli\"\n"           // a repeat
            "#include \"missing.hlsli\"\n"     // a file that does not exist
            "// #include <system.h>\n");         // not a quoted include
        WriteFile(directory / "a.hlsli", "#include \"sub/b.hlsli\"\nfloat4 gA;\n"); // includes b too
        WriteFile(directory / "sub" / "b.hlsli", "#include \"c.hlsli\"\n");           // relative to sub/
        WriteFile(directory / "sub" / "c.hlsli", "#include \"../main.hlsl\"\n");      // a cycle back to the main file

        const std::vector<fs::path> includes = ScanShaderIncludes(directory / "main.hlsl");
        const auto has = [&includes](const fs::path& path)
        {
            for (const fs::path& include : includes)
            {
                if (include.lexically_normal() == path.lexically_normal())
                {
                    return true;
                }
            }
            return false;
        };
        Check(has(directory / "a.hlsli"), "a.hlsli is an include");
        Check(has(directory / "sub" / "b.hlsli"), "sub/b.hlsli is an include");
        Check(has(directory / "sub" / "c.hlsli"), "the include of an include (relative to its own folder) is listed");
        Check(has(directory / "missing.hlsli"), "a file that does not exist is listed: creating it must reload the program");
        Check(has(directory / "main.hlsl"), "the cycle reaches the main file once and stops");
        Check(includes.size() == 5, "a.hlsli, b, c, missing, the main file again: " + std::to_string(includes.size()) + " listed");

        // The text given by the caller is scanned instead of the file
        const std::vector<fs::path> fromText = ScanShaderIncludes(directory / "other.hlsl", "#include \"x.hlsli\"\n");
        Check(fromText.size() == 1 && fromText[0].filename() == "x.hlsli" && fromText[0].parent_path().lexically_normal() == directory.lexically_normal(), "the includes of a text are relative to the given file");
        Check(ScanShaderIncludes(directory / "no_such_file.hlsl").empty(), "a missing source has no includes");

        std::error_code ec;
        fs::remove_all(directory, ec);
    }

    void TestInvalidateSeesIncludeEdit()
    {
        const fs::path directory = fs::temp_directory_path() / fs::u8path("myengine_invalidate_\u0442\u0435\u0441\u0442");
        fs::remove_all(directory);
        const fs::path include = directory / "common.hlsli";
        const fs::path main = directory / "lit.hlsl";
        WriteFile(include, "cbuffer Frame : register(b0) { float4 gTint; };\n");
        WriteFile(main, "#include \"common.hlsli\"\n"
                        "float4 VSMain(float3 p : POSITION) : SV_POSITION { return float4(p, 1) * gTint; }\n"
                        "float4 PSMain(float4 p : SV_POSITION) : SV_TARGET { return p; }\n");

        ShaderProgramData program = Entries();
        program.sourcePath = main;
        ShaderReflector reflector;

        const ShaderCompileResult first = reflector.CompileProgram(program);
        std::string firstMessages;
        for (const ShaderDiagnostic& diagnostic : first.diagnostics)
        {
            firstMessages += "\n  " + FormatShaderDiagnostic(diagnostic);
        }
        Check(first.ok && !first.fromCache, "the first compile:" + firstMessages);
        Check(reflector.CompileProgram(program).fromCache, "unchanged: from the cache");

        // Invalidate drops the program of the file: the next call compiles again although nothing changed
        reflector.Invalidate(main);
        Check(reflector.CachedProgramCount() == 0, "Invalidate empties the cache of this file");
        const ShaderCompileResult again = reflector.CompileProgram(program);
        Check(again.ok && !again.fromCache && again.program != first.program, "after Invalidate the program is compiled again");

        // Only the include changed: Invalidate of the main file and a new compile see the new layout
        WriteFile(include, "cbuffer Frame : register(b0) { float4 gTint; float gStrength; };\n");
        reflector.Invalidate(main);
        const ShaderCompileResult edited = reflector.CompileProgram(program);
        Check(edited.ok && !edited.fromCache, "a changed include: a new compile");
        Check(edited.program->layout.FindVariable("gStrength").variable != nullptr, "the new variable of the include is in the layout");
        Check(again.program->layout.FindVariable("gStrength").variable == nullptr, "the older program is untouched");

        // Invalidate of another file leaves this one in the cache
        reflector.Invalidate(directory / "other.hlsl");
        Check(reflector.CompileProgram(program).fromCache, "Invalidate of another file keeps the cache");

        // A broken include: the compile fails with a message that points into the include, the cached good version stays
        WriteFile(include, "cbuffer Frame : register(b0) { float4 gTint float gStrength; };\n");
        reflector.Invalidate(main);
        const ShaderCompileResult broken = reflector.CompileProgram(program);
        Check(!broken.ok && !broken.diagnostics.empty(), "a broken include fails the compile");
        bool pointsIntoInclude = false;
        for (const ShaderDiagnostic& diagnostic : broken.diagnostics)
        {
            pointsIntoInclude = pointsIntoInclude || (diagnostic.file.find("common.hlsli") != std::string::npos && diagnostic.line == 1);
        }
        Check(pointsIntoInclude, "the message names common.hlsli, line 1");

        std::error_code ec;
        fs::remove_all(directory, ec);
    }

    // ---- the shaders of the engine ----

    fs::path ShadersDirectory();

    void TestEngineShaders()
    {
        ShaderReflector reflector;
        const std::uint8_t vs = StageMask(ShaderStage::Vertex);
        const std::uint8_t ps = StageMask(ShaderStage::Pixel);

        ShaderProgramData scene = Entries();
        scene.sourcePath = ShadersDirectory() / "textured_lit.hlsl";
        const ShaderCompileResult sceneResult = reflector.CompileProgram(scene);
        Check(sceneResult.ok, "textured_lit.hlsl compiles");
        const ShaderLayout& sceneLayout = sceneResult.program->layout;
        CheckResource(sceneLayout, "DrawCB", ShaderResourceType::ConstantBuffer, 0, vs | ps);
        CheckResource(sceneLayout, "gTexture", ShaderResourceType::Texture, 0, ps);
        CheckResource(sceneLayout, "gSampler", ShaderResourceType::Sampler, 0, ps);
        Check(sceneLayout.resources.size() == 3, "textured_lit: three resources");
        const ShaderConstantBuffer* drawCb = sceneLayout.FindConstantBuffer("DrawCB");
        Check(drawCb != nullptr && drawCb->sizeBytes == 160 && drawCb->variables.size() == 4, "DrawCB: 160 bytes, 4 variables");
        CheckVariable(*drawCb, "gModel", ShaderValueType::Float4x4, 0, 64);
        CheckVariable(*drawCb, "gViewProjection", ShaderValueType::Float4x4, 64, 64);
        CheckVariable(*drawCb, "gColor", ShaderValueType::Float4, 128, 16);
        CheckVariable(*drawCb, "gLightDirection", ShaderValueType::Float4, 144, 16);
        Check(sceneLayout.vertexInputs.size() == 3, "textured_lit: three vertex inputs");
        CheckInput(sceneLayout, 0, "POSITION", 0, VertexFormat::Float3);
        CheckInput(sceneLayout, 1, "NORMAL", 0, VertexFormat::Float3);
        CheckInput(sceneLayout, 2, "TEXCOORD", 0, VertexFormat::Float2);

        ShaderProgramData debug = Entries();
        debug.sourcePath = ShadersDirectory() / "debug_line.hlsl";
        const ShaderCompileResult debugResult = reflector.CompileProgram(debug);
        Check(debugResult.ok, "debug_line.hlsl compiles");
        Check(debugResult.program->layout.resources.empty() && debugResult.program->layout.constantBuffers.empty(), "debug_line has no resources");
        Check(debugResult.program->layout.vertexInputs.size() == 2, "debug_line: two vertex inputs");
        CheckInput(debugResult.program->layout, 0, "POSITION", 0, VertexFormat::Float4);
        CheckInput(debugResult.program->layout, 1, "COLOR", 0, VertexFormat::Float4);

        ShaderProgramData ui = Entries();
        ui.sourcePath = ShadersDirectory() / "ui_overlay.hlsl";
        const ShaderCompileResult uiResult = reflector.CompileProgram(ui);
        Check(uiResult.ok, "ui_overlay.hlsl compiles");
        CheckResource(uiResult.program->layout, "UiConstants", ShaderResourceType::ConstantBuffer, 0, vs);
        CheckResource(uiResult.program->layout, "uiTexture", ShaderResourceType::Texture, 0, ps);
        CheckResource(uiResult.program->layout, "uiSampler", ShaderResourceType::Sampler, 0, ps);
        const ShaderConstantBuffer* uiBuffer = uiResult.program->layout.FindConstantBuffer("UiConstants");
        Check(uiBuffer != nullptr && uiBuffer->sizeBytes == 16, "UiConstants: 16 bytes");
        CheckVariable(*uiBuffer, "screenSize", ShaderValueType::Float2, 0, 8);
        CheckVariable(*uiBuffer, "translation", ShaderValueType::Float2, 8, 8);
        CheckInput(uiResult.program->layout, 0, "POSITION", 0, VertexFormat::Float2);
        CheckInput(uiResult.program->layout, 1, "COLOR", 0, VertexFormat::Float4);
        CheckInput(uiResult.program->layout, 2, "TEXCOORD", 0, VertexFormat::Float2);
    }

    void TestDump()
    {
        std::string text;
        const int failures = DumpShaderLayouts(ShadersDirectory(), text);
        Check(failures == 0, "every shader of assets/shaders compiles; the dump:\n" + text);
        for (const char* expected : {"textured_lit.shader.json", "debug_line.hlsl", "ui_overlay.hlsl", "DrawCB", "gLightDirection", "offset  128", "POSITION0", "Float3"})
        {
            Check(text.find(expected) != std::string::npos, std::string("the dump has no '") + expected + "':\n" + text);
        }
        const std::string table = FormatShaderLayout(ShaderLayout{});
        Check(table.find("Resources (0)") != std::string::npos, "the table of an empty layout");
    }

    fs::path ShadersDirectory()
    {
        return fs::u8path(MYENGINE_SOURCE_DIR) / "assets/shaders";
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
        {"resource types", TestResourceTypes},
        {"resource array", TestResourceArray},
        {"constant buffer packing", TestConstantBufferPacking},
        {"$Globals buffer", TestGlobalsBuffer},
        {"vertex inputs", TestVertexInputs},
        {"stages merge", TestStagesMerge},
        {"merge conflicts", TestMergeConflicts},
        {"compiler messages", TestParseMessages},
        {"broken shader: file and line", TestBrokenShaderHasFileAndLine},
        {"missing entry, include, file", TestMissingEntryAndInclude},
        {"cache", TestCache},
        {"cache follows includes", TestCacheFollowsIncludes},
        {"scan of includes", TestScanShaderIncludes},
        {"Invalidate and includes", TestInvalidateSeesIncludeEdit},
        {"engine shaders", TestEngineShaders},
        {"layout dump", TestDump},
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
    std::cout << (failed == 0 ? "shader_reflection: all passed\n" : "shader_reflection: FAILED\n");
    return failed == 0 ? 0 : 1;
}
