// ShaderDescriptor.h
//
// A *.shader.json: the source file, the entry points and profiles, and the states that the shader cannot say itself
// (the static samplers). One reader for the resource manager, the renderer's own programs and the layout dump.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <myengine/render/RenderTypes.h>

namespace myengine::render
{
    // {
    //   "source": "textured_lit.hlsl", "vertexEntry": "VSMain", "pixelEntry": "PSMain",
    //   "vertexProfile": "vs_5_0", "pixelProfile": "ps_5_0", "vertexStream": "mesh",
    //   "samplers": { "gSampler": "linear_wrap" }
    // }
    // `source` is made relative to the folder of the descriptor. false: `outError` says what is wrong.
    bool LoadShaderDescriptor(const std::filesystem::path& descriptorPath, ShaderProgramData& outProgram, std::string& outError);

    // The files that a shader source includes ("#include "x.hlsli"", relative to the including file), the includes of the
    // includes too, without repeats. A file that does not exist is listed too: creating it must reload the program.
    // `text` is the content of `sourceFile` when the caller has it already.
    std::vector<std::filesystem::path> ScanShaderIncludes(const std::filesystem::path& sourceFile, const std::string& text);
    std::vector<std::filesystem::path> ScanShaderIncludes(const std::filesystem::path& sourceFile);
}
