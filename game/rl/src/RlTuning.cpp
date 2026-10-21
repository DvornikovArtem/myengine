// RlTuning.cpp

#include <rl/RlTuning.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace rl
{
    namespace
    {
        using json = nlohmann::json;

        // One list of the fields serves both the reader and the writer.
        template <typename Visitor>
        void Reflect(RlTuning& t, Visitor& v)
        {
            v.Float("gravity", t.gravity);

            v.Begin("arena");
            v.Float("halfWidth", t.arena.halfWidth);
            v.Float("halfLength", t.arena.halfLength);
            v.Float("height", t.arena.height);
            v.Float("cornerCut", t.arena.cornerCut);
            v.Float("floorRampRadius", t.arena.floorRampRadius);
            v.Float("ceilingRampRadius", t.arena.ceilingRampRadius);
            v.Int("rampSegments", t.arena.rampSegments);
            v.Float("goalHalfWidth", t.arena.goalHalfWidth);
            v.Float("goalHeight", t.arena.goalHeight);
            v.Float("goalDepth", t.arena.goalDepth);
            v.Float("friction", t.arena.friction);
            v.Float("restitution", t.arena.restitution);
            v.End();

            v.Begin("ball");
            v.Float("radius", t.ball.radius);
            v.Float("mass", t.ball.mass);
            v.Float("restitution", t.ball.restitution);
            v.Float("friction", t.ball.friction);
            v.Float("maxSpeed", t.ball.maxSpeed);
            v.Float("maxAngularSpeed", t.ball.maxAngularSpeed);
            v.Float("linearDamping", t.ball.linearDamping);
            v.Float("angularDamping", t.ball.angularDamping);
            v.Float("hitNormalYScale", t.ball.hitNormalYScale);
            v.Floats("hitSpeed", t.ball.hitSpeed, 4);
            v.Floats("hitScale", t.ball.hitScale, 4);
            v.Int("hitCooldownTicks", t.ball.hitCooldownTicks);
            v.End();

            v.Begin("car");
            v.Float("mass", t.car.mass);
            v.Float("halfExtentX", t.car.halfExtentX);
            v.Float("halfExtentY", t.car.halfExtentY);
            v.Float("halfExtentZ", t.car.halfExtentZ);
            v.Float("inertiaMultiplier", t.car.inertiaMultiplier);
            v.Float("friction", t.car.friction);
            v.Float("restitution", t.car.restitution);
            v.Float("maxSpeed", t.car.maxSpeed);
            v.Float("maxAngularSpeed", t.car.maxAngularSpeed);
            v.Float("supersonicSpeed", t.car.supersonicSpeed);
            v.Float("throttleMaxSpeed", t.car.throttleMaxSpeed);
            v.Float("wheelMountX", t.car.wheelMountX);
            v.Float("wheelMountYOffset", t.car.wheelMountYOffset);
            v.Float("wheelFrontZ", t.car.wheelFrontZ);
            v.Float("wheelRearZ", t.car.wheelRearZ);
            v.Float("suspensionRest", t.car.suspensionRest);
            v.Float("suspensionContactMargin", t.car.suspensionContactMargin);
            v.Float("springStiffness", t.car.springStiffness);
            v.Float("springDamping", t.car.springDamping);
            v.Float("throttleAccel", t.car.throttleAccel);
            v.Float("throttleAccelAtMax", t.car.throttleAccelAtMax);
            v.Float("brakeAccel", t.car.brakeAccel);
            v.Float("coastDecel", t.car.coastDecel);
            v.Float("lateralAccel", t.car.lateralAccel);
            v.Float("lateralGain", t.car.lateralGain);
            v.Float("handbrakeLateralScale", t.car.handbrakeLateralScale);
            v.Float("handbrakeSteerScale", t.car.handbrakeSteerScale);
            v.Float("maxSteerAngle", t.car.maxSteerAngle);
            v.Floats("curvatureSpeed", t.car.curvatureSpeed, 6);
            v.Floats("curvatureValue", t.car.curvatureValue, 6);
            v.Float("groundAngularDamping", t.car.groundAngularDamping);
            v.Float("stickAccel", t.car.stickAccel);
            v.Float("stickMinContacts", t.car.stickMinContacts);
            v.Float("jumpStickGrace", t.car.jumpStickGrace);
            v.Float("jumpImpulse", t.car.jumpImpulse);
            v.Float("jumpHoldAccel", t.car.jumpHoldAccel);
            v.Float("jumpHoldTime", t.car.jumpHoldTime);
            v.Float("doubleJumpImpulse", t.car.doubleJumpImpulse);
            v.Float("dodgeWindow", t.car.dodgeWindow);
            v.Float("dodgeDeadzone", t.car.dodgeDeadzone);
            v.Float("dodgeImpulse", t.car.dodgeImpulse);
            v.Float("dodgeForwardSpeedBonus", t.car.dodgeForwardSpeedBonus);
            v.Float("dodgeVerticalCancel", t.car.dodgeVerticalCancel);
            v.Float("dodgeSpin", t.car.dodgeSpin);
            v.Float("dodgeSpinTime", t.car.dodgeSpinTime);
            v.Float("dodgeLockTime", t.car.dodgeLockTime);
            v.Float("airPitchTorque", t.car.airPitchTorque);
            v.Float("airYawTorque", t.car.airYawTorque);
            v.Float("airRollTorque", t.car.airRollTorque);
            v.Float("airPitchDamping", t.car.airPitchDamping);
            v.Float("airYawDamping", t.car.airYawDamping);
            v.Float("airRollDamping", t.car.airRollDamping);
            v.Float("boostStart", t.car.boostStart);
            v.Float("boostMax", t.car.boostMax);
            v.Float("boostUsePerSecond", t.car.boostUsePerSecond);
            v.Float("boostAccelGround", t.car.boostAccelGround);
            v.Float("boostAccelAir", t.car.boostAccelAir);
            v.Float("respawnDelay", t.car.respawnDelay);
            v.End();
        }

        struct Reader
        {
            const json* node;
            std::vector<const json*> stack;

            void Begin(const char* name)
            {
                stack.push_back(node);
                const auto it = node->find(name);
                node = (it != node->end() && it->is_object()) ? &*it : nullptr;
                if (node == nullptr)
                {
                    static const json empty = json::object();
                    node = &empty;
                }
            }
            void End()
            {
                node = stack.back();
                stack.pop_back();
            }
            void Float(const char* name, float& value) const
            {
                const auto it = node->find(name);
                if (it != node->end() && it->is_number())
                {
                    value = it->get<float>();
                }
            }
            void Int(const char* name, int& value) const
            {
                const auto it = node->find(name);
                if (it != node->end() && it->is_number())
                {
                    value = it->get<int>();
                }
            }
            void Floats(const char* name, float* values, int count) const
            {
                const auto it = node->find(name);
                if (it == node->end() || !it->is_array() || static_cast<int>(it->size()) != count)
                {
                    return;
                }
                for (int i = 0; i < count; ++i)
                {
                    if ((*it)[static_cast<size_t>(i)].is_number())
                    {
                        values[i] = (*it)[static_cast<size_t>(i)].get<float>();
                    }
                }
            }
        };

        // 0.4f is 0.4000000059604645 as a double: keep the six digits that matter so the file reads like a hand-written one
        double Short(float value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.6g", static_cast<double>(value));
            return std::strtod(text, nullptr);
        }

        struct Writer
        {
            using ordered = nlohmann::ordered_json;
            ordered root = ordered::object();
            std::vector<ordered*> stack;
            ordered* node = &root;

            void Begin(const char* name)
            {
                stack.push_back(node);
                node = &(*node)[name];
                *node = ordered::object();
            }
            void End()
            {
                node = stack.back();
                stack.pop_back();
            }
            void Float(const char* name, float& value) { (*node)[name] = Short(value); }
            void Int(const char* name, int& value) { (*node)[name] = value; }
            void Floats(const char* name, float* values, int count)
            {
                ordered array = ordered::array();
                for (int i = 0; i < count; ++i)
                {
                    array.push_back(Short(values[i]));
                }
                (*node)[name] = array;
            }
        };
    }

    bool RlTuning::LoadFromText(const std::string& text, std::string* error)
    {
        json parsed = json::parse(text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            if (error != nullptr)
            {
                *error = "rl_tuning: the text is not a json object";
            }
            return false;
        }
        Reader reader{&parsed, {}};
        Reflect(*this, reader);
        return true;
    }

    bool RlTuning::LoadFromFile(const std::string& path, std::string* error)
    {
        std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
        if (!file)
        {
            if (error != nullptr)
            {
                *error = "rl_tuning: cannot open " + path;
            }
            return false;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return LoadFromText(buffer.str(), error);
    }

    std::string RlTuning::ToJson() const
    {
        RlTuning copy = *this;
        Writer writer;
        Reflect(copy, writer);
        return writer.root.dump(2);
    }
}
