// ShaderReflector.cpp

#include "ShaderReflector.h"

#include <algorithm>

#include <d3d11shader.h>

#include <myengine/render/ShaderMetadata.h>

#include <tracy/Tracy.hpp>

#include "ShaderCompiler.h"

namespace myengine::render::dx12
{
    namespace
    {
        ShaderDiagnostic MakeError(const ShaderStage stage, std::string message)
        {
            ShaderDiagnostic diagnostic;
            diagnostic.stage = StageMask(stage);
            diagnostic.message = std::move(message);
            return diagnostic;
        }

        bool HasError(const std::vector<ShaderDiagnostic>& diagnostics)
        {
            return std::any_of(diagnostics.begin(), diagnostics.end(), [](const ShaderDiagnostic& d) { return !d.warning; });
        }

        ShaderValueType MapValueType(const D3D11_SHADER_TYPE_DESC& type)
        {
            if (type.Class == D3D_SVC_STRUCT || type.Members > 0)
            {
                return ShaderValueType::Unknown;
            }

            if (type.Class == D3D_SVC_MATRIX_ROWS || type.Class == D3D_SVC_MATRIX_COLUMNS)
            {
                if (type.Type != D3D_SVT_FLOAT)
                {
                    return ShaderValueType::Unknown;
                }
                if (type.Rows == 4 && type.Columns == 4)
                {
                    return ShaderValueType::Float4x4;
                }
                if (type.Rows == 3 && type.Columns == 3)
                {
                    return ShaderValueType::Float3x3;
                }
                return ShaderValueType::Unknown;
            }

            if (type.Class != D3D_SVC_SCALAR && type.Class != D3D_SVC_VECTOR)
            {
                return ShaderValueType::Unknown;
            }
            const UINT components = type.Class == D3D_SVC_SCALAR ? 1u : type.Columns;
            if (components < 1 || components > 4)
            {
                return ShaderValueType::Unknown;
            }

            const auto pick = [components](const ShaderValueType one, const ShaderValueType two, const ShaderValueType three, const ShaderValueType four)
            {
                switch (components)
                {
                    case 1: return one;
                    case 2: return two;
                    case 3: return three;
                    default: return four;
                }
            };
            switch (type.Type)
            {
                case D3D_SVT_BOOL:
                    return components == 1 ? ShaderValueType::Bool : ShaderValueType::Unknown;
                case D3D_SVT_INT:
                    return pick(ShaderValueType::Int, ShaderValueType::Int2, ShaderValueType::Int3, ShaderValueType::Int4);
                case D3D_SVT_UINT:
                    return pick(ShaderValueType::UInt, ShaderValueType::UInt2, ShaderValueType::UInt3, ShaderValueType::UInt4);
                case D3D_SVT_FLOAT:
                    return pick(ShaderValueType::Float, ShaderValueType::Float2, ShaderValueType::Float3, ShaderValueType::Float4);
                default:
                    return ShaderValueType::Unknown;
            }
        }

        // The components of an input: the bits of the xyzw mask
        int ComponentCount(const BYTE mask)
        {
            int count = 0;
            for (int bit = 0; bit < 4; ++bit)
            {
                count += (mask >> bit) & 1;
            }
            return count;
        }

        VertexFormat MapVertexFormat(const D3D_REGISTER_COMPONENT_TYPE componentType, const int components)
        {
            if (components < 1 || components > 4)
            {
                return VertexFormat::Unknown;
            }
            const auto pick = [components](const VertexFormat one, const VertexFormat two, const VertexFormat three, const VertexFormat four)
            {
                switch (components)
                {
                    case 1: return one;
                    case 2: return two;
                    case 3: return three;
                    default: return four;
                }
            };
            switch (componentType)
            {
                case D3D_REGISTER_COMPONENT_FLOAT32:
                    return pick(VertexFormat::Float, VertexFormat::Float2, VertexFormat::Float3, VertexFormat::Float4);
                case D3D_REGISTER_COMPONENT_SINT32:
                    return pick(VertexFormat::Int, VertexFormat::Int2, VertexFormat::Int3, VertexFormat::Int4);
                case D3D_REGISTER_COMPONENT_UINT32:
                    return pick(VertexFormat::UInt, VertexFormat::UInt2, VertexFormat::UInt3, VertexFormat::UInt4);
                default:
                    return VertexFormat::Unknown;
            }
        }

