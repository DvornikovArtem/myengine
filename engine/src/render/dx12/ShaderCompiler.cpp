// ShaderCompiler.cpp

#include "ShaderCompiler.h"

#include <myengine/render/ShaderDescriptor.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>

#include <tracy/Tracy.hpp>

namespace myengine::render::dx12
{
    namespace
    {
        ShaderDiagnostic MakeDiagnostic(const std::string& file, const ShaderStage stage, std::string message)
        {
            ShaderDiagnostic diagnostic;
            diagnostic.file = file;
            diagnostic.stage = StageMask(stage);
            diagnostic.message = std::move(message);
            return diagnostic;
        }
    }

    std::uint64_t HashBytes(const void* data, const std::size_t size, std::uint64_t seed)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i)
        {
            seed ^= bytes[i];
            seed *= 1099511628211ull;
        }
        return seed;
    }

    std::uint64_t HashShaderSource(const std::filesystem::path& file, const std::string& text)
    {
        std::uint64_t hash = HashBytes(text.data(), text.size());
        for (const std::filesystem::path& included : ScanShaderIncludes(file, text))
        {
            std::ifstream stream(included, std::ios::binary);
            if (!stream.is_open())
            {
                continue; // the compiler reports a missing include
            }
            const std::string includedText((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            hash = HashBytes(includedText.data(), includedText.size(), hash);
        }
        return hash;
    }

    bool ReadShaderSource(const std::filesystem::path& path, std::string& outText, std::vector<ShaderDiagnostic>& diagnostics)
    {
        std::ifstream sourceFile(path, std::ios::binary);
        if (!sourceFile.is_open())
        {
            diagnostics.push_back(MakeDiagnostic(path.string(), ShaderStage::None, "shader source file could not be opened"));
            return false;
        }
        outText.assign((std::istreambuf_iterator<char>(sourceFile)), std::istreambuf_iterator<char>());
        if (outText.empty())
        {
            diagnostics.push_back(MakeDiagnostic(path.string(), ShaderStage::None, "shader source file is empty"));
            return false;
        }
        return true;
    }

    std::vector<ShaderDiagnostic> ParseCompilerMessages(const std::string& text, const std::string& fallbackFile, const ShaderStage stage)
    {
        // The position form: file(line), file(line,column), file(line,column-lastColumn)
        static const std::regex positioned(R"(^(.*)\((\d+)(?:,(\d+)(?:-\d+)?)?\)\s*:\s*(error|warning)\s+(\w+)\s*:\s*(.*)$)");
        // No position: "error X3501: 'PSMain': entrypoint not found", maybe with a file in front
        static const std::regex plain(R"(^(?:(.*?)\s*:\s*)?(error|warning)\s+([A-Za-z]\d+)\s*:\s*(.*)$)");

        std::vector<ShaderDiagnostic> result;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }

            std::smatch match;
            ShaderDiagnostic diagnostic;
            diagnostic.stage = StageMask(stage);
            if (std::regex_match(line, match, positioned))
            {
                diagnostic.file = match[1].str();
                diagnostic.line = static_cast<std::uint32_t>(std::stoul(match[2].str()));
                diagnostic.column = match[3].matched ? static_cast<std::uint32_t>(std::stoul(match[3].str())) : 0;
                diagnostic.warning = match[4].str() == "warning";
                diagnostic.code = match[5].str();
                diagnostic.message = match[6].str();
            }
            else if (std::regex_match(line, match, plain))
            {
                diagnostic.file = match[1].matched && !match[1].str().empty() ? match[1].str() : fallbackFile;
                diagnostic.warning = match[2].str() == "warning";
                diagnostic.code = match[3].str();
                diagnostic.message = match[4].str();
            }
            else
            {
                // A continuation of the previous message ("note", the text of the line); without one it is a message of its own
                if (!result.empty())
                {
                    result.back().message += "\n" + line;
                    continue;
                }
                diagnostic.file = fallbackFile;
                diagnostic.message = line;
            }
            result.push_back(std::move(diagnostic));
        }
        return result;
    }

    bool CompileShaderStage(const ShaderStageSource& request, Microsoft::WRL::ComPtr<ID3DBlob>& outBlob, std::vector<ShaderDiagnostic>& diagnostics)
    {
        ZoneScopedN("Shader::Compile");

        if (request.source == nullptr)
        {
            diagnostics.push_back(MakeDiagnostic(request.sourceName, request.stage, "no shader source"));
            return false;
        }

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        std::vector<D3D_SHADER_MACRO> macros;
        macros.reserve(request.defines.size() + 1);
        for (const auto& define : request.defines)
        {
            macros.push_back({define.first.c_str(), define.second.c_str()});
        }
        macros.push_back({nullptr, nullptr});

        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        const HRESULT hr = D3DCompile(
            request.source->data(),
            request.source->size(),
            request.sourceName.c_str(),
            macros.data(),
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            request.entry.c_str(),
            request.profile.c_str(),
            compileFlags,
            0,
            outBlob.ReleaseAndGetAddressOf(),
            errorBlob.GetAddressOf());

        std::vector<ShaderDiagnostic> messages;
        if (errorBlob != nullptr && errorBlob->GetBufferSize() > 0)
        {
            messages = ParseCompilerMessages(
                std::string(static_cast<const char*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize()), request.sourceName, request.stage);
        }

        if (FAILED(hr))
        {
            outBlob.Reset();
            const bool hasError = std::any_of(messages.begin(), messages.end(), [](const ShaderDiagnostic& d) { return !d.warning; });
            if (!hasError)
            {
                char code[32];
                std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
                messages.push_back(MakeDiagnostic(request.sourceName, request.stage,
                    std::string("D3DCompile failed (") + code + ") for entry '" + request.entry + "', profile '" + request.profile + "'"));
            }
            diagnostics.insert(diagnostics.end(), messages.begin(), messages.end());
            return false;
        }

        diagnostics.insert(diagnostics.end(), messages.begin(), messages.end());
        return outBlob != nullptr;
    }
}
