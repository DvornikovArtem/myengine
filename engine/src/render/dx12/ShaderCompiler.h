// ShaderCompiler.h
//
// HLSL -> DXBC (fxc, SM 5.0) with structured messages: file, line, column. No device is needed, so it works on any
// thread and in tests. docs/render/architecture.md, section 3.3.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <myengine/render/ShaderLayout.h>

namespace myengine::render::dx12
{
    struct ShaderStageSource
    {
        std::string sourceName;                                      // what the messages call the file (the path)
        const std::string* source = nullptr;                         // the text of the file
        std::string entry;
        std::string profile;                                         // "vs_5_0"
        ShaderStage stage = ShaderStage::Vertex;
        std::vector<std::pair<std::string, std::string>> defines;    // #define name value
    };

    // Reads a whole shader file. false: the diagnostic says why (no file, empty file)
    bool ReadShaderSource(const std::filesystem::path& path, std::string& outText, std::vector<ShaderDiagnostic>& diagnostics);

    // Compiles one entry point. The diagnostics get the errors and the warnings of the compiler. false: no blob.
    bool CompileShaderStage(const ShaderStageSource& request, Microsoft::WRL::ComPtr<ID3DBlob>& outBlob, std::vector<ShaderDiagnostic>& diagnostics);

    // The messages of fxc: "file(12,5-9): error X3004: text", "error X3501: 'PSMain': entrypoint not found".
    // `fallbackFile` goes to the messages without a file, `stage` to all of them.
    std::vector<ShaderDiagnostic> ParseCompilerMessages(const std::string& text, const std::string& fallbackFile, ShaderStage stage);

    // A hash of the text and of the files that it includes ("#include "x.hlsli"", relative to the file)
    std::uint64_t HashShaderSource(const std::filesystem::path& file, const std::string& text);

    // FNV-1a, to mix several parts into one hash
    std::uint64_t HashBytes(const void* data, std::size_t size, std::uint64_t seed = 14695981039346656037ull);
}