        bool MapResourceType(const D3D11_SHADER_INPUT_BIND_DESC& bind, ShaderResourceType& outType)
        {
            switch (bind.Type)
            {
                case D3D_SIT_CBUFFER:
                    outType = ShaderResourceType::ConstantBuffer;
                    return true;
                case D3D_SIT_TEXTURE:
                    outType = (bind.Dimension == D3D_SRV_DIMENSION_BUFFER || bind.Dimension == D3D_SRV_DIMENSION_BUFFEREX)
                        ? ShaderResourceType::Buffer
                        : ShaderResourceType::Texture;
                    return true;
                case D3D_SIT_TBUFFER:
                case D3D_SIT_STRUCTURED:
                case D3D_SIT_BYTEADDRESS:
                    outType = ShaderResourceType::Buffer;
                    return true;
                case D3D_SIT_SAMPLER:
                    outType = ShaderResourceType::Sampler;
                    return true;
                case D3D_SIT_UAV_RWTYPED:
                case D3D_SIT_UAV_RWSTRUCTURED:
                case D3D_SIT_UAV_RWBYTEADDRESS:
                case D3D_SIT_UAV_APPEND_STRUCTURED:
                case D3D_SIT_UAV_CONSUME_STRUCTURED:
                case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
                    outType = ShaderResourceType::RWResource;
                    return true;
                default:
                    return false;
            }
        }
    }

