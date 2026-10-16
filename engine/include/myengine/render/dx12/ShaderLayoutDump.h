// ShaderLayoutDump.h
//
// A text table of the layouts of all the shader programs in a folder: for the acceptance of the reflection and for
// people who write materials (myengine.exe --dump-shader-layouts <file>). No device is needed.

#pragma once

#include <filesystem>
#include <string>

namespace myengine::render::dx12
{
    // Every *.shader.json of the folder is a program (source, entries, profiles from it); every other *.hlsl that no
    // .shader.json names is a program with the entries VSMain and PSMain. The text gets the layout of each, or its errors
    // (file, line, column). Returns the number of programs that could not be compiled.
    int DumpShaderLayouts(const std::filesystem::path& shadersDirectory, std::string& outText);
}
