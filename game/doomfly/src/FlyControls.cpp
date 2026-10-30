// FlyControls.cpp

#include <doomfly/FlyControls.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace doomfly
{
    namespace
    {
        bool ReadSide(const nlohmann::json& value, flybrain::Side& side, std::string& error)
        {
            const std::string text = value.is_string() ? value.get<std::string>() : "any";
            if (text == "left")
            {
                side = flybrain::Side::Left;
            }
            else if (text == "right")
            {
                side = flybrain::Side::Right;
            }
            else if (text == "any")
            {
                side = flybrain::Side::Any;
            }
            else
            {
                error = "unknown side \"" + text + "\" (left, right, any)";
                return false;
            }
            return true;
        }

        bool ReadTerms(const nlohmann::json& array, std::vector<ControlTerm>& terms, std::string& error)
        {
            if (!array.is_array() || array.empty())
            {
                error = "\"terms\" must be a non-empty array";
                return false;
            }
            for (const auto& item : array)
            {
                ControlTerm term;
                if (!item.contains("type") || !item["type"].is_string())
                {
                    error = "a term needs a \"type\"";
                    return false;
                }
                term.type = item["type"].get<std::string>();
                if (item.contains("side") && !ReadSide(item["side"], term.side, error))
                {
                    return false;
                }
                term.weight = item.value("weight", 1.0f);
                terms.push_back(std::move(term));
            }
            return true;
        }

        bool ReadAnalog(const nlohmann::json& root, const char* key, AnalogChannel& channel, std::string& error)
        {
            if (!root.contains(key) || !root[key].is_object())
            {
                error = std::string("no \"") + key + "\" channel";
                return false;
            }
            const auto& node = root[key];
            std::string termsError;
            if (!node.contains("terms") || !ReadTerms(node["terms"], channel.terms, termsError))
            {
                error = std::string(key) + ": " + (termsError.empty() ? std::string("no terms") : termsError);
                return false;
            }
            channel.offsetHz = node.value("offsetHz", 0.0f);
            channel.deadZoneHz = node.value("deadZoneHz", 0.0f);
            channel.gain = node.value("gain", 0.0f);
            channel.limit = node.value("limit", 0.0f);
            return true;
        }
    }

    bool FlyControls::Parse(const std::string& text, FlyControls& out, std::string& error)
    {
        nlohmann::json root;
        try
        {
            root = nlohmann::json::parse(text);
        }
        catch (const std::exception& exception)
        {
            error = std::string("not a JSON file: ") + exception.what();
            return false;
        }
        FlyControls controls;
        controls.name = root.value("name", std::string("controls"));
        controls.description = root.value("description", std::string());
        controls.smoothingMs = root.value("smoothingMs", 100.0f);
        if (!(controls.smoothingMs >= 10.0f && controls.smoothingMs <= 2000.0f))
        {
            error = "smoothingMs must be 10..2000";
            return false;
        }
        if (!ReadAnalog(root, "turn", controls.turn, error) || !ReadAnalog(root, "forward", controls.forward, error))
        {
            return false;
        }
        std::string fireError;
        if (!root.contains("fire") || !root["fire"].is_object() || !root["fire"].contains("terms") || !ReadTerms(root["fire"]["terms"], controls.fire.terms, fireError))
        {
            error = "fire: " + (fireError.empty() ? std::string("no terms") : fireError);
            return false;
        }
        controls.fire.windowMs = root["fire"].value("windowMs", 100.0f);
        controls.fire.minSpikes = root["fire"].value("minSpikes", 1u);
        controls.fire.thresholdHz = root["fire"].value("thresholdHz", 0.0f);
        if (root.contains("use") && root["use"].is_object())
        {
            controls.use.everyTics = root["use"].value("everyTics", 35u);
            controls.use.stuckTics = root["use"].value("stuckTics", 12u);
            controls.use.stuckUnits = root["use"].value("stuckUnits", 6.0f);
        }
        if (root.contains("unstick") && root["unstick"].is_object())
        {
            controls.unstick.afterTics = root["unstick"].value("afterTics", 0u);
            controls.unstick.turnDegrees = root["unstick"].value("turnDegrees", 5.0f);
            controls.unstick.forTics = root["unstick"].value("forTics", 18u);
            controls.unstick.backUnits = root["unstick"].value("backUnits", 10.0f);
        }
        out = std::move(controls);
        return true;
    }

    bool FlyControls::Load(const std::filesystem::path& path, FlyControls& out, std::string& error)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            error = "cannot read " + path.string();
            return false;
        }
        std::stringstream text;
        text << file.rdbuf();
        if (!Parse(text.str(), out, error))
        {
            error = path.string() + ": " + error;
            return false;
        }
        return true;
    }
}