    bool ReflectShader(const void* bytecode, const std::size_t size, const ShaderStage stage, ShaderLayout& outLayout, std::vector<ShaderDiagnostic>& diagnostics)
    {
        ZoneScopedN("Shader::Reflect");

        outLayout = {};
        Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
        const HRESULT hr = D3DReflect(bytecode, size, IID_PPV_ARGS(reflection.GetAddressOf()));
        if (FAILED(hr) || reflection == nullptr)
        {
            char code[32];
            std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
            diagnostics.push_back(MakeError(stage, std::string("D3DReflect failed (") + code + ")"));
            return false;
        }

        D3D11_SHADER_DESC shaderDesc{};
        if (FAILED(reflection->GetDesc(&shaderDesc)))
        {
            diagnostics.push_back(MakeError(stage, "ID3D11ShaderReflection::GetDesc failed"));
            return false;
        }

        const UINT shaderType = D3D11_SHVER_GET_TYPE(shaderDesc.Version);
        const UINT expectedType = stage == ShaderStage::Vertex ? D3D11_SHVER_VERTEX_SHADER : D3D11_SHVER_PIXEL_SHADER;
        if (shaderType != expectedType)
        {
            diagnostics.push_back(MakeError(stage, std::string("the profile of the entry point is not a ") + (stage == ShaderStage::Vertex ? "vertex" : "pixel") + " shader profile"));
            return false;
        }

        const std::uint8_t stageMask = StageMask(stage);

        for (UINT i = 0; i < shaderDesc.BoundResources; ++i)
        {
            D3D11_SHADER_INPUT_BIND_DESC bind{};
            if (FAILED(reflection->GetResourceBindingDesc(i, &bind)))
            {
                diagnostics.push_back(MakeError(stage, "GetResourceBindingDesc failed for resource " + std::to_string(i)));
                return false;
            }

            ShaderResource resource;
            resource.name = bind.Name != nullptr ? bind.Name : "";
            resource.registerIndex = bind.BindPoint;
            resource.space = 0; // SM 5.0 has no spaces and D3D11_SHADER_INPUT_BIND_DESC has no field for them
            resource.bindCount = bind.BindCount;
            resource.stages = stageMask;
            if (!MapResourceType(bind, resource.type))
            {
                ShaderDiagnostic warning = MakeError(stage, "resource '" + resource.name + "' has a type that the layout does not know and is skipped");
                warning.warning = true;
                diagnostics.push_back(std::move(warning));
                continue;
            }
            outLayout.resources.push_back(std::move(resource));
        }

        for (UINT i = 0; i < shaderDesc.ConstantBuffers; ++i)
        {
            ID3D11ShaderReflectionConstantBuffer* buffer = reflection->GetConstantBufferByIndex(i);
            D3D11_SHADER_BUFFER_DESC bufferDesc{};
            if (buffer == nullptr || FAILED(buffer->GetDesc(&bufferDesc)))
            {
                diagnostics.push_back(MakeError(stage, "GetDesc failed for constant buffer " + std::to_string(i)));
                return false;
            }
            if (bufferDesc.Type != D3D_CT_CBUFFER)
            {
                continue; // tbuffer and the binding info are not constant buffers
            }

            ShaderConstantBuffer constantBuffer;
            constantBuffer.name = bufferDesc.Name != nullptr ? bufferDesc.Name : "";
            constantBuffer.sizeBytes = bufferDesc.Size;
            constantBuffer.stages = stageMask;

            const auto bound = std::find_if(outLayout.resources.begin(), outLayout.resources.end(),
                [&constantBuffer](const ShaderResource& resource)
                { return resource.type == ShaderResourceType::ConstantBuffer && resource.name == constantBuffer.name; });
            if (bound == outLayout.resources.end())
            {
                continue; // declared but not bound: nothing to put into a slot
            }
            constantBuffer.registerIndex = bound->registerIndex;
            constantBuffer.space = bound->space;

            for (UINT v = 0; v < bufferDesc.Variables; ++v)
            {
                ID3D11ShaderReflectionVariable* variable = buffer->GetVariableByIndex(v);
                D3D11_SHADER_VARIABLE_DESC variableDesc{};
                D3D11_SHADER_TYPE_DESC typeDesc{};
                ID3D11ShaderReflectionType* type = variable != nullptr ? variable->GetType() : nullptr;
                if (variable == nullptr || type == nullptr || FAILED(variable->GetDesc(&variableDesc)) || FAILED(type->GetDesc(&typeDesc)))
                {
                    diagnostics.push_back(MakeError(stage, "GetDesc failed for variable " + std::to_string(v) + " of constant buffer '" + constantBuffer.name + "'"));
                    return false;
                }

                ShaderVariable shaderVariable;
                shaderVariable.name = variableDesc.Name != nullptr ? variableDesc.Name : "";
                shaderVariable.type = MapValueType(typeDesc);
                shaderVariable.offset = variableDesc.StartOffset;
                shaderVariable.size = variableDesc.Size;
                shaderVariable.arrayCount = typeDesc.Elements != 0 ? typeDesc.Elements : 1;
                constantBuffer.variables.push_back(std::move(shaderVariable));
            }
            outLayout.constantBuffers.push_back(std::move(constantBuffer));
        }

        if (stage == ShaderStage::Vertex)
        {
            for (UINT i = 0; i < shaderDesc.InputParameters; ++i)
            {
                D3D11_SIGNATURE_PARAMETER_DESC parameter{};
                if (FAILED(reflection->GetInputParameterDesc(i, &parameter)))
                {
                    diagnostics.push_back(MakeError(stage, "GetInputParameterDesc failed for input " + std::to_string(i)));
                    return false;
                }
                if (parameter.SystemValueType != D3D_NAME_UNDEFINED)
                {
                    continue; // SV_VertexID, SV_InstanceID: not data from a vertex buffer
                }

                VertexInput input;
                input.semantic = parameter.SemanticName != nullptr ? parameter.SemanticName : "";
                input.semanticIndex = parameter.SemanticIndex;
                input.format = MapVertexFormat(parameter.ComponentType, ComponentCount(parameter.Mask));
                outLayout.vertexInputs.push_back(std::move(input));
            }
        }
        return true;
    }

