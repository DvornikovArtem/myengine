// ParticleEffect.cpp

#include <myengine/fx/ParticleEffect.h>

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace myengine::fx
{
    namespace
    {
        using json = nlohmann::json;

        struct ParseError
        {
            std::string message;
        };

        float Number(const json& value, const std::string& where)
        {
            if (!value.is_number())
            {
                throw ParseError{where + " must be a number"};
            }
            return value.get<float>();
        }

        // A number, or [min, max]
        void Range(const json& object, const char* key, float& low, float& high, const std::string& emitter)
        {
            const auto it = object.find(key);
            if (it == object.end())
            {
                return;
            }
            const std::string where = emitter + "." + key;
            if (it->is_number())
            {
                low = high = it->get<float>();
            }
            else if (it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number())
            {
                low = (*it)[0].get<float>();
                high = (*it)[1].get<float>();
                if (high < low)
                {
                    std::swap(low, high);
                }
            }
            else
            {
                throw ParseError{where + " must be a number or [min, max]"};
            }
        }

        void Scalar(const json& object, const char* key, float& out, const std::string& emitter)
        {
            if (const auto it = object.find(key); it != object.end())
            {
                out = Number(*it, emitter + "." + key);
            }
        }

        std::vector<CurveKey> Curve(const json& value, const std::string& where)
        {
            std::vector<CurveKey> keys;
            if (value.is_number())
            {
                keys.push_back({0.0f, value.get<float>()});
                return keys;
            }
            if (!value.is_array())
            {
                throw ParseError{where + " must be a number or a list of [t, value]"};
            }
            for (const json& key : value)
            {
                if (!key.is_array() || key.size() != 2 || !key[0].is_number() || !key[1].is_number())
                {
                    throw ParseError{where + ": every key is [t, value]"};
                }
                keys.push_back({std::clamp(key[0].get<float>(), 0.0f, 1.0f), key[1].get<float>()});
            }
            std::stable_sort(keys.begin(), keys.end(), [](const CurveKey& a, const CurveKey& b) { return a.t < b.t; });
            return keys;
        }

        std::vector<ColorKey> Gradient(const json& value, const std::string& where)
        {
            std::vector<ColorKey> keys;
            if (!value.is_array())
            {
                throw ParseError{where + " must be a list of [t, [r, g, b, a]]"};
            }
            for (const json& key : value)
            {
                if (!key.is_array() || key.size() != 2 || !key[0].is_number() || !key[1].is_array() || key[1].size() < 3 || key[1].size() > 4)
                {
                    throw ParseError{where + ": every key is [t, [r, g, b, a]]"};
                }
                ColorKey color;
                color.t = std::clamp(key[0].get<float>(), 0.0f, 1.0f);
                for (std::size_t i = 0; i < key[1].size(); ++i)
                {
                    color.rgba[i] = Number(key[1][i], where + " colour");
                }
                keys.push_back(color);
            }
            std::stable_sort(keys.begin(), keys.end(), [](const ColorKey& a, const ColorKey& b) { return a.t < b.t; });
            return keys;
        }

        EmitterDesc ParseEmitter(const json& object, const std::string& effect, const std::size_t index)
        {
            EmitterDesc emitter;
            emitter.name = object.value("name", "emitter" + std::to_string(index));
            const std::string where = effect + "/" + emitter.name;

            const std::string blend = object.value("blend", std::string("additive"));
            if (blend == "additive")
            {
                emitter.blend = render::BlendMode::Additive;
            }
            else if (blend == "alpha")
            {
                emitter.blend = render::BlendMode::Alpha;
            }
            else
            {
                throw ParseError{where + ": unknown blend '" + blend + "' (additive, alpha)"};
            }
            emitter.frame = std::clamp(object.value("frame", 0), 0, 15);
            emitter.maxParticles = std::clamp<std::uint32_t>(object.value("maxParticles", 200u), 1u, 5000u);

            Scalar(object, "rate", emitter.rate, where);
            emitter.loop = object.value("loop", false);
            Scalar(object, "duration", emitter.duration, where);
            Scalar(object, "startDelay", emitter.startDelay, where);
            if (const auto it = object.find("bursts"); it != object.end())
            {
                if (!it->is_array())
                {
                    throw ParseError{where + ".bursts must be a list of [time, count]"};
                }
                for (const json& burst : *it)
                {
                    if (!burst.is_array() || burst.size() != 2 || !burst[0].is_number() || !burst[1].is_number())
                    {
                        throw ParseError{where + ".bursts: every burst is [time, count]"};
                    }
                    emitter.bursts.push_back({std::max(burst[0].get<float>(), 0.0f), static_cast<std::uint32_t>(std::max(burst[1].get<float>(), 0.0f))});
                }
                std::stable_sort(emitter.bursts.begin(), emitter.bursts.end(), [](const Burst& a, const Burst& b) { return a.time < b.time; });
            }
            if (emitter.rate <= 0.0f && emitter.bursts.empty())
            {
                throw ParseError{where + ": no emission (rate or bursts)"};
            }

            const std::string shape = object.value("shape", std::string("point"));
            if (shape == "point") emitter.shape = EmitterShape::Point;
            else if (shape == "sphere") emitter.shape = EmitterShape::Sphere;
            else if (shape == "disc") emitter.shape = EmitterShape::Disc;
            else if (shape == "cone") emitter.shape = EmitterShape::Cone;
            else throw ParseError{where + ": unknown shape '" + shape + "' (point, sphere, disc, cone)"};
            Scalar(object, "shapeRadius", emitter.shapeRadius, where);
            Scalar(object, "coneAngle", emitter.coneAngleDeg, where);
            Range(object, "lifetime", emitter.lifetimeMin, emitter.lifetimeMax, where);
            if (emitter.lifetimeMin <= 0.0f)
            {
                throw ParseError{where + ".lifetime must be above 0"};
            }
            Range(object, "speed", emitter.speedMin, emitter.speedMax, where);
            Scalar(object, "inheritVelocity", emitter.inheritVelocity, where);
            Range(object, "size", emitter.sizeMin, emitter.sizeMax, where);
            Range(object, "rotation", emitter.rotationMinDeg, emitter.rotationMaxDeg, where);
            Range(object, "rotationSpeed", emitter.rotationSpeedMinDeg, emitter.rotationSpeedMaxDeg, where);
            if (const auto it = object.find("gravity"); it != object.end())
            {
                if (!it->is_array() || it->size() != 3)
                {
                    throw ParseError{where + ".gravity must be [x, y, z]"};
                }
                for (int i = 0; i < 3; ++i)
                {
                    emitter.gravity[i] = Number((*it)[static_cast<std::size_t>(i)], where + ".gravity");
                }
            }
            Scalar(object, "drag", emitter.drag, where);
            Scalar(object, "floorY", emitter.floorY, where);
            Scalar(object, "bounce", emitter.bounce, where);
            if (const auto it = object.find("sizeOverLife"); it != object.end())
            {
                emitter.sizeOverLife = Curve(*it, where + ".sizeOverLife");
            }
            if (const auto it = object.find("color"); it != object.end())
            {
                emitter.colorOverLife = Gradient(*it, where + ".color");
            }
            Scalar(object, "intensity", emitter.intensity, where);
            Scalar(object, "stretch", emitter.stretch, where);
            Scalar(object, "stretchSpeed", emitter.stretchSpeed, where);
            emitter.useTint = object.value("useTint", true);
            emitter.sortBackToFront = object.value("sort", true);
            return emitter;
        }
    }

    bool ParseEffectText(const std::string& jsonText, EffectDesc& out, std::string& error)
    {
        out = {};
        try
        {
            const json root = json::parse(jsonText);
            if (!root.is_object())
            {
                error = "the effect must be a JSON object";
                return false;
            }
            out.name = root.value("name", std::string());
            if (out.name.empty())
            {
                error = "the effect has no \"name\"";
                return false;
            }
            const auto emitters = root.find("emitters");
            if (emitters == root.end() || !emitters->is_array() || emitters->empty())
            {
                error = out.name + ": \"emitters\" must be a non-empty list";
                return false;
            }
            for (std::size_t i = 0; i < emitters->size(); ++i)
            {
                if (!(*emitters)[i].is_object())
                {
                    error = out.name + ": every emitter must be an object";
                    return false;
                }
                out.emitters.push_back(ParseEmitter((*emitters)[i], out.name, i));
            }
        }
        catch (const ParseError& parse)
        {
            error = parse.message;
            return false;
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            return false;
        }
        return true;
    }

    float EvaluateCurve(const std::vector<CurveKey>& keys, const float t)
    {
        if (keys.empty())
        {
            return 1.0f;
        }
        if (t <= keys.front().t)
        {
            return keys.front().value;
        }
        for (std::size_t i = 1; i < keys.size(); ++i)
        {
            if (t <= keys[i].t)
            {
                const float span = keys[i].t - keys[i - 1].t;
                const float f = span > 1e-6f ? (t - keys[i - 1].t) / span : 1.0f;
                return keys[i - 1].value + (keys[i].value - keys[i - 1].value) * f;
            }
        }
        return keys.back().value;
    }

    void EvaluateGradient(const std::vector<ColorKey>& keys, const float t, float out[4])
    {
        if (keys.empty())
        {
            out[0] = out[1] = out[2] = out[3] = 1.0f;
            return;
        }
        if (t <= keys.front().t)
        {
            std::copy(keys.front().rgba, keys.front().rgba + 4, out);
            return;
        }
        for (std::size_t i = 1; i < keys.size(); ++i)
        {
            if (t <= keys[i].t)
            {
                const float span = keys[i].t - keys[i - 1].t;
                const float f = span > 1e-6f ? (t - keys[i - 1].t) / span : 1.0f;
                for (int c = 0; c < 4; ++c)
                {
                    out[c] = keys[i - 1].rgba[c] + (keys[i].rgba[c] - keys[i - 1].rgba[c]) * f;
                }
                return;
            }
        }
        std::copy(keys.back().rgba, keys.back().rgba + 4, out);
    }
}
