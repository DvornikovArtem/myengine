// ShaderLayoutDump.cpp

#include <myengine/render/dx12/ShaderLayoutDump.h>

#include <algorithm>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <myengine/render/RenderTypes.h>
#include <myengine/render/ShaderLayout.h>

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

        void DumpProgram(ShaderReflector& reflector, const std::string& title, const ShaderProgramData& program, std::string& text, int& failures)
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
            text += FormatShaderLayout(result.program->layout) + "\n";
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

        for (const std::filesystem::path& description : descriptions)
        {
            ShaderProgramData program;
            try
            {
                std::ifstream stream(description);
                const nlohmann::json json = nlohmann::json::parse(stream);
                program.sourcePath = description.parent_path() / std::filesystem::u8path(json.at("source").get<std::string>());
                program.vertexEntry = json.value("vertexEntry", program.vertexEntry);
                program.pixelEntry = json.value("pixelEntry", program.pixelEntry);
                program.vertexProfile = json.value("vertexProfile", program.vertexProfile);
                program.pixelProfile = json.value("pixelProfile", program.pixelProfile);
            }
            catch (const std::exception& ex)
            {
                outText += "=== " + description.filename().u8string() + " ===\n" + description.u8string() + ": error: " + ex.what() + "\nNOT COMPILED\n\n";
                ++failures;
                continue;
            }
            named.insert(program.sourcePath.lexically_normal().generic_string());
            DumpProgram(reflector, description.filename().u8string(), program, outText, failures);
        }

        for (const std::filesystem::path& source : sources)
        {
            if (named.count(source.lexically_normal().generic_string()) != 0)
            {
                continue;
            }
            ShaderProgramData program;
            program.sourcePath = source;
            DumpProgram(reflector, source.filename().u8string(), program, outText, failures);
        }
        return failures;
    }
}
