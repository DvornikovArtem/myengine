// ShaderDescriptor.cpp

#include <myengine/render/ShaderDescriptor.h>

#include <fstream>

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
}
