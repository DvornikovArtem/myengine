// ShaderLayout.cpp

#include <algorithm>
#include <cstdio>

#include <myengine/render/ShaderLayout.h>

namespace myengine::render
{
    namespace
    {
        // b / t / s / u: the register file of a resource. A texture and a sampler may both sit at index 0
        char RegisterClass(const ShaderResourceType type)
        {
            switch (type)
            {
                case ShaderResourceType::ConstantBuffer: return 'b';
                case ShaderResourceType::Texture:
                case ShaderResourceType::Buffer: return 't';
                case ShaderResourceType::Sampler: return 's';
                case ShaderResourceType::RWResource: return 'u';
            }
            return '?';
        }

        std::string RegisterText(const ShaderResourceType type, const std::uint32_t index, const std::uint32_t space)
        {
            std::string text(1, RegisterClass(type));
            text += std::to_string(index);
            if (space != 0)
            {
                text += ",space" + std::to_string(space);
            }
            return text;
        }

        bool SameVariable(const ShaderVariable& a, const ShaderVariable& b)
        {
            return a.name == b.name && a.type == b.type && a.offset == b.offset && a.size == b.size && a.arrayCount == b.arrayCount;
        }

        bool SameConstantBuffer(const ShaderConstantBuffer& a, const ShaderConstantBuffer& b)
        {
            return a.name == b.name && a.registerIndex == b.registerIndex && a.space == b.space && a.sizeBytes == b.sizeBytes &&
                a.variables.size() == b.variables.size() &&
                std::equal(a.variables.begin(), a.variables.end(), b.variables.begin(), SameVariable);
        }

        void AddConflict(std::vector<ShaderDiagnostic>& diagnostics, std::string message)
        {
            ShaderDiagnostic diagnostic;
            diagnostic.message = "layout conflict: " + std::move(message);
            diagnostics.push_back(std::move(diagnostic));
        }

        std::string Pad(std::string text, const std::size_t width)
        {
            if (text.size() < width)
            {
                text.append(width - text.size(), ' ');
            }
            return text;
        }
    }

    const ShaderResource* ShaderLayout::FindResourceByName(const std::string& name) const
    {
        for (const ShaderResource& resource : resources)
        {
            if (resource.name == name)
            {
                return &resource;
            }
        }
        return nullptr;
    }

    const ShaderConstantBuffer* ShaderLayout::FindConstantBuffer(const std::string& name) const
    {
        for (const ShaderConstantBuffer& buffer : constantBuffers)
        {
            if (buffer.name == name)
            {
                return &buffer;
            }
        }
        return nullptr;
    }

    ShaderVariableRef ShaderLayout::FindVariable(const std::string& name) const
    {
        for (const ShaderConstantBuffer& buffer : constantBuffers)
        {
            for (const ShaderVariable& variable : buffer.variables)
            {
                if (variable.name == name)
                {
                    return {&buffer, &variable};
                }
            }
        }
        return {};
    }

    std::string ToString(const ShaderStage stages)
    {
        const std::uint8_t mask = StageMask(stages);
        if (mask == 0)
        {
            return "-";
        }
        std::string text;
        if ((mask & StageMask(ShaderStage::Vertex)) != 0)
        {
            text = "VS";
        }
        if ((mask & StageMask(ShaderStage::Pixel)) != 0)
        {
            text += text.empty() ? "PS" : "+PS";
        }
        return text;
    }

    std::string ToString(const ShaderResourceType type)
    {
        switch (type)
        {
            case ShaderResourceType::ConstantBuffer: return "ConstantBuffer";
            case ShaderResourceType::Texture: return "Texture";
            case ShaderResourceType::Buffer: return "Buffer";
            case ShaderResourceType::Sampler: return "Sampler";
            case ShaderResourceType::RWResource: return "RWResource";
        }
        return "?";
    }

