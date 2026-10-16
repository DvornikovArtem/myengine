// ShaderLayoutDump.cpp

#include <myengine/render/dx12/ShaderLayoutDump.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderDescriptor.h>
#include <myengine/render/ShaderLayout.h>
#include <myengine/render/dx12/InputLayoutBuilder.h>
#include <myengine/render/dx12/RootSignatureBuilder.h>

#include "ShaderReflector.h"

namespace myengine::render::dx12
{
    namespace
    {
        // The compiler gives its messages in the ANSI code page; the dump file and the editor window are UTF-8
        std::string AnsiToUtf8(const std::string& text)
        {
            if (text.empty())
            {
                return text;
            }
            const int wideLength = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (wideLength <= 0)
            {
                return text;
            }
            std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
            MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), wideLength);
            const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
            if (utf8Length <= 0)
            {
                return text;
            }
            std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, utf8.data(), utf8Length, nullptr, nullptr);
            return utf8;
        }

        // The programs by the hash of the root signature shape: who shares a root signature with whom
        using ShapeUsers = std::map<std::uint64_t, std::vector<std::string>>;

        void DumpProgram(ShaderReflector& reflector, const std::string& title, const ShaderProgramData& program, std::string& text, int& failures, ShapeUsers& shapes)
        {
            text += "=== " + title + " ===\n";
            text += "source: " + program.sourcePath.generic_u8string() + "  VS " + program.vertexEntry + "/" + program.vertexProfile +
                "  PS " + program.pixelEntry + "/" + program.pixelProfile + "\n";
            const ShaderCompileResult result = reflector.CompileProgram(program);
            for (const ShaderDiagnostic& diagnostic : result.diagnostics)
            {
                text += AnsiToUtf8(FormatShaderDiagnostic(diagnostic)) + "\n";
            }
            if (!result.ok)
            {
                ++failures;
                text += "NOT COMPILED\n\n";
                return;
            }
            text += FormatShaderLayout(result.program->layout);

            // What the layout becomes: the root signature by the rules of the builder
            const RootSignatureBuild build = BuildRootSignatureDescription(result.program->layout, program.samplers, title);
            for (const ShaderDiagnostic& diagnostic : build.diagnostics)
            {
                text += AnsiToUtf8(FormatShaderDiagnostic(diagnostic)) + "\n";
            }
            if (!build.ok)
            {
                ++failures;
                text += "NO ROOT SIGNATURE\n\n";
                return;
            }
            text += FormatRootSignature(build.description);
            shapes[build.description.shapeHash].push_back(title);

            // The input layout: the inputs of the vertex shader against the vertex stream of the program
            const VertexStreamDesc* stream = FindVertexStream(program.vertexStream);
            if (stream == nullptr)
            {
                ++failures;
                text += "NO INPUT LAYOUT: unknown vertex stream '" + program.vertexStream + "'\n\n";
                return;
            }
            const InputLayoutBuild inputLayout = BuildInputLayout(result.program->layout, *stream, title);
            for (const ShaderDiagnostic& diagnostic : inputLayout.diagnostics)
            {
                text += AnsiToUtf8(FormatShaderDiagnostic(diagnostic)) + "\n";
            }
            if (!inputLayout.ok)
            {
                ++failures;
                text += "NO INPUT LAYOUT\n\n";
                return;
            }
            text += FormatInputLayout(inputLayout) + "\n";
        }
    }

    int DumpShaderLayouts(const std::filesystem::path& shadersDirectory, std::string& outText)
    {
        outText.clear();
        std::error_code ec;
        std::vector<std::filesystem::path> descriptions;
        std::vector<std::filesystem::path> sources;
        for (const auto& entry : std::filesystem::directory_iterator(shadersDirectory, ec))
        {
            const std::string name = entry.path().filename().string();
            if (name.size() > 12 && name.compare(name.size() - 12, 12, ".shader.json") == 0)
            {
                descriptions.push_back(entry.path());
            }
            else if (entry.path().extension() == ".hlsl")
            {
                sources.push_back(entry.path());
            }
        }
        std::sort(descriptions.begin(), descriptions.end());
        std::sort(sources.begin(), sources.end());

        ShaderReflector reflector;
        int failures = 0;
        std::set<std::string> named;
        ShapeUsers shapes;

        for (const std::filesystem::path& description : descriptions)
        {
            ShaderProgramData program;
            std::string error;
            if (!LoadShaderDescriptor(description, program, error))
            {
                outText += "=== " + description.filename().u8string() + " ===\n" + description.u8string() + ": error: " + error + "\nNOT COMPILED\n\n";
                ++failures;
                continue;
            }
            named.insert(program.sourcePath.lexically_normal().generic_string());
            DumpProgram(reflector, description.filename().u8string(), program, outText, failures, shapes);
        }

        for (const std::filesystem::path& source : sources)
        {
            if (named.count(source.lexically_normal().generic_string()) != 0)
            {
                continue;
            }
            ShaderProgramData program;
            program.sourcePath = source;
            DumpProgram(reflector, source.filename().u8string(), program, outText, failures, shapes);
        }

        // The renderer makes one root signature per shape: programs of the same structure share it
        outText += "=== Root signature shapes ===\n";
        for (const auto& shape : shapes)
        {
            char hash[32];
            std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(shape.first));
            outText += std::string("  ") + hash + "  shared by " + std::to_string(shape.second.size()) + ": ";
            for (std::size_t i = 0; i < shape.second.size(); ++i)
            {
                outText += (i == 0 ? "" : ", ") + shape.second[i];
            }
            outText += "\n";
        }
        return failures;
    }
}
