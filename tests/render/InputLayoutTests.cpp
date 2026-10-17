// The input layout builder without a GPU: the inputs of a vertex shader against the table of a vertex stream, and the real
// shaders of the engine against the streams of the renderer.

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderLayout.h>
#include <myengine/render/dx12/InputLayoutBuilder.h>

#include "ShaderReflector.h"

namespace
{
    using namespace myengine::render;
    using namespace myengine::render::dx12;

    void Check(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    VertexInput Input(const std::string& semantic, const std::uint32_t index, const VertexFormat format)
    {
        VertexInput input;
        input.semantic = semantic;
        input.semanticIndex = index;
        input.format = format;
        return input;
    }

    ShaderLayout LayoutOf(const std::vector<VertexInput>& inputs)
    {
        ShaderLayout layout;
        layout.vertexInputs = inputs;
        return layout;
    }

    const VertexStreamDesc& Stream(const std::string& name)
    {
        const VertexStreamDesc* stream = FindVertexStream(name);
        Check(stream != nullptr, "no vertex stream '" + name + "'");
        return *stream;
    }

    void CheckElement(const InputLayoutBuild& build, const std::size_t index, const std::string& semantic, const std::uint32_t semanticIndex, const StreamFormat format, const std::uint32_t offset)
    {
        Check(index < build.elements.size(), "no element " + std::to_string(index) + " (" + std::to_string(build.elements.size()) + " built)");
        const InputElement& element = build.elements[index];
        Check(element.semantic == semantic && element.semanticIndex == semanticIndex,
            "element " + std::to_string(index) + " is " + element.semantic + std::to_string(element.semanticIndex) + " instead of " + semantic + std::to_string(semanticIndex));
        Check(element.format == format, "element " + semantic + ": format " + ToString(element.format) + " instead of " + ToString(format));
        Check(element.offset == offset, "element " + semantic + ": offset " + std::to_string(element.offset) + " instead of " + std::to_string(offset));
    }

    std::string Messages(const InputLayoutBuild& build)
    {
        std::string text;
        for (const ShaderDiagnostic& diagnostic : build.diagnostics)
        {
            text += "\n  " + FormatShaderDiagnostic(diagnostic);
        }
        return text;
    }

    bool Mentions(const InputLayoutBuild& build, const std::string& text)
    {
        for (const ShaderDiagnostic& diagnostic : build.diagnostics)
        {
            if (diagnostic.message.find(text) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    // ---- the tables of the streams ----

    void TestStreams()
    {
        const VertexStreamDesc& mesh = Stream("mesh");
        Check(mesh.stride == kMeshVertexStride && mesh.stride == 32 && mesh.attributes.size() == 3, "the mesh stream: 32 bytes, three attributes");
        const VertexStreamDesc& lines = Stream("debug_lines");
        Check(lines.stride == kDebugLineVertexStride && lines.stride == 32 && lines.attributes.size() == 2, "the debug line stream: 32 bytes, two attributes");
        const VertexStreamDesc& ui = Stream("ui");
        Check(ui.stride == kUiVertexStride && ui.stride == 20 && ui.attributes.size() == 3, "the UI stream: 20 bytes, three attributes");
        Check(FindVertexStream("nothing") == nullptr && FindVertexStream("") == nullptr, "an unknown stream is not found");

        // Every attribute fits into the stride
        for (const VertexStreamDesc* stream : {&mesh, &lines, &ui})
        {
            for (const StreamAttribute& attribute : stream->attributes)
            {
                std::uint32_t size = 0;
                switch (attribute.format)
                {
                    case StreamFormat::Float: case StreamFormat::Int: case StreamFormat::UInt: size = 4; break;
                    case StreamFormat::Float2: case StreamFormat::Int2: case StreamFormat::UInt2: size = 8; break;
                    case StreamFormat::Float3: case StreamFormat::Int3: case StreamFormat::UInt3: size = 12; break;
                    case StreamFormat::Float4: case StreamFormat::Int4: case StreamFormat::UInt4: size = 16; break;
                    case StreamFormat::Unorm8x4: size = 4; break;
                }
                Check(attribute.offset + size <= stream->stride, "'" + stream->name + "': " + attribute.semantic + " does not fit into the stride");
            }
        }

        Check(ToDxgiFormat(StreamFormat::Float3) == DXGI_FORMAT_R32G32B32_FLOAT, "Float3 is R32G32B32_FLOAT");
        Check(ToDxgiFormat(StreamFormat::Unorm8x4) == DXGI_FORMAT_R8G8B8A8_UNORM, "Unorm8x4 is R8G8B8A8_UNORM");
        Check(ToDxgiFormat(StreamFormat::UInt4) == DXGI_FORMAT_R32G32B32A32_UINT && ToDxgiFormat(StreamFormat::Int2) == DXGI_FORMAT_R32G32_SINT, "integer formats");
    }

    // ---- the order and the contents of the elements ----

    void TestMeshLayout()
    {
        const ShaderLayout layout = LayoutOf({
            Input("POSITION", 0, VertexFormat::Float3),
            Input("NORMAL", 0, VertexFormat::Float3),
            Input("TEXCOORD", 0, VertexFormat::Float2),
        });
        const InputLayoutBuild build = BuildInputLayout(layout, Stream("mesh"), "scene");
        Check(build.ok && build.diagnostics.empty(), "the scene inputs fit the mesh stream:" + Messages(build));
        Check(build.stream == "mesh" && build.stride == 32 && build.elements.size() == 3, "stream, stride and three elements");
        CheckElement(build, 0, "POSITION", 0, StreamFormat::Float3, 0);
        CheckElement(build, 1, "NORMAL", 0, StreamFormat::Float3, 12);
        CheckElement(build, 2, "TEXCOORD", 0, StreamFormat::Float2, 24);

        const std::vector<D3D12_INPUT_ELEMENT_DESC> elements = ToD3D12Elements(build);
        Check(elements.size() == 3, "three D3D12 elements");
        Check(std::string(elements[2].SemanticName) == "TEXCOORD" && elements[2].SemanticIndex == 0 && elements[2].Format == DXGI_FORMAT_R32G32_FLOAT &&
                elements[2].AlignedByteOffset == 24 && elements[2].InputSlot == 0 && elements[2].InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA &&
                elements[2].InstanceDataStepRate == 0,
            "the D3D12 element of TEXCOORD");
        Check(elements[0].Format == DXGI_FORMAT_R32G32B32_FLOAT && elements[0].AlignedByteOffset == 0, "the D3D12 element of POSITION");

        const std::string text = FormatInputLayout(build);
        Check(text.find("stream 'mesh'") != std::string::npos && text.find("stride 32") != std::string::npos &&
                text.find("TEXCOORD0") != std::string::npos && text.find("R32G32_FLOAT") != std::string::npos && text.find("offset 24") != std::string::npos,
            "the text of the layout:\n" + text);
    }

    void TestOrderFollowsTheShader()
    {
        // The elements go in the order of the vertex shader inputs, whatever the order in the stream
        const ShaderLayout layout = LayoutOf({
            Input("TEXCOORD", 0, VertexFormat::Float2),
            Input("POSITION", 0, VertexFormat::Float3),
        });
        const InputLayoutBuild build = BuildInputLayout(layout, Stream("mesh"), "reordered");
        Check(build.ok && build.elements.size() == 2, "two inputs in another order:" + Messages(build));
        CheckElement(build, 0, "TEXCOORD", 0, StreamFormat::Float2, 24);
        CheckElement(build, 1, "POSITION", 0, StreamFormat::Float3, 0);
    }

    void TestExtraAttributesAreIgnored()
    {
        // The stream has NORMAL and TEXCOORD that the shader does not read
        const InputLayoutBuild build = BuildInputLayout(LayoutOf({Input("POSITION", 0, VertexFormat::Float3)}), Stream("mesh"), "positions only");
        Check(build.ok && build.diagnostics.empty() && build.elements.size() == 1, "attributes that the shader does not read are ignored:" + Messages(build));
        CheckElement(build, 0, "POSITION", 0, StreamFormat::Float3, 0);
        Check(build.stride == 32, "the stride is the stride of the stream, not of the elements");

        const InputLayoutBuild none = BuildInputLayout(ShaderLayout{}, Stream("mesh"), "no inputs");
        Check(none.ok && none.elements.empty(), "a shader without inputs gives no elements");
    }

    void TestSemanticsAreCaseInsensitive()
    {
        const InputLayoutBuild build = BuildInputLayout(LayoutOf({Input("position", 0, VertexFormat::Float3), Input("TexCoord", 0, VertexFormat::Float2)}), Stream("mesh"), "lowercase");
        Check(build.ok, "HLSL semantics do not depend on the case:" + Messages(build));
        CheckElement(build, 0, "position", 0, StreamFormat::Float3, 0); // the element keeps the name of the shader
        CheckElement(build, 1, "TexCoord", 0, StreamFormat::Float2, 24);
    }

    // ---- what is not supported: an error with the words of the architecture ----

    void TestMissingSemantic()
    {
        const ShaderLayout layout = LayoutOf({
            Input("POSITION", 0, VertexFormat::Float3),
            Input("TANGENT", 0, VertexFormat::Float3),
        });
        const InputLayoutBuild build = BuildInputLayout(layout, Stream("mesh"), "tangent.hlsl");
        Check(!build.ok && build.elements.empty(), "a missing semantic fails and leaves no elements");
        Check(build.diagnostics.size() == 1, "one message:" + Messages(build));
        const ShaderDiagnostic& diagnostic = build.diagnostics[0];
        Check(diagnostic.message == "the shader requires the input TANGENT0, the vertex stream 'mesh' does not contain it", "the text: " + diagnostic.message);
        Check(diagnostic.file == "tangent.hlsl" && !diagnostic.warning && diagnostic.stage == static_cast<std::uint8_t>(ShaderStage::Vertex), "the message names the program and the vertex stage");

        // Another index of a known semantic is another input
        const InputLayoutBuild second = BuildInputLayout(LayoutOf({Input("TEXCOORD", 1, VertexFormat::Float2)}), Stream("mesh"), "uv1");
        Check(!second.ok && Mentions(second, "TEXCOORD1"), "TEXCOORD1 is not in the mesh stream:" + Messages(second));

        // Every missing input is reported
        const InputLayoutBuild two = BuildInputLayout(LayoutOf({Input("TANGENT", 0, VertexFormat::Float3), Input("BINORMAL", 0, VertexFormat::Float3)}), Stream("mesh"), "two");
        Check(!two.ok && two.diagnostics.size() == 2, "two missing inputs, two messages:" + Messages(two));

        // The same shader against the UI stream and the debug stream
        const ShaderLayout scene = LayoutOf({Input("POSITION", 0, VertexFormat::Float3), Input("NORMAL", 0, VertexFormat::Float3), Input("TEXCOORD", 0, VertexFormat::Float2)});
        const InputLayoutBuild onUi = BuildInputLayout(scene, Stream("ui"), "scene on ui");
        Check(!onUi.ok && Mentions(onUi, "NORMAL0") && Mentions(onUi, "'ui'"), "the scene shader on the UI stream:" + Messages(onUi));
    }

    void TestIncompatibleTypes()
    {
        // An integer input from a float attribute and the other way round
        const InputLayoutBuild uintFromFloat = BuildInputLayout(LayoutOf({Input("POSITION", 0, VertexFormat::UInt3)}), Stream("mesh"), "ints");
        Check(!uintFromFloat.ok && Mentions(uintFromFloat, "POSITION0") && Mentions(uintFromFloat, "uint3") && Mentions(uintFromFloat, "float") && Mentions(uintFromFloat, "'mesh'"),
            "a uint input from a float attribute:" + Messages(uintFromFloat));
        const InputLayoutBuild intFromFloat = BuildInputLayout(LayoutOf({Input("NORMAL", 0, VertexFormat::Int)}), Stream("mesh"), "ints");
        Check(!intFromFloat.ok && Mentions(intFromFloat, "int1"), "an int input from a float attribute:" + Messages(intFromFloat));

        // A stream with an integer attribute feeds integer inputs only
        VertexStreamDesc integers;
        integers.name = "integers";
        integers.stride = 16;
        integers.attributes = {{"BLENDINDICES", 0, StreamFormat::UInt4, 0}, {"CELL", 0, StreamFormat::Int2, 8}};
        const InputLayoutBuild fine = BuildInputLayout(LayoutOf({Input("BLENDINDICES", 0, VertexFormat::UInt4), Input("CELL", 0, VertexFormat::Int2)}), integers, "skin");
        Check(fine.ok, "integer inputs from integer attributes:" + Messages(fine));
        CheckElement(fine, 0, "BLENDINDICES", 0, StreamFormat::UInt4, 0);
        CheckElement(fine, 1, "CELL", 0, StreamFormat::Int2, 8);
        const InputLayoutBuild floatFromUint = BuildInputLayout(LayoutOf({Input("BLENDINDICES", 0, VertexFormat::Float4)}), integers, "skin");
        Check(!floatFromUint.ok && Mentions(floatFromUint, "float4"), "a float input from a uint attribute:" + Messages(floatFromUint));
        const InputLayoutBuild signMismatch = BuildInputLayout(LayoutOf({Input("CELL", 0, VertexFormat::UInt2)}), integers, "skin");
        Check(!signMismatch.ok, "uint input from an int attribute:" + Messages(signMismatch));

        // An input of a type that the builder does not know
        const InputLayoutBuild unknown = BuildInputLayout(LayoutOf({Input("POSITION", 0, VertexFormat::Unknown)}), Stream("mesh"), "unknown");
        Check(!unknown.ok && Mentions(unknown, "does not know"), "an unknown input type:" + Messages(unknown));
    }

    void TestTooManyComponents()
    {
        // The mesh position has three components: a float4 input would read past the attribute
        const InputLayoutBuild build = BuildInputLayout(LayoutOf({Input("POSITION", 0, VertexFormat::Float4)}), Stream("mesh"), "float4");
        Check(!build.ok && Mentions(build, "4 components") && Mentions(build, "only 3") && Mentions(build, "R32G32B32_FLOAT"),
            "a float4 input from a float3 attribute:" + Messages(build));

        // Fewer components than the attribute has are fine: the element keeps the format of the stream
        const InputLayoutBuild fewer = BuildInputLayout(LayoutOf({Input("POSITION", 0, VertexFormat::Float2), Input("TEXCOORD", 0, VertexFormat::Float)}), Stream("mesh"), "fewer");
        Check(fewer.ok, "fewer components than the attribute has:" + Messages(fewer));
        CheckElement(fewer, 0, "POSITION", 0, StreamFormat::Float3, 0);
        CheckElement(fewer, 1, "TEXCOORD", 0, StreamFormat::Float2, 24);
    }

    // ---- the streams of the renderer ----

    void TestUiColorIsUnorm()
    {
        const ShaderLayout ui = LayoutOf({
            Input("POSITION", 0, VertexFormat::Float2),
            Input("COLOR", 0, VertexFormat::Float4),
            Input("TEXCOORD", 0, VertexFormat::Float2),
        });
        const InputLayoutBuild build = BuildInputLayout(ui, Stream("ui"), "ui");
        Check(build.ok, "the UI inputs fit the UI stream:" + Messages(build));
        CheckElement(build, 0, "POSITION", 0, StreamFormat::Float2, 0);
        CheckElement(build, 1, "COLOR", 0, StreamFormat::Unorm8x4, 8); // a float4 in the shader, four bytes in the buffer
        CheckElement(build, 2, "TEXCOORD", 0, StreamFormat::Float2, 12);
        Check(ToD3D12Elements(build)[1].Format == DXGI_FORMAT_R8G8B8A8_UNORM, "the colour is R8G8B8A8_UNORM in the pipeline");
        Check(build.stride == 20, "the UI vertex is 20 bytes");

        // float3 is fine too (the shader ignores the alpha), an integer input is not
        const InputLayoutBuild rgb = BuildInputLayout(LayoutOf({Input("COLOR", 0, VertexFormat::Float3)}), Stream("ui"), "rgb");
        Check(rgb.ok, "a float3 colour from the rgba8 attribute:" + Messages(rgb));
        const InputLayoutBuild integers = BuildInputLayout(LayoutOf({Input("COLOR", 0, VertexFormat::UInt4)}), Stream("ui"), "uint colour");
        Check(!integers.ok, "a uint colour from the unorm attribute is an error");
    }

    void TestDebugLines()
    {
        const InputLayoutBuild build = BuildInputLayout(LayoutOf({Input("POSITION", 0, VertexFormat::Float4), Input("COLOR", 0, VertexFormat::Float4)}), Stream("debug_lines"), "lines");
        Check(build.ok, "the debug line inputs:" + Messages(build));
        CheckElement(build, 0, "POSITION", 0, StreamFormat::Float4, 0);
        CheckElement(build, 1, "COLOR", 0, StreamFormat::Float4, 16);
        Check(build.stride == 32, "the debug line vertex is 32 bytes");
    }

    // ---- the real shaders against the streams of the renderer ----

    void TestEngineShaders()
    {
        const std::filesystem::path shaders = std::filesystem::u8path(MYENGINE_SOURCE_DIR) / "assets/shaders";
        ShaderReflector reflector;
        const auto compile = [&](const char* file)
        {
            ShaderProgramData program;
            program.sourcePath = shaders / file;
            const ShaderCompileResult result = reflector.CompileProgram(program);
            Check(result.ok, std::string(file) + " compiles");
            return result.program->layout;
        };

        const InputLayoutBuild scene = BuildInputLayout(compile("textured_lit.hlsl"), Stream("mesh"), "textured_lit");
        Check(scene.ok && scene.elements.size() == 3, "textured_lit on the mesh stream:" + Messages(scene));
        CheckElement(scene, 0, "POSITION", 0, StreamFormat::Float3, 0);
        CheckElement(scene, 1, "NORMAL", 0, StreamFormat::Float3, 12);
        CheckElement(scene, 2, "TEXCOORD", 0, StreamFormat::Float2, 24);

        const InputLayoutBuild lines = BuildInputLayout(compile("debug_line.hlsl"), Stream("debug_lines"), "debug_line");
        Check(lines.ok && lines.elements.size() == 2, "debug_line on the debug line stream:" + Messages(lines));
        CheckElement(lines, 1, "COLOR", 0, StreamFormat::Float4, 16);

        const InputLayoutBuild ui = BuildInputLayout(compile("ui_overlay.hlsl"), Stream("ui"), "ui_overlay");
        Check(ui.ok && ui.elements.size() == 3, "ui_overlay on the UI stream:" + Messages(ui));
        CheckElement(ui, 1, "COLOR", 0, StreamFormat::Unorm8x4, 8);

        // A shader on the wrong stream is an error, not a layout that reads garbage
        const InputLayoutBuild wrong = BuildInputLayout(compile("ui_overlay.hlsl"), Stream("mesh"), "ui_overlay on mesh");
        Check(!wrong.ok && Mentions(wrong, "COLOR0"), "the UI shader on the mesh stream:" + Messages(wrong));
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
        {"stream tables", TestStreams},
        {"mesh layout", TestMeshLayout},
        {"order follows the shader", TestOrderFollowsTheShader},
        {"extra attributes are ignored", TestExtraAttributesAreIgnored},
        {"semantics are case-insensitive", TestSemanticsAreCaseInsensitive},
        {"missing semantic", TestMissingSemantic},
        {"incompatible types", TestIncompatibleTypes},
        {"too many components", TestTooManyComponents},
        {"UI colour is unorm", TestUiColorIsUnorm},
        {"debug lines", TestDebugLines},
        {"engine shaders", TestEngineShaders},
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
    std::cout << (failed == 0 ? "input_layout: all passed\n" : "input_layout: FAILED\n");
    return failed == 0 ? 0 : 1;
}