    std::string ToString(const ShaderValueType type)
    {
        switch (type)
        {
            case ShaderValueType::Unknown: return "Unknown";
            case ShaderValueType::Bool: return "Bool";
            case ShaderValueType::Int: return "Int";
            case ShaderValueType::Int2: return "Int2";
            case ShaderValueType::Int3: return "Int3";
            case ShaderValueType::Int4: return "Int4";
            case ShaderValueType::UInt: return "UInt";
            case ShaderValueType::UInt2: return "UInt2";
            case ShaderValueType::UInt3: return "UInt3";
            case ShaderValueType::UInt4: return "UInt4";
            case ShaderValueType::Float: return "Float";
            case ShaderValueType::Float2: return "Float2";
            case ShaderValueType::Float3: return "Float3";
            case ShaderValueType::Float4: return "Float4";
            case ShaderValueType::Float3x3: return "Float3x3";
            case ShaderValueType::Float4x4: return "Float4x4";
        }
        return "?";
    }

    std::string ToString(const VertexFormat format)
    {
        switch (format)
        {
            case VertexFormat::Unknown: return "Unknown";
            case VertexFormat::Float: return "Float";
            case VertexFormat::Float2: return "Float2";
            case VertexFormat::Float3: return "Float3";
            case VertexFormat::Float4: return "Float4";
            case VertexFormat::Int: return "Int";
            case VertexFormat::Int2: return "Int2";
            case VertexFormat::Int3: return "Int3";
            case VertexFormat::Int4: return "Int4";
            case VertexFormat::UInt: return "UInt";
            case VertexFormat::UInt2: return "UInt2";
            case VertexFormat::UInt3: return "UInt3";
            case VertexFormat::UInt4: return "UInt4";
        }
        return "?";
    }

    std::string FormatShaderDiagnostic(const ShaderDiagnostic& diagnostic)
    {
        std::string text = diagnostic.file;
        if (diagnostic.line != 0)
        {
            text += "(" + std::to_string(diagnostic.line);
            if (diagnostic.column != 0)
            {
                text += "," + std::to_string(diagnostic.column);
            }
            text += ")";
        }
        if (!text.empty())
        {
            text += ": ";
        }
        text += diagnostic.warning ? "warning" : "error";
        if (!diagnostic.code.empty())
        {
            text += " " + diagnostic.code;
        }
        text += ": " + diagnostic.message;
        return text;
    }

    ShaderLayout MergeShaderLayouts(const ShaderLayout& vertex, const ShaderLayout& pixel, std::vector<ShaderDiagnostic>& diagnostics)
    {
        ShaderLayout result;
        result.vertexInputs = vertex.vertexInputs; // the inputs of the pixel stage are the interpolants, not vertex data
        result.resources = vertex.resources;
        result.constantBuffers = vertex.constantBuffers;

        for (const ShaderResource& resource : pixel.resources)
        {
            const auto existing = std::find_if(result.resources.begin(), result.resources.end(),
                [&resource](const ShaderResource& other) { return other.name == resource.name; });
            if (existing == result.resources.end())
            {
                result.resources.push_back(resource);
                continue;
            }

            if (existing->type == resource.type && existing->registerIndex == resource.registerIndex &&
                existing->space == resource.space && existing->bindCount == resource.bindCount)
            {
                existing->stages = static_cast<std::uint8_t>(existing->stages | resource.stages);
            }
            else
            {
                AddConflict(diagnostics,
                    "'" + resource.name + "' is " + ToString(existing->type) + " " + RegisterText(existing->type, existing->registerIndex, existing->space) +
                    " (x" + std::to_string(existing->bindCount) + ") in " + ToString(static_cast<ShaderStage>(existing->stages)) + " but " +
                    ToString(resource.type) + " " + RegisterText(resource.type, resource.registerIndex, resource.space) +
                    " (x" + std::to_string(resource.bindCount) + ") in " + ToString(static_cast<ShaderStage>(resource.stages)));
            }
        }

        // One register under different names: the program cannot bind both
        for (std::size_t i = 0; i < result.resources.size(); ++i)
        {
            for (std::size_t j = i + 1; j < result.resources.size(); ++j)
            {
                const ShaderResource& a = result.resources[i];
                const ShaderResource& b = result.resources[j];
                if (a.name != b.name && RegisterClass(a.type) == RegisterClass(b.type) && a.space == b.space &&
                    a.registerIndex < b.registerIndex + b.bindCount && b.registerIndex < a.registerIndex + a.bindCount)
                {
                    AddConflict(diagnostics,
                        "register " + RegisterText(a.type, a.registerIndex, a.space) + " is '" + a.name + "' in " +
                        ToString(static_cast<ShaderStage>(a.stages)) + " but '" + b.name + "' in " + ToString(static_cast<ShaderStage>(b.stages)));
                }
            }
        }

        for (const ShaderConstantBuffer& buffer : pixel.constantBuffers)
        {
            const auto existing = std::find_if(result.constantBuffers.begin(), result.constantBuffers.end(),
                [&buffer](const ShaderConstantBuffer& other) { return other.name == buffer.name; });
            if (existing == result.constantBuffers.end())
            {
                result.constantBuffers.push_back(buffer);
            }
            else if (SameConstantBuffer(*existing, buffer))
            {
                existing->stages = static_cast<std::uint8_t>(existing->stages | buffer.stages);
            }
            else
            {
                AddConflict(diagnostics,
                    "constant buffer '" + buffer.name + "' differs between the stages (" +
                    std::to_string(existing->sizeBytes) + " bytes, " + std::to_string(existing->variables.size()) + " variables in " +
                    ToString(static_cast<ShaderStage>(existing->stages)) + "; " + std::to_string(buffer.sizeBytes) + " bytes, " +
                    std::to_string(buffer.variables.size()) + " variables in " + ToString(static_cast<ShaderStage>(buffer.stages)) + ")");
            }
        }

        // The same variable name in two buffers: the material finds a variable by name
        for (std::size_t i = 0; i < result.constantBuffers.size(); ++i)
        {
            for (std::size_t j = i + 1; j < result.constantBuffers.size(); ++j)
            {
                for (const ShaderVariable& first : result.constantBuffers[i].variables)
                {
                    for (const ShaderVariable& second : result.constantBuffers[j].variables)
                    {
                        if (first.name == second.name)
                        {
                            AddConflict(diagnostics,
                                "variable '" + first.name + "' is declared in constant buffers '" + result.constantBuffers[i].name +
                                "' and '" + result.constantBuffers[j].name + "'");
                        }
                    }
                }
            }
        }
        return result;
    }

