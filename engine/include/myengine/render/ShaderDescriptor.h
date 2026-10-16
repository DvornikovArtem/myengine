// ShaderDescriptor.h
//
// A *.shader.json: the source file, the entry points and profiles, and the states that the shader cannot say itself
// (the static samplers). One reader for the resource manager, the renderer's own programs and the layout dump.

#pragma once

#include <filesystem>
#include <string>

#include <myengine/render/RenderTypes.h>

namespace myengine::render
{
    // {
    //   "source": "textured_lit.hlsl", "vertexEntry": "VSMain", "pixelEntry": "PSMain",
    //   "vertexProfile": "vs_5_0", "pixelProfile": "ps_5_0",
    //   "samplers": { "gSampler": "linear_wrap" }
    // }
    // `source` is made relative to the folder of the descriptor. false: `outError` says what is wrong.
    bool LoadShaderDescriptor(const std::filesystem::path& descriptorPath, ShaderProgramData& outProgram, std::string& outError);
}
