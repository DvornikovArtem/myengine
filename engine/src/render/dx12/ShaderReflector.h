// ShaderReflector.h
//
// D3DReflect on compiled shaders -> render::ShaderLayout, and a cache of compiled programs: a program is compiled and
// reflected once per (file, hash of the text and of its includes, entries, profiles, defines).
// docs/render/architecture.md, sections 3.2, 3.3.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderLayout.h>

namespace myengine::render::dx12
{
    // One stage of a program from the DXBC: the layout of its resources, constant buffers and (VS) inputs.
    // false: the diagnostics say what could not be read.
    bool ReflectShader(const void* bytecode, std::size_t size, ShaderStage stage, ShaderLayout& outLayout, std::vector<ShaderDiagnostic>& diagnostics);

    // A compiled and reflected program; shared by the cache and the caller
    struct CompiledShaderProgram
    {
        Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> pixelBlob;
        ShaderLayout layout;          // VS + PS together
        ShaderLayout vertexLayout;    // each stage alone
        ShaderLayout pixelLayout;
        std::string sourceName;
        std::uint64_t sourceHash = 0; // the text and its includes
    };

    struct ShaderCompileResult
    {
        std::shared_ptr<const CompiledShaderProgram> program; // null when `ok` is false
        std::vector<ShaderDiagnostic> diagnostics;            // errors and warnings, also on success (warnings)
        bool ok = false;
        bool fromCache = false;
    };

    struct ShaderReflectorStats
    {
        std::uint64_t compiles = 0;   // programs compiled (a cache miss)
        std::uint64_t cacheHits = 0;
        std::uint64_t failures = 0;
    };

    class ShaderReflector
    {
    public:
        // Reads the file, compiles both entry points and merges the layouts. A second call with the same file content
        // and parameters returns the cached program.
        ShaderCompileResult CompileProgram(const ShaderProgramData& program);

        // The same for a text that is not in a file (tests, built-in shaders). `name` is what the messages call it.
        ShaderCompileResult CompileProgramFromSource(const std::string& name, const std::string& source, const ShaderProgramData& program);

        // Forgets the programs compiled from this file (hot reload: the file changed, an include may have too)
        void Invalidate(const std::filesystem::path& sourcePath);
        void Clear();

        ShaderReflectorStats Stats() const;
        std::size_t CachedProgramCount() const;

    private:
        struct CacheEntry
        {
            std::string sourceName;
            std::uint64_t sourceHash = 0;
            std::shared_ptr<const CompiledShaderProgram> program;
        };

        ShaderCompileResult Build(const std::string& name, const std::string& source, const std::filesystem::path& file, const ShaderProgramData& program);

        mutable std::mutex mutex_;
        // Key: the identity of the program (file, entries, profiles, defines). One entry per identity: a changed text
        // replaces the old compiled version
        std::unordered_map<std::uint64_t, CacheEntry> cache_;
        ShaderReflectorStats stats_;
    };
}