    std::string FormatShaderLayout(const ShaderLayout& layout)
    {
        std::string text;

        text += "Resources (" + std::to_string(layout.resources.size()) + "):\n";
        for (const ShaderResource& resource : layout.resources)
        {
            std::string registerText = RegisterText(resource.type, resource.registerIndex, resource.space);
            if (resource.bindCount != 1)
            {
                registerText += "[" + std::to_string(resource.bindCount) + "]";
            }
            text += "  " + Pad(resource.name, 20) + Pad(ToString(resource.type), 16) + Pad(registerText, 10) +
                ToString(static_cast<ShaderStage>(resource.stages)) + "\n";
        }

        text += "Constant buffers (" + std::to_string(layout.constantBuffers.size()) + "):\n";
        for (const ShaderConstantBuffer& buffer : layout.constantBuffers)
        {
            text += "  " + buffer.name + "  " + RegisterText(ShaderResourceType::ConstantBuffer, buffer.registerIndex, buffer.space) +
                "  " + std::to_string(buffer.sizeBytes) + " bytes  " + ToString(static_cast<ShaderStage>(buffer.stages)) + "\n";
            for (const ShaderVariable& variable : buffer.variables)
            {
                char numbers[64];
                std::snprintf(numbers, sizeof(numbers), "offset %4u  size %4u", variable.offset, variable.size);
                text += "    " + Pad(variable.name, 20) + Pad(ToString(variable.type), 10) + numbers;
                if (variable.arrayCount != 1)
                {
                    text += "  [" + std::to_string(variable.arrayCount) + "]";
                }
                text += "\n";
            }
        }

        text += "Vertex inputs (" + std::to_string(layout.vertexInputs.size()) + "):\n";
        for (std::size_t i = 0; i < layout.vertexInputs.size(); ++i)
        {
            const VertexInput& input = layout.vertexInputs[i];
            text += "  " + std::to_string(i) + "  " + Pad(input.semantic + std::to_string(input.semanticIndex), 16) + ToString(input.format) + "\n";
        }
        return text;
    }
}
