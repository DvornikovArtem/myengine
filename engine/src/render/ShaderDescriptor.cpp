// ShaderDescriptor.cpp

#include <myengine/render/ShaderDescriptor.h>

#include <fstream>
#include <iterator>
#include <regex>
#include <set>

#include <nlohmann/json.hpp>

namespace myengine::render
{
    bool LoadShaderDescriptor(const std::filesystem::path& descriptorPath, ShaderProgramData& outProgram, std::string& outError)
    {
        std::ifstream stream(descriptorPath);
        if (!stream.is_open())
        {
            outError = "shader descriptor could not be opened";
            return false;
        }

        try
        {
            nlohmann::json descriptor;
            stream >> descriptor;

            const std::string source = descriptor.value("source", std::string());
            if (source.empty())
            {
                outError = "shader descriptor has an empty \"source\"";
                return false;
            }

            ShaderProgramData program = outProgram;
            program.sourcePath = descriptorPath.parent_path() / std::filesystem::u8path(source);
            program.vertexEntry = descriptor.value("vertexEntry", program.vertexEntry);
            program.pixelEntry = descriptor.value("pixelEntry", program.pixelEntry);
            program.vertexProfile = descriptor.value("vertexProfile", program.vertexProfile);
            program.pixelProfile = descriptor.value("pixelProfile", program.pixelProfile);

            program.vertexStream = descriptor.value("vertexStream", program.vertexStream);

            program.samplers.clear();
            const auto samplers = descriptor.find("samplers");
            if (samplers != descriptor.end())
            {
                if (!samplers->is_object())
                {
                    outError = "\"samplers\" must be an object: { \"gSampler\": \"linear_wrap\" }";
                    return false;
                }
                for (const auto& entry : samplers->items())
                {
                    if (!entry.value().is_string())
                    {
                        outError = "\"samplers\": the state of '" + entry.key() + "' must be a string";
                        return false;
                    }
                    program.samplers.emplace_back(entry.key(), entry.value().get<std::string>());
                }
            }
            outProgram = std::move(program);
            return true;
        }
        catch (const std::exception& ex)
        {
            outError = std::string("shader descriptor parse failed: ") + ex.what();
            return false;
        }
    }

    namespace
    {
        constexpr int kMaxIncludeDepth = 8;

        void CollectIncludes(const std::filesystem::path& file, const std::string& text, std::vector<std::filesystem::path>& result, std::set<std::string>& visited, const int depth)
        {
            if (depth > kMaxIncludeDepth)
            {
                return;
            }
            static const std::regex includePattern(R"re(#\s*include\s*"([^"]+)")re");
            for (std::sregex_iterator it(text.begin(), text.end(), includePattern), end; it != end; ++it)
            {
                const std::filesystem::path included = (file.parent_path() / std::filesystem::u8path((*it)[1].str())).lexically_normal();
                if (!visited.insert(included.generic_string()).second)
                {
                    continue;
                }
                result.push_back(included);

                std::ifstream stream(included, std::ios::binary);
                if (!stream.is_open())
                {
                    continue; // the compiler reports a missing include
                }
                const std::string includedText((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                CollectIncludes(included, includedText, result, visited, depth + 1);
            }
        }
    }

    std::vector<std::filesystem::path> ScanShaderIncludes(const std::filesystem::path& sourceFile, const std::string& text)
    {
        std::vector<std::filesystem::path> result;
        std::set<std::string> visited;
        CollectIncludes(sourceFile, text, result, visited, 0);
        return result;
    }

    std::vector<std::filesystem::path> ScanShaderIncludes(const std::filesystem::path& sourceFile)
    {
        std::ifstream stream(sourceFile, std::ios::binary);
        if (!stream.is_open())
        {
            return {};
        }
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return ScanShaderIncludes(sourceFile, text);
    }
}
