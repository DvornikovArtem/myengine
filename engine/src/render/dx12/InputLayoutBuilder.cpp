// InputLayoutBuilder.cpp

#include <myengine/render/dx12/InputLayoutBuilder.h>

#include <algorithm>
#include <cctype>

namespace myengine::render::dx12
{
    namespace
    {
        bool SameSemantic(const std::string& a, const std::string& b)
        {
            // HLSL semantics are case-insensitive
            return a.size() == b.size() &&
                std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) { return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y)); });
        }

        struct FormatInfo
        {
            enum class Kind { Float, Int, UInt } kind;
            int components;
        };

        // What the shader sees when it reads an attribute of this format
        FormatInfo InfoOf(const StreamFormat format)
        {
            switch (format)
            {
                case StreamFormat::Float: return {FormatInfo::Kind::Float, 1};
                case StreamFormat::Float2: return {FormatInfo::Kind::Float, 2};
                case StreamFormat::Float3: return {FormatInfo::Kind::Float, 3};
                case StreamFormat::Float4: return {FormatInfo::Kind::Float, 4};
                case StreamFormat::Int: return {FormatInfo::Kind::Int, 1};
                case StreamFormat::Int2: return {FormatInfo::Kind::Int, 2};
                case StreamFormat::Int3: return {FormatInfo::Kind::Int, 3};
                case StreamFormat::Int4: return {FormatInfo::Kind::Int, 4};
                case StreamFormat::UInt: return {FormatInfo::Kind::UInt, 1};
                case StreamFormat::UInt2: return {FormatInfo::Kind::UInt, 2};
                case StreamFormat::UInt3: return {FormatInfo::Kind::UInt, 3};
                case StreamFormat::UInt4: return {FormatInfo::Kind::UInt, 4};
                case StreamFormat::Unorm8x4: return {FormatInfo::Kind::Float, 4};
            }
            return {FormatInfo::Kind::Float, 0};
        }

        // What the vertex shader asks for
        bool InfoOf(const VertexFormat format, FormatInfo& out)
        {
            switch (format)
            {
                case VertexFormat::Float: out = {FormatInfo::Kind::Float, 1}; return true;
                case VertexFormat::Float2: out = {FormatInfo::Kind::Float, 2}; return true;
                case VertexFormat::Float3: out = {FormatInfo::Kind::Float, 3}; return true;
                case VertexFormat::Float4: out = {FormatInfo::Kind::Float, 4}; return true;
                case VertexFormat::Int: out = {FormatInfo::Kind::Int, 1}; return true;
                case VertexFormat::Int2: out = {FormatInfo::Kind::Int, 2}; return true;
                case VertexFormat::Int3: out = {FormatInfo::Kind::Int, 3}; return true;
                case VertexFormat::Int4: out = {FormatInfo::Kind::Int, 4}; return true;
                case VertexFormat::UInt: out = {FormatInfo::Kind::UInt, 1}; return true;
                case VertexFormat::UInt2: out = {FormatInfo::Kind::UInt, 2}; return true;
                case VertexFormat::UInt3: out = {FormatInfo::Kind::UInt, 3}; return true;
                case VertexFormat::UInt4: out = {FormatInfo::Kind::UInt, 4}; return true;
                case VertexFormat::Unknown: return false;
            }
            return false;
        }

        const char* KindName(const FormatInfo::Kind kind)
        {
            switch (kind)
            {
                case FormatInfo::Kind::Float: return "float";
                case FormatInfo::Kind::Int: return "int";
                case FormatInfo::Kind::UInt: return "uint";
            }
            return "?";
        }

        void AddError(InputLayoutBuild& build, const std::string& programName, std::string message)
        {
            ShaderDiagnostic diagnostic;
            diagnostic.file = programName;
            diagnostic.stage = StageMask(ShaderStage::Vertex);
            diagnostic.message = std::move(message);
            build.diagnostics.push_back(std::move(diagnostic));
            build.ok = false;
        }

        std::vector<VertexStreamDesc> MakeStreams()
        {
            std::vector<VertexStreamDesc> streams;

            VertexStreamDesc mesh;
            mesh.name = "mesh";
            mesh.stride = kMeshVertexStride;
            mesh.attributes = {
                {"POSITION", 0, StreamFormat::Float3, 0},
                {"NORMAL", 0, StreamFormat::Float3, 12},
                {"TEXCOORD", 0, StreamFormat::Float2, 24},
            };
            VertexStreamDesc instanced = mesh;
            instanced.name = "mesh_instanced";
            instanced.instanceStride = kInstanceStride;
            for (std::uint32_t row = 0; row < 4; ++row)
            {
                StreamAttribute attribute{"INSTANCE", row, StreamFormat::Float4, row * 16};
                attribute.slot = 1;
                instanced.attributes.push_back(attribute);
            }
            streams.push_back(std::move(mesh));
            streams.push_back(std::move(instanced));

            VertexStreamDesc debugLines;
            debugLines.name = "debug_lines";
            debugLines.stride = kDebugLineVertexStride;
            debugLines.attributes = {
                {"POSITION", 0, StreamFormat::Float4, 0},
                {"COLOR", 0, StreamFormat::Float4, 16},
            };
            streams.push_back(std::move(debugLines));

            VertexStreamDesc ui;
            ui.name = "ui";
            ui.stride = kUiVertexStride;
            ui.attributes = {
                {"POSITION", 0, StreamFormat::Float2, 0},
                {"COLOR", 0, StreamFormat::Unorm8x4, 8},
                {"TEXCOORD", 0, StreamFormat::Float2, 12},
            };
            streams.push_back(std::move(ui));

            // The point cloud pass (Dx12CloudPass.cpp) builds its own pipeline: both buffers are read per instance (six vertices from SV_VertexID, one instance per point).
            // The entry is here so that the shader is compiled and reflected like the others (the layout dump, the Shader Layouts window).
            VertexStreamDesc cloud;
            cloud.name = "point_cloud";
            cloud.stride = kPointCloudStride;
            cloud.attributes = {{"CLOUDPOS", 0, StreamFormat::Float4, 0}};
            cloud.instanceStride = kPointCloudFlashStride;
            StreamAttribute spike{"CLOUDSPIKE", 0, StreamFormat::Float, 0};
            spike.slot = 1;
            cloud.attributes.push_back(spike);
            streams.push_back(std::move(cloud));
            return streams;
        }
    }

    const VertexStreamDesc* FindVertexStream(const std::string& name)
    {
        static const std::vector<VertexStreamDesc> kStreams = MakeStreams();
        for (const VertexStreamDesc& stream : kStreams)
        {
            if (stream.name == name)
            {
                return &stream;
            }
        }
        return nullptr;
    }

    InputLayoutBuild BuildInputLayout(const ShaderLayout& layout, const VertexStreamDesc& stream, const std::string& programName)
    {
        InputLayoutBuild build;
        build.ok = true;
        build.stream = stream.name;
        build.stride = stream.stride;
        build.instanceStride = 0;

        for (const VertexInput& input : layout.vertexInputs)
        {
            const std::string inputName = input.semantic + std::to_string(input.semanticIndex);

            const StreamAttribute* attribute = nullptr;
            for (const StreamAttribute& candidate : stream.attributes)
            {
                if (candidate.semanticIndex == input.semanticIndex && SameSemantic(candidate.semantic, input.semantic))
                {
                    attribute = &candidate;
                    break;
                }
            }
            if (attribute == nullptr)
            {
                AddError(build, programName, "the shader requires the input " + inputName + ", the vertex stream '" + stream.name + "' does not contain it");
                continue;
            }

            FormatInfo wanted{};
            if (!InfoOf(input.format, wanted))
            {
                AddError(build, programName, "the input " + inputName + " has a type that the input layout builder does not know");
                continue;
            }
            const FormatInfo given = InfoOf(attribute->format);
            if (wanted.kind != given.kind)
            {
                AddError(build, programName, "the input " + inputName + " is " + KindName(wanted.kind) + std::to_string(wanted.components) + " in the shader, but the stream '" +
                    stream.name + "' holds " + ToString(attribute->format) + " (" + KindName(given.kind) + ")");
                continue;
            }
            if (wanted.components > given.components)
            {
                AddError(build, programName, "the input " + inputName + " has " + std::to_string(wanted.components) + " components in the shader, the stream '" +
                    stream.name + "' holds only " + std::to_string(given.components) + " (" + ToString(attribute->format) + ")");
                continue;
            }

            InputElement element;
            element.semantic = input.semantic;
            element.semanticIndex = input.semanticIndex;
            element.format = attribute->format;
            element.offset = attribute->offset;
            element.slot = attribute->slot;
            if (attribute->slot == 1)
            {
                build.instanceStride = stream.instanceStride;
            }
            build.elements.push_back(std::move(element));
        }

        if (!build.ok)
        {
            build.elements.clear();
        }
        return build;
    }

    DXGI_FORMAT ToDxgiFormat(const StreamFormat format)
    {
        switch (format)
        {
            case StreamFormat::Float: return DXGI_FORMAT_R32_FLOAT;
            case StreamFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
            case StreamFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
            case StreamFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            case StreamFormat::Int: return DXGI_FORMAT_R32_SINT;
            case StreamFormat::Int2: return DXGI_FORMAT_R32G32_SINT;
            case StreamFormat::Int3: return DXGI_FORMAT_R32G32B32_SINT;
            case StreamFormat::Int4: return DXGI_FORMAT_R32G32B32A32_SINT;
            case StreamFormat::UInt: return DXGI_FORMAT_R32_UINT;
            case StreamFormat::UInt2: return DXGI_FORMAT_R32G32_UINT;
            case StreamFormat::UInt3: return DXGI_FORMAT_R32G32B32_UINT;
            case StreamFormat::UInt4: return DXGI_FORMAT_R32G32B32A32_UINT;
            case StreamFormat::Unorm8x4: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        return DXGI_FORMAT_UNKNOWN;
    }

    std::string ToString(const StreamFormat format)
    {
        switch (format)
        {
            case StreamFormat::Float: return "R32_FLOAT";
            case StreamFormat::Float2: return "R32G32_FLOAT";
            case StreamFormat::Float3: return "R32G32B32_FLOAT";
            case StreamFormat::Float4: return "R32G32B32A32_FLOAT";
            case StreamFormat::Int: return "R32_SINT";
            case StreamFormat::Int2: return "R32G32_SINT";
            case StreamFormat::Int3: return "R32G32B32_SINT";
            case StreamFormat::Int4: return "R32G32B32A32_SINT";
            case StreamFormat::UInt: return "R32_UINT";
            case StreamFormat::UInt2: return "R32G32_UINT";
            case StreamFormat::UInt3: return "R32G32B32_UINT";
            case StreamFormat::UInt4: return "R32G32B32A32_UINT";
            case StreamFormat::Unorm8x4: return "R8G8B8A8_UNORM";
        }
        return "?";
    }

    std::vector<D3D12_INPUT_ELEMENT_DESC> ToD3D12Elements(const InputLayoutBuild& build)
    {
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
        elements.reserve(build.elements.size());
        for (const InputElement& element : build.elements)
        {
            D3D12_INPUT_ELEMENT_DESC desc{};
            desc.SemanticName = element.semantic.c_str();
            desc.SemanticIndex = element.semanticIndex;
            desc.Format = ToDxgiFormat(element.format);
            desc.InputSlot = element.slot;
            desc.AlignedByteOffset = element.offset;
            desc.InputSlotClass = element.slot == 1 ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            desc.InstanceDataStepRate = element.slot == 1 ? 1 : 0;
            elements.push_back(desc);
        }
        return elements;
    }

    std::string FormatInputLayout(const InputLayoutBuild& build)
    {
        std::string text = "Input layout  stream '" + build.stream + "'  stride " + std::to_string(build.stride) + "  " +
            std::to_string(build.elements.size()) + " element(s)\n";
        for (std::size_t i = 0; i < build.elements.size(); ++i)
        {
            const InputElement& element = build.elements[i];
            std::string name = element.semantic + std::to_string(element.semanticIndex);
            std::string format = ToString(element.format);
            name.resize(std::max<std::size_t>(name.size(), 16), ' ');
            format.resize(std::max<std::size_t>(format.size(), 20), ' ');
            text += "  " + std::to_string(i) + "  " + name + format + "offset " + std::to_string(element.offset) +
                (element.slot == 1 ? "  per instance (slot 1)" : "") + "\n";
        }
        return text;
    }
}
