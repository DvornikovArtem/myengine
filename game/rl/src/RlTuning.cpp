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
            v.Float("hitForwardScale", t.ball.hitForwardScale);
            v.Float("hitForceScale", t.ball.hitForceScale);
            v.Floats("hitSpeed", t.ball.hitSpeed, 4);
            v.Floats("hitScale", t.ball.hitScale, 4);
            v.Float("hitMaxRelativeSpeed", t.ball.hitMaxRelativeSpeed);
            v.End();

            v.Begin("car");
            v.Section("Body");
            v.Float("mass", t.car.mass);
            v.Float("halfExtentX", t.car.halfExtentX);
            v.Float("halfExtentY", t.car.halfExtentY);
            v.Float("halfExtentZ", t.car.halfExtentZ);
            v.Float("comOffsetY", t.car.comOffsetY);
            v.Float("comOffsetZ", t.car.comOffsetZ);
            v.Float("inertiaMultiplier", t.car.inertiaMultiplier);
            v.Float("friction", t.car.friction);
            v.Float("restitution", t.car.restitution);
            v.Float("carBallFriction", t.car.carBallFriction);
            v.Float("carBallRestitution", t.car.carBallRestitution);
            v.Float("carCarFriction", t.car.carCarFriction);
            v.Float("carCarRestitution", t.car.carCarRestitution);
            v.Float("maxSpeed", t.car.maxSpeed);
            v.Float("maxAngularSpeed", t.car.maxAngularSpeed);
            v.Float("supersonicSpeed", t.car.supersonicSpeed);
            v.Float("supersonicMaintainSpeed", t.car.supersonicMaintainSpeed);
            v.Float("supersonicMaintainTime", t.car.supersonicMaintainTime);
            v.Section("Suspension");
            v.Float("wheelMountXFront", t.car.wheelMountXFront);
            v.Float("wheelMountXRear", t.car.wheelMountXRear);
            v.Float("wheelMountYOffset", t.car.wheelMountYOffset);
            v.Float("wheelFrontZ", t.car.wheelFrontZ);
            v.Float("wheelRearZ", t.car.wheelRearZ);
            v.Float("wheelRadiusFront", t.car.wheelRadiusFront);
            v.Float("wheelRadiusRear", t.car.wheelRadiusRear);
            v.Float("suspensionRestFront", t.car.suspensionRestFront);
            v.Float("suspensionRestRear", t.car.suspensionRestRear);
            v.Float("suspensionTravel", t.car.suspensionTravel);
            v.Float("suspensionSubtraction", t.car.suspensionSubtraction);
            v.Float("suspensionStiffness", t.car.suspensionStiffness);
            v.Float("suspensionDampCompression", t.car.suspensionDampCompression);
            v.Float("suspensionDampRelaxation", t.car.suspensionDampRelaxation);
            v.Float("suspensionScaleFront", t.car.suspensionScaleFront);
            v.Float("suspensionScaleRear", t.car.suspensionScaleRear);
            v.Float("pushbackErp", t.car.pushbackErp);
            v.Float("spawnHeight", t.car.spawnHeight);
            v.Section("Throttle and brake");
            v.Float("throttleAccel", t.car.throttleAccel);
            v.Floats("driveSpeed", t.car.driveSpeed, 3);
            v.Floats("driveFactor", t.car.driveFactor, 3);
            v.Float("throttleDeadzone", t.car.throttleDeadzone);
            v.Float("brakeAccel", t.car.brakeAccel);
            v.Float("coastBrakeFactor", t.car.coastBrakeFactor);
            v.Float("stopSpeed", t.car.stopSpeed);
            v.Float("brakeStiffness", t.car.brakeStiffness);
            v.Section("Steering and tyres");
            v.Floats("steerSpeed", t.car.steerSpeed, 6);
            v.Floats("steerAngle", t.car.steerAngle, 6);
            v.Floats("powerslideSteerSpeed", t.car.powerslideSteerSpeed, 2);
            v.Floats("powerslideSteerAngle", t.car.powerslideSteerAngle, 2);
            v.Float("slipMinSpeed", t.car.slipMinSpeed);
            v.Floats("latFrictionSlip", t.car.latFrictionSlip, 2);
            v.Floats("latFriction", t.car.latFriction, 2);
            v.Float("handbrakeLatFriction", t.car.handbrakeLatFriction);
            v.Floats("handbrakeLongSlip", t.car.handbrakeLongSlip, 2);
            v.Floats("handbrakeLongFriction", t.car.handbrakeLongFriction, 2);
            v.Floats("nonStickyNormal", t.car.nonStickyNormal, 3);
            v.Floats("nonStickyScale", t.car.nonStickyScale, 3);
            v.Float("handbrakeRise", t.car.handbrakeRise);
            v.Float("handbrakeFall", t.car.handbrakeFall);
            v.Float("frictionDamping", t.car.frictionDamping);
            v.Section("Pull into the surface");
            v.Float("stickBase", t.car.stickBase);
            v.Float("stickSteep", t.car.stickSteep);
            v.Float("stickStopSpeed", t.car.stickStopSpeed);
            v.Section("Jump and dodge");
            v.Float("jumpImpulse", t.car.jumpImpulse);
            v.Float("jumpHoldAccel", t.car.jumpHoldAccel);
            v.Float("jumpMinTime", t.car.jumpMinTime);
            v.Float("jumpMinAccelScale", t.car.jumpMinAccelScale);
            v.Float("jumpHoldTime", t.car.jumpHoldTime);
            v.Float("doubleJumpImpulse", t.car.doubleJumpImpulse);
            v.Float("dodgeWindow", t.car.dodgeWindow);
            v.Float("dodgeDeadzone", t.car.dodgeDeadzone);
            v.Float("dodgeImpulse", t.car.dodgeImpulse);
            v.Float("dodgeForwardSpeedScale", t.car.dodgeForwardSpeedScale);
            v.Float("dodgeSideSpeedScale", t.car.dodgeSideSpeedScale);
            v.Float("dodgeBackSpeedScale", t.car.dodgeBackSpeedScale);
            v.Float("dodgeBackImpulseScale", t.car.dodgeBackImpulseScale);
            v.Float("dodgeZDamp", t.car.dodgeZDamp);
            v.Float("dodgeZDampStart", t.car.dodgeZDampStart);
            v.Float("dodgeZDampEnd", t.car.dodgeZDampEnd);
            v.Float("dodgeTorqueTime", t.car.dodgeTorqueTime);
            v.Float("dodgeTorquePitch", t.car.dodgeTorquePitch);
            v.Float("dodgeTorqueRoll", t.car.dodgeTorqueRoll);
            v.Float("dodgePitchLockExtra", t.car.dodgePitchLockExtra);
            v.Section("Air control");
            v.Float("airPitchTorque", t.car.airPitchTorque);
            v.Float("airYawTorque", t.car.airYawTorque);
            v.Float("airRollTorque", t.car.airRollTorque);
            v.Float("airPitchDamping", t.car.airPitchDamping);
            v.Float("airYawDamping", t.car.airYawDamping);
            v.Float("airRollDamping", t.car.airRollDamping);
            v.Float("airThrottleAccel", t.car.airThrottleAccel);
            v.Section("Auto-roll and auto-flip");
            v.Float("autoRollForce", t.car.autoRollForce);
            v.Float("autoRollTorque", t.car.autoRollTorque);
            v.Float("autoFlipImpulse", t.car.autoFlipImpulse);
            v.Float("autoFlipTorque", t.car.autoFlipTorque);
            v.Float("autoFlipTime", t.car.autoFlipTime);
            v.Float("autoFlipNormalY", t.car.autoFlipNormalY);
            v.Float("autoFlipRollThreshold", t.car.autoFlipRollThreshold);
            v.Section("Boost");
            v.Float("boostStart", t.car.boostStart);
            v.Float("boostMax", t.car.boostMax);
            v.Float("boostUsePerSecond", t.car.boostUsePerSecond);
            v.Float("boostMinTime", t.car.boostMinTime);
            v.Float("boostAccelGround", t.car.boostAccelGround);
            v.Float("boostAccelAir", t.car.boostAccelAir);
            v.Section("Bump and demolish");
            v.Float("bumpMinForward", t.car.bumpMinForward);
            v.Float("bumpCooldown", t.car.bumpCooldown);
            v.Floats("bumpSpeed", t.car.bumpSpeed, 3);
            v.Floats("bumpGroundVelocity", t.car.bumpGroundVelocity, 3);
            v.Floats("bumpAirVelocity", t.car.bumpAirVelocity, 3);
            v.Floats("bumpUpVelocity", t.car.bumpUpVelocity, 3);
            v.Float("respawnDelay", t.car.respawnDelay);
            v.End();

            v.Begin("camera");
            v.Float("fov", t.camera.fov);
            v.Float("distance", t.camera.distance);
            v.Float("height", t.camera.height);
            v.Float("pitchDegrees", t.camera.pitchDegrees);
            v.Float("stiffness", t.camera.stiffness);
            v.Float("swivelSpeed", t.camera.swivelSpeed);
            v.Float("transitionSpeed", t.camera.transitionSpeed);
            v.Float("upFollow", t.camera.upFollow);
            v.Float("airUpFollow", t.camera.airUpFollow);
            v.Float("airSwivelScale", t.camera.airSwivelScale);
            v.Float("minDistance", t.camera.minDistance);
            v.Float("wallMargin", t.camera.wallMargin);
            v.Float("ballCamMaxPitch", t.camera.ballCamMaxPitch);
            v.Float("rollFollow", t.camera.rollFollow);
            v.Float("airVelocityFollow", t.camera.airVelocityFollow);
            v.Float("maxTurnRate", t.camera.maxTurnRate);
            v.End();

            v.Begin("match");
            v.Float("matchSeconds", t.match.matchSeconds);
            v.Float("countdownSeconds", t.match.countdownSeconds);
            v.Float("goalSeconds", t.match.goalSeconds);
            v.Float("goalLineMargin", t.match.goalLineMargin);
            v.Float("padBigBoost", t.match.padBigBoost);
            v.Float("padBigRespawn", t.match.padBigRespawn);
            v.Float("padBigRadius", t.match.padBigRadius);
            v.Float("padSmallBoost", t.match.padSmallBoost);
            v.Float("padSmallRespawn", t.match.padSmallRespawn);
            v.Float("padSmallRadius", t.match.padSmallRadius);
            v.Float("padMaxHeight", t.match.padMaxHeight);
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
            void Section(const char*) const {}
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
            void Section(const char*) {}
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

    namespace
    {
        // adapts the abstract visitor of the header to the compile-time visitor of Reflect
        struct VisitorAdapter
        {
            RlTuningVisitor& target;
            void Begin(const char* name) { target.Begin(name); }
            void End() { target.End(); }
            void Section(const char* title) { target.Section(title); }
            void Float(const char* name, float& value) { target.Float(name, value); }
            void Int(const char* name, int& value) { target.Int(name, value); }
            void Floats(const char* name, float* values, int count) { target.Floats(name, values, count); }
        };
    }

    void RlTuning::Visit(RlTuningVisitor& visitor)
    {
        VisitorAdapter adapter{visitor};
        Reflect(*this, adapter);
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
