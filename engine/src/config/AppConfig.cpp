// AppConfig.cpp

#include <nlohmann/json.hpp>

#include <myengine/config/AppConfig.h>
#include <myengine/core/Logger.h>

namespace myengine::config
{
    namespace
    {
        void LogIfAvailable(core::Logger* logger, const std::string& message, const bool error = false)
        {
            if (logger == nullptr)
            {
                return;
            }

            if (error)
            {
                logger->Error(message);
            }
            else
            {
                logger->Warning(message);
            }
        }

        core::Color ParseColor(const nlohmann::json& value, const core::Color& fallback)
        {
            if (!value.is_array() || value.size() != 4)
            {
                return fallback;
            }

            core::Color color = fallback;
            color.r = value[0].get<float>();
            color.g = value[1].get<float>();
            color.b = value[2].get<float>();
            color.a = value[3].get<float>();
            return color;
        }
    }

    AppConfig AppConfig::Default()
    {
        AppConfig config;

        config.windows =
        {
            WindowConfig{"myengine - main", 1280, 720, core::Color{0.10f, 0.12f, 0.20f, 1.0f}},
        };

        config.input = InputConfig{0.8f, 0.8f, 120.0f};
        return config;
    }

    AppConfig AppConfig::LoadFromFile(const std::filesystem::path& path, core::Logger* logger)
    {
        AppConfig config = Default();

        std::ifstream file(path);
        if (!file.is_open())
        {
            LogIfAvailable(logger, "Config file is missing, using defaults: " + path.string());
            return config;
        }

        try
        {
            nlohmann::json root;
            file >> root;

            if (root.contains("windows") && root["windows"].is_array())
            {
                config.windows.clear();
                for (const auto& windowValue : root["windows"])
                {
                    WindowConfig window;

                    if (windowValue.contains("title"))
                    {
                        window.title = windowValue["title"].get<std::string>();
                    }
                    if (windowValue.contains("width"))
                    {
                        window.width = windowValue["width"].get<std::uint32_t>();
                    }
                    if (windowValue.contains("height"))
                    {
                        window.height = windowValue["height"].get<std::uint32_t>();
                    }
                    if (windowValue.contains("clearColor"))
                    {
                        window.clearColor = ParseColor(windowValue["clearColor"], window.clearColor);
                    }

                    config.windows.push_back(window);
                }
            }

            if (config.windows.empty())
            {
                config.windows = Default().windows;
            }

            if (root.contains("input") && root["input"].is_object())
            {
                const auto& input = root["input"];

                if (input.contains("moveSpeed"))
                {
                    config.input.moveSpeed = input["moveSpeed"].get<float>();
                }
                if (input.contains("scaleSpeed"))
                {
                    config.input.scaleSpeed = input["scaleSpeed"].get<float>();
                }
                if (input.contains("rotateSpeedDeg"))
                {
                    config.input.rotateSpeedDeg = input["rotateSpeedDeg"].get<float>();
                }
            }

        }
        catch (const std::exception& ex)
        {
            LogIfAvailable(logger, std::string("Failed to parse config, using defaults: ") + ex.what(), true);
            return Default();
        }

        return config;
    }
}