// ShaderMetadata.h
//
// What the author of a shader says about its parameters, in comments of the HLSL:
//
//     // @label "Roughness" @min 0 @max 1 @step 0.01 @default 0.5
//     float gRoughness;
//     float4 gColor;   // @color @label "Tint" @default 1 1 1 1
//
// The comment is on the line of the declaration, or on the lines right above it (several lines add up). The reflection
// does not know any of it: the source is read (the same file and #include files that the compiler read) and the tags
// are put into the ShaderLayout by the name of the variable or the texture. No DirectX types.
// A mistake in a comment is a warning with file and line, never an error: the shader still compiles and reloads.
// docs/render/b2-shader-metadata.md.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <myengine/render/ShaderLayout.h>

namespace myengine::render
{
    // `file`: the path of the source (its #include "x" files are read relative to the folder of the file that includes them);
    // empty when the text is not in a file, then no include is read. `sourceName`: what the messages call the file.
    // `layout`: the merged layout of the program; the metadata of its variables and textures is filled in.
    // Not evaluated: #if / #ifdef (a declaration in a disabled branch is read like any other).
    void ApplyShaderMetadata(const std::filesystem::path& file, const std::string& source, const std::string& sourceName, ShaderLayout& layout, std::vector<ShaderDiagnostic>& diagnostics);
}