    ShaderCompileResult ShaderReflector::CompileProgram(const ShaderProgramData& program)
    {
        ShaderCompileResult result;
        std::string source;
        if (!ReadShaderSource(program.sourcePath, source, result.diagnostics))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.failures;
            return result;
        }
        return Build(program.sourcePath.string(), source, program.sourcePath, program);
    }

    ShaderCompileResult ShaderReflector::CompileProgramFromSource(const std::string& name, const std::string& source, const ShaderProgramData& program)
    {
        return Build(name, source, std::filesystem::path(), program);
    }

    ShaderCompileResult ShaderReflector::Build(const std::string& name, const std::string& source, const std::filesystem::path& file, const ShaderProgramData& program)
    {
        // The identity: what the program is (not the text): a new text of the same program replaces the old one
        std::uint64_t identity = HashBytes(name.data(), name.size());
        for (const std::string* part : {&program.vertexEntry, &program.pixelEntry, &program.vertexProfile, &program.pixelProfile})
        {
            identity = HashBytes("\x1f", 1, identity);
            identity = HashBytes(part->data(), part->size(), identity);
        }
        for (const auto& define : program.defines)
        {
            identity = HashBytes("\x1e", 1, identity);
            identity = HashBytes(define.first.data(), define.first.size(), identity);
            identity = HashBytes("=", 1, identity);
            identity = HashBytes(define.second.data(), define.second.size(), identity);
        }
        const std::uint64_t sourceHash = file.empty() ? HashBytes(source.data(), source.size()) : HashShaderSource(file, source);

        ShaderCompileResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto cached = cache_.find(identity);
            if (cached != cache_.end() && cached->second.sourceHash == sourceHash)
            {
                ++stats_.cacheHits;
                result.program = cached->second.program;
                result.ok = true;
                result.fromCache = true;
                return result;
            }
            ++stats_.compiles;
        }

        // Both stages are compiled even when the first one fails: the author sees all the errors at once
        auto compiled = std::make_shared<CompiledShaderProgram>();
        compiled->sourceName = name;
        compiled->sourceHash = sourceHash;

        ShaderStageSource stageSource;
        stageSource.sourceName = name;
        stageSource.sourcePath = file;
        stageSource.source = &source;
        stageSource.defines = program.defines;

        stageSource.entry = program.vertexEntry;
        stageSource.profile = program.vertexProfile;
        stageSource.stage = ShaderStage::Vertex;
        const bool vertexCompiled = CompileShaderStage(stageSource, compiled->vertexBlob, result.diagnostics);

        stageSource.entry = program.pixelEntry;
        stageSource.profile = program.pixelProfile;
        stageSource.stage = ShaderStage::Pixel;
        const bool pixelCompiled = CompileShaderStage(stageSource, compiled->pixelBlob, result.diagnostics);

        bool ok = vertexCompiled && pixelCompiled;
        if (ok)
        {
            ok = ReflectShader(compiled->vertexBlob->GetBufferPointer(), compiled->vertexBlob->GetBufferSize(), ShaderStage::Vertex, compiled->vertexLayout, result.diagnostics);
            ok = ReflectShader(compiled->pixelBlob->GetBufferPointer(), compiled->pixelBlob->GetBufferSize(), ShaderStage::Pixel, compiled->pixelLayout, result.diagnostics) && ok;
        }
        if (ok)
        {
            std::vector<ShaderDiagnostic> conflicts;
            compiled->layout = MergeShaderLayouts(compiled->vertexLayout, compiled->pixelLayout, conflicts);
            for (ShaderDiagnostic& conflict : conflicts)
            {
                conflict.file = name;
            }
            result.diagnostics.insert(result.diagnostics.end(), conflicts.begin(), conflicts.end());
            ok = !HasError(conflicts);
        }
        if (ok)
        {
            // The comments of the source ("// @label ...") say more about the variables and the textures than the reflection
            // does; a mistake in them is a warning and never fails the build of the program
            ApplyShaderMetadata(file, source, name, compiled->layout, result.diagnostics);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok)
        {
            ++stats_.failures;
            return result; // a failed build leaves the cached older version where it is
        }
        result.ok = true;
        result.program = compiled;
        CacheEntry entry;
        entry.sourceName = name;
        entry.sourceHash = sourceHash;
        entry.program = compiled;
        cache_[identity] = std::move(entry);
        return result;
    }

    void ShaderReflector::Invalidate(const std::filesystem::path& sourcePath)
    {
        const std::string name = sourcePath.string();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = cache_.begin(); it != cache_.end();)
        {
            it = it->second.sourceName == name ? cache_.erase(it) : std::next(it);
        }
    }

    void ShaderReflector::Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }

    ShaderReflectorStats ShaderReflector::Stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    std::size_t ShaderReflector::CachedProgramCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }
}
