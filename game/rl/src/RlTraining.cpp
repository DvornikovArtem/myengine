// RlTraining.cpp

#include <rl/RlTraining.h>

#include <rl/RlMatch.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rl
{
    namespace
    {
        using json = nlohmann::json;

        constexpr float kReadySeconds = 1.0f;     // an exercise waits this long (the world frozen) before the clock starts
        constexpr float kBannerSeconds = 1.8f;    // SUCCESS / MISS, then the runner goes on by itself
        constexpr float kSaveSeconds = 1.5f;      // a touched ball that does not go in this long after the touch is a save
        constexpr float kSavedBallSpeed = 0.5f;   // m/s: ... or the one that goes away from the own goal at once

        // ---- the json of a pack

        float Number(const json& object, const char* key, const float fallback)
        {
            const auto it = object.find(key);
            return (it != object.end() && it->is_number()) ? it->get<float>() : fallback;
        }

        std::string Text(const json& object, const char* key, const std::string& fallback = std::string())
        {
            const auto it = object.find(key);
            return (it != object.end() && it->is_string()) ? it->get<std::string>() : fallback;
        }

        bool Vector(const json& object, const char* key, RlVec3& out)
        {
            const auto it = object.find(key);
            if (it == object.end())
            {
                return true;
            }
            if (!it->is_array() || it->size() != 3 || !(*it)[0].is_number() || !(*it)[1].is_number() || !(*it)[2].is_number())
            {
                return false;
            }
            out = {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
            return true;
        }

        bool Fail(std::string* error, const std::string& message)
        {
            if (error != nullptr)
            {
                *error = message;
            }
            return false;
        }

        bool Plausible(const float value, const float limit) { return std::isfinite(value) && std::fabs(value) <= limit; }

        bool ParseExercise(const json& node, const int number, RlTrainingExercise& e, std::string* error)
        {
            const std::string where = "exercise " + std::to_string(number);
            if (!node.is_object())
            {
                return Fail(error, where + ": an object is expected");
            }
            e.id = Text(node, "id", "ex" + std::to_string(number));
            e.name = Text(node, "name", e.id);
            e.hint = Text(node, "hint");
            e.timeLimit = Number(node, "timeLimit", e.timeLimit);
            e.attempts = static_cast<int>(Number(node, "attempts", static_cast<float>(e.attempts)));
            if (e.id.empty() || !(e.timeLimit > 0.0f) || e.timeLimit > 120.0f || e.attempts < 1 || e.attempts > 20)
            {
                return Fail(error, where + " (" + e.id + "): id, timeLimit (0..120 s) or attempts (1..20) is wrong");
            }

            const auto success = node.find("success");
            const std::string type = (success != node.end() && success->is_object()) ? Text(*success, "type") : std::string();
            if (type == "goal")
            {
                e.condition = RlTrainingCondition::Goal;
            }
            else if (type == "saved")
            {
                e.condition = RlTrainingCondition::Saved;
            }
            else if (type == "touchAbove")
            {
                e.condition = RlTrainingCondition::TouchAbove;
                e.height = Number(*success, "height", e.height);
            }
            else if (type == "kickoffWin")
            {
                e.condition = RlTrainingCondition::KickoffWin;
                e.distance = Number(*success, "distance", e.distance);
            }
            else
            {
                return Fail(error, where + " (" + e.id + "): success.type must be goal, saved, touchAbove or kickoffWin");
            }

            const auto kickoff = node.find("kickoff");
            if (kickoff != node.end())
            {
                if (!kickoff->is_object() || e.condition != RlTrainingCondition::KickoffWin)
                {
                    return Fail(error, where + " (" + e.id + "): kickoff is an object and goes with success.type kickoffWin");
                }
                e.kickoff = true;
                e.kickoffSpot = static_cast<int>(Number(*kickoff, "spot", 0.0f));
                e.botLevel = static_cast<int>(Number(*kickoff, "botLevel", 2.0f));
                if (e.kickoffSpot < 0 || e.kickoffSpot > 4 || e.botLevel < 0 || e.botLevel > 2)
                {
                    return Fail(error, where + " (" + e.id + "): kickoff.spot is 0..4, kickoff.botLevel 0..2");
                }
            }
            else if (e.condition == RlTrainingCondition::KickoffWin)
            {
                return Fail(error, where + " (" + e.id + "): kickoffWin needs the kickoff object");
            }

            const auto car = node.find("car");
            if (car != node.end() && car->is_object())
            {
                e.carX = Number(*car, "x", e.carX);
                e.carZ = Number(*car, "z", e.carZ);
                e.carYaw = Number(*car, "yaw", e.carYaw);
                e.carBoost = Number(*car, "boost", e.carBoost);
            }
            else if (!e.kickoff)
            {
                return Fail(error, where + " (" + e.id + "): the car object is missing");
            }
            const auto ball = node.find("ball");
            if (ball != node.end() && ball->is_object())
            {
                if (!Vector(*ball, "position", e.ballPosition) || !Vector(*ball, "velocity", e.ballVelocity) || !Vector(*ball, "spin", e.ballSpin))
                {
                    return Fail(error, where + " (" + e.id + "): ball.position, velocity and spin are [x, y, z]");
                }
            }
            else if (!e.kickoff)
            {
                return Fail(error, where + " (" + e.id + "): the ball object is missing");
            }
            if (!Plausible(e.carX, 60.0f) || !Plausible(e.carZ, 70.0f) || !Plausible(e.ballPosition.x, 60.0f) || !Plausible(e.ballPosition.z, 70.0f) ||
                !Plausible(e.ballPosition.y, 30.0f) || e.ballPosition.y < 0.0f || !std::isfinite(e.carYaw) || e.carBoost < 0.0f || e.carBoost > 100.0f ||
                !Plausible(e.ballVelocity.x, 80.0f) || !Plausible(e.ballVelocity.y, 80.0f) || !Plausible(e.ballVelocity.z, 80.0f))
            {
                return Fail(error, where + " (" + e.id + "): a position, a speed or the boost is out of range");
            }
            return true;
        }
    }

    bool RlLoadTrainingPackFromText(const std::string& text, RlTrainingPack& out, std::string* error)
    {
        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            return Fail(error, "the pack is not a json object");
        }
        RlTrainingPack pack;
        pack.id = Text(root, "id");
        pack.name = Text(root, "name", pack.id);
        pack.description = Text(root, "description");
        pack.order = static_cast<int>(Number(root, "order", 0.0f));
        const auto list = root.find("exercises");
        if (pack.id.empty() || list == root.end() || !list->is_array() || list->empty())
        {
            return Fail(error, "the pack needs an id and a list of exercises");
        }
        for (std::size_t i = 0; i < list->size(); ++i)
        {
            RlTrainingExercise exercise;
            if (!ParseExercise((*list)[i], static_cast<int>(i) + 1, exercise, error))
            {
                return false;
            }
            for (const RlTrainingExercise& other : pack.exercises)
            {
                if (other.id == exercise.id)
                {
                    return Fail(error, "exercise " + std::to_string(i + 1) + ": the id " + exercise.id + " is used twice");
                }
            }
            pack.exercises.push_back(std::move(exercise));
        }
        out = std::move(pack);
        return true;
    }

    bool RlLoadTrainingPackFromFile(const std::string& path, RlTrainingPack& out, std::string* error)
    {
        std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
        if (!file)
        {
            return Fail(error, path + ": cannot be opened");
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string message;
        if (!RlLoadTrainingPackFromText(buffer.str(), out, &message))
        {
            return Fail(error, path + ": " + message);
        }
        return true;
    }

    std::vector<RlTrainingPack> RlLoadTrainingPacks(const std::string& directory, std::vector<std::string>* errors)
    {
        std::vector<RlTrainingPack> packs;
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(std::filesystem::u8path(directory), ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file() && it->path().extension() == ".json")
            {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const std::filesystem::path& file : files)
        {
            RlTrainingPack pack;
            std::string error;
            if (RlLoadTrainingPackFromFile(file.generic_u8string(), pack, &error))
            {
                packs.push_back(std::move(pack));
            }
            else if (errors != nullptr)
            {
                errors->push_back(error);
            }
        }
        std::stable_sort(packs.begin(), packs.end(), [](const RlTrainingPack& a, const RlTrainingPack& b) {
            return a.order != b.order ? a.order < b.order : a.name < b.name;
        });
        return packs;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------

    namespace
    {
        double Rounded(const float value) { return std::round(static_cast<double>(value) * 1000.0) / 1000.0; }
    }

    bool RlTrainingResults::LoadFromText(const std::string& text, std::string* error)
    {
        entries_.clear();
        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            return Fail(error, "the results are not a json object");
        }
        const auto packs = root.find("packs");
        if (packs == root.end() || !packs->is_object())
        {
            return true;
        }
        for (auto pack = packs->begin(); pack != packs->end(); ++pack)
        {
            if (!pack.value().is_object())
            {
                continue;
            }
            for (auto exercise = pack.value().begin(); exercise != pack.value().end(); ++exercise)
            {
                if (!exercise.value().is_object())
                {
                    continue;
                }
                RlTrainingExerciseResult r;
                r.successes = std::max(0, static_cast<int>(Number(exercise.value(), "successes", 0.0f)));
                r.attempts = std::max(0, static_cast<int>(Number(exercise.value(), "attempts", 0.0f)));
                r.bestTime = std::max(0.0f, Number(exercise.value(), "bestTime", 0.0f));
                r.bestHitSpeed = std::max(0.0f, Number(exercise.value(), "bestHitSpeed", 0.0f));
                entries_[{pack.key(), exercise.key()}] = r;
            }
        }
        return true;
    }

    bool RlTrainingResults::LoadFromFile(const std::string& path, std::string* error)
    {
        entries_.clear();
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::u8path(path), ec))
        {
            return true;  // nothing played yet
        }
        std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
        if (!file)
        {
            return Fail(error, path + ": cannot be opened");
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string message;
        if (!LoadFromText(buffer.str(), &message))
        {
            return Fail(error, path + ": " + message);
        }
        return true;
    }

    std::string RlTrainingResults::ToText() const
    {
        json packs = json::object();
        for (const auto& entry : entries_)
        {
            json r = json::object();
            r["successes"] = entry.second.successes;
            r["attempts"] = entry.second.attempts;
            r["bestTime"] = Rounded(entry.second.bestTime);
            r["bestHitSpeed"] = Rounded(entry.second.bestHitSpeed);
            packs[entry.first.first][entry.first.second] = r;
        }
        json root = json::object();
        root["version"] = 1;
        root["packs"] = packs;
        return root.dump(2) + "\n";
    }

    bool RlTrainingResults::SaveToFile(const std::string& path, std::string* error) const
    {
        const std::filesystem::path target = std::filesystem::u8path(path);
        std::error_code ec;
        if (target.has_parent_path())
        {
            std::filesystem::create_directories(target.parent_path(), ec);
        }
        // written beside and moved over: a crash in the middle does not leave half a file
        std::filesystem::path temp = target;
        temp += ".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                return Fail(error, path + ": cannot be written");
            }
            file << ToText();
            if (!file)
            {
                return Fail(error, path + ": cannot be written");
            }
        }
        std::filesystem::rename(temp, target, ec);
        if (ec)
        {
            std::filesystem::remove(target, ec);
            std::filesystem::rename(temp, target, ec);
        }
        return ec ? Fail(error, path + ": " + ec.message()) : true;
    }

    RlTrainingExerciseResult RlTrainingResults::Get(const std::string& packId, const std::string& exerciseId) const
    {
        const auto it = entries_.find({packId, exerciseId});
        return it == entries_.end() ? RlTrainingExerciseResult{} : it->second;
    }

    void RlTrainingResults::Record(const std::string& packId, const std::string& exerciseId, const bool success, const float time, const float hitSpeed)
    {
        RlTrainingExerciseResult& r = entries_[{packId, exerciseId}];
        ++r.attempts;
        if (success)
        {
            ++r.successes;
            if (r.bestTime <= 0.0f || time < r.bestTime)
            {
                r.bestTime = time;
            }
            r.bestHitSpeed = std::max(r.bestHitSpeed, hitSpeed);
        }
    }

    int RlTrainingResults::PackSuccesses(const RlTrainingPack& pack) const
    {
        int count = 0;
        for (const RlTrainingExercise& exercise : pack.exercises)
        {
            count += Get(pack.id, exercise.id).successes > 0 ? 1 : 0;
        }
        return count;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------

    namespace
    {
        float Speed(const RlVec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

        // the car looks along +Z of its own frame
        RlVec3 FlatForward(const RlQuat& q)
        {
            const float x = 2.0f * (q.x * q.z + q.w * q.y);
            const float z = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
            const float length = std::sqrt(x * x + z * z);
            return length > 1e-4f ? RlVec3{x / length, 0.0f, z / length} : RlVec3{0.0f, 0.0f, 1.0f};
        }
    }

    struct RlTraining::Impl
    {
        RlTuning tuning;
        std::unique_ptr<RlSim> sim;       // free play and the exercises that start from a standstill: one car
        std::unique_ptr<RlMatch> match;   // a kick-off against a bot: the match of mode 1 (slot 0 is the player, slot 1 the bot)
        RlTrainingMode mode = RlTrainingMode::FreePlay;
        RlTrainingPhase phase = RlTrainingPhase::Running;
        RlTrainingPack pack;
        int index = 0;
        int attemptsUsed = 0;
        bool outOfAttempts = false;
        bool newBest = false;
        RlTrainingMiss missReason = RlTrainingMiss::None;

        float readyTimer = 0.0f;
        float elapsed = 0.0f;
        float bannerTime = 0.0f;
        bool infiniteBoost = false;

        // counters (free play: since ResetCounters; a pack: since the attempt began)
        int hits = 0;
        float lastHitSpeed = 0.0f;
        float bestHitSpeed = 0.0f;
        float maxTouchHeight = 0.0f;

        // what the conditions watch in an attempt
        bool touched = false;
        int firstToucher = -1;
        float sinceTouch = 0.0f;

        RlWorldState world;
        RlWorldState previous;
        RlEvents events;
        RlTrainingResults results;
        std::string resultsPath;

        explicit Impl(const RlTuning& t)
            : tuning(t)
        {
            sim = std::make_unique<RlSim>(tuning, 1);
            StartFreePlay();
        }

        RlSim& ActiveSim() { return match ? match->MutableSim() : *sim; }
        const RlSim& ActiveSim() const { return match ? match->Sim() : *sim; }
        const RlTrainingExercise* Exercise() const
        {
            return (mode == RlTrainingMode::Pack && phase != RlTrainingPhase::PackDone && index >= 0 && index < static_cast<int>(pack.exercises.size()))
                       ? &pack.exercises[static_cast<std::size_t>(index)]
                       : nullptr;
        }

        void RefreshWorld()
        {
            world = ActiveSim().State();
            if (match)
            {
                world.match = match->World().match;
                return;
            }
            RlMatchState m;
            m.mode = 0;
            m.phase = phase == RlTrainingPhase::Ready ? RlMatchPhase::Countdown : RlMatchPhase::Playing;
            m.phaseTimer = phase == RlTrainingPhase::Ready ? std::max(readyTimer, 0.0f) : 0.0f;
            m.clock = 0.0f;
            world.match = m;
        }

        void ClearCounters()
        {
            hits = 0;
            lastHitSpeed = 0.0f;
            bestHitSpeed = 0.0f;
            maxTouchHeight = 0.0f;
        }

        void ClearAttempt()
        {
            ClearCounters();
            touched = false;
            firstToucher = -1;
            sinceTouch = 0.0f;
            elapsed = 0.0f;
            bannerTime = 0.0f;
            missReason = RlTrainingMiss::None;
            outOfAttempts = false;
            newBest = false;
        }

        void PutCar(const float x, const float z, const float yaw, const float boost)
        {
            ActiveSim().ResetCar(0, x, z, yaw);
            ActiveSim().SetCarBoost(0, boost);
        }

        // ---- free play

        void StartFreePlay()
        {
            match.reset();
            mode = RlTrainingMode::FreePlay;
            phase = RlTrainingPhase::Running;
            ClearAttempt();
            const RlSpot spot = RlKickoffSpot(0, 0);
            PutCar(spot.x, spot.z, spot.yaw, tuning.car.boostStart);
            sim->ResetBall({0.0f, tuning.ball.radius + 0.05f, 0.0f});
            RefreshWorld();
            previous = world;
        }

        // ---- packs

        void BeginExercise(const int number)
        {
            index = std::clamp(number, 0, static_cast<int>(pack.exercises.size()) - 1);
            attemptsUsed = 0;
            BeginAttempt();
        }

        void BeginAttempt()
        {
            const RlTrainingExercise& e = pack.exercises[static_cast<std::size_t>(index)];
            ClearAttempt();
            phase = RlTrainingPhase::Ready;
            if (e.kickoff)
            {
                RlMatchConfig config;
                config.mode = 1;
                config.seed = 7919u + static_cast<uint64_t>(index) * 31u + static_cast<uint64_t>(attemptsUsed);
                config.humanMask = 1;
                config.botLevel = e.botLevel;
                match = std::make_unique<RlMatch>(tuning, config);
                const RlSpot mine = RlKickoffSpot(0, e.kickoffSpot);
                const RlSpot theirs = RlKickoffSpot(1, e.kickoffSpot);
                match->MutableSim().ResetCar(0, mine.x, mine.z, mine.yaw);
                match->MutableSim().SetCarBoost(0, e.carBoost);
                match->MutableSim().ResetCar(1, theirs.x, theirs.z, theirs.yaw);
                readyTimer = 0.0f;
            }
            else
            {
                match.reset();
                PutCar(e.carX, e.carZ, e.carYaw, e.carBoost);
                sim->ResetBall(e.ballPosition, e.ballVelocity, e.ballSpin);
                readyTimer = kReadySeconds;
            }
            RefreshWorld();
            previous = world;
        }

        void Finish(const bool success, const RlTrainingMiss reason)
        {
            const RlTrainingExercise& e = pack.exercises[static_cast<std::size_t>(index)];
            const RlTrainingExerciseResult before = results.Get(pack.id, e.id);
            results.Record(pack.id, e.id, success, elapsed, lastHitSpeed);
            const RlTrainingExerciseResult after = results.Get(pack.id, e.id);
            bannerTime = 0.0f;
            if (success)
            {
                phase = RlTrainingPhase::Success;
                newBest = after.bestTime != before.bestTime || after.bestHitSpeed != before.bestHitSpeed;
                missReason = RlTrainingMiss::None;
            }
            else
            {
                phase = RlTrainingPhase::Miss;
                missReason = reason;
                ++attemptsUsed;
                outOfAttempts = attemptsUsed >= e.attempts;
            }
            if (!resultsPath.empty())
            {
                results.SaveToFile(resultsPath);
            }
        }

        void AdvanceAfterBanner()
        {
            if (phase == RlTrainingPhase::Success || (phase == RlTrainingPhase::Miss && outOfAttempts))
            {
                GoNext();
            }
            else
            {
                BeginAttempt();
            }
        }

        void GoNext()
        {
            if (index + 1 >= static_cast<int>(pack.exercises.size()))
            {
                phase = RlTrainingPhase::PackDone;
                bannerTime = 0.0f;
            }
            else
            {
                BeginExercise(index + 1);
            }
        }

        // ---- the tick

        int GoalSide(const RlVec3& p) const
        {
            const float line = tuning.arena.halfLength + tuning.ball.radius + tuning.match.goalLineMargin;
            if (std::fabs(p.z) > line && std::fabs(p.x) < tuning.arena.goalHalfWidth && p.y < tuning.arena.goalHeight)
            {
                return p.z > 0.0f ? 1 : -1;
            }
            return 0;
        }

        void StepBackend(const RlCarInput& input)
        {
            if (match)
            {
                const RlCarInput inputs[2] = {input, RlCarInput{}};
                match->Step(inputs);
                events = match->LastEvents();
            }
            else
            {
                sim->Step(&input);
                events = sim->LastEvents();
            }
        }

        void Step(const RlCarInput& input)
        {
            previous = world;
            if (mode == RlTrainingMode::Pack && phase == RlTrainingPhase::Ready && !match)
            {
                readyTimer -= kTickDt;
                events = {};
                if (readyTimer <= 0.0f)
                {
                    phase = RlTrainingPhase::Running;
                }
                RefreshWorld();
                previous = world;  // frozen: nothing to interpolate
                return;
            }

            if (infiniteBoost && mode == RlTrainingMode::FreePlay)
            {
                ActiveSim().SetCarBoost(0, 100.0f);
            }
            StepBackend(input);

            const RlWorldState& state = ActiveSim().State();
            const bool hitByCar = events.ballHits > 0 && state.lastBallToucher == 0;
            if (hitByCar)
            {
                ++hits;
                lastHitSpeed = Speed(state.ball.linearVelocity);
                bestHitSpeed = std::max(bestHitSpeed, lastHitSpeed);
                maxTouchHeight = std::max(maxTouchHeight, state.ball.position.y);
            }

            switch (phase)
            {
            case RlTrainingPhase::Ready:  // a kick-off: the countdown of the match
                if (match && match->World().match.phase == RlMatchPhase::Playing)
                {
                    phase = RlTrainingPhase::Running;
                }
                break;
            case RlTrainingPhase::Running:
                if (mode == RlTrainingMode::FreePlay)
                {
                    elapsed += kTickDt;
                }
                else
                {
                    Evaluate(state, hitByCar);
                }
                break;
            case RlTrainingPhase::Success:
            case RlTrainingPhase::Miss:
                bannerTime += kTickDt;
                if (bannerTime >= kBannerSeconds)
                {
                    AdvanceAfterBanner();
                }
                break;
            case RlTrainingPhase::PackDone:
                bannerTime += kTickDt;
                break;
            }
            RefreshWorld();
        }

        void Evaluate(const RlWorldState& state, const bool hitByCar)
        {
            const RlTrainingExercise& e = pack.exercises[static_cast<std::size_t>(index)];
            elapsed += kTickDt;
            const int goal = GoalSide(state.ball.position);
            const bool anyHit = events.ballHits > 0;
            const bool firstTouch = anyHit && !touched;
            if (firstTouch)
            {
                touched = true;
                firstToucher = state.lastBallToucher;
                sinceTouch = 0.0f;
            }
            if (hitByCar)
            {
                sinceTouch = 0.0f;
            }
            else if (touched)
            {
                sinceTouch += kTickDt;
            }

            switch (e.condition)
            {
            case RlTrainingCondition::Goal:
                if (goal > 0)
                {
                    Finish(true, RlTrainingMiss::None);
                    return;
                }
                if (goal < 0)
                {
                    Finish(false, RlTrainingMiss::OwnGoal);
                    return;
                }
                break;
            case RlTrainingCondition::Saved:
                if (goal < 0)
                {
                    Finish(false, RlTrainingMiss::OwnGoal);
                    return;
                }
                if (hits > 0 && (state.ball.linearVelocity.z > kSavedBallSpeed || sinceTouch >= kSaveSeconds))
                {
                    Finish(true, RlTrainingMiss::None);
                    return;
                }
                break;
            case RlTrainingCondition::TouchAbove:
                if (hitByCar && state.ball.position.y >= e.height)
                {
                    Finish(true, RlTrainingMiss::None);
                    return;
                }
                break;
            case RlTrainingCondition::KickoffWin:
                if (firstTouch && firstToucher != 0)
                {
                    Finish(false, RlTrainingMiss::LostKickoff);
                    return;
                }
                if (goal > 0 || (hits > 0 && state.ball.position.z >= e.distance))
                {
                    Finish(true, RlTrainingMiss::None);
                    return;
                }
                if (goal < 0 || (hits > 0 && state.ball.position.z <= -e.distance))
                {
                    Finish(false, RlTrainingMiss::LostKickoff);
                    return;
                }
                break;
            }
            if (elapsed >= e.timeLimit)
            {
                Finish(false, RlTrainingMiss::TimeUp);
            }
        }
    };

    RlTraining::RlTraining(const RlTuning& tuning)
        : impl_(std::make_unique<Impl>(tuning))
    {
    }

    RlTraining::~RlTraining() = default;

    void RlTraining::StartFreePlay() { impl_->StartFreePlay(); }

    bool RlTraining::StartPack(const RlTrainingPack& pack, const int exercise)
    {
        if (pack.exercises.empty())
        {
            return false;
        }
        impl_->pack = pack;
        impl_->mode = RlTrainingMode::Pack;
        impl_->BeginExercise(exercise);
        return true;
    }

    void RlTraining::SetResultsFile(const std::string& path)
    {
        impl_->resultsPath = path;
        if (!path.empty())
        {
            impl_->results.LoadFromFile(path);
        }
    }

    RlTrainingResults& RlTraining::Results() { return impl_->results; }
    const RlTrainingResults& RlTraining::Results() const { return impl_->results; }

    void RlTraining::Step(const RlCarInput& input) { impl_->Step(input); }

    void RlTraining::Retry()
    {
        Impl& m = *impl_;
        if (m.mode != RlTrainingMode::Pack)
        {
            return;
        }
        if (m.phase == RlTrainingPhase::PackDone)
        {
            m.BeginExercise(0);
        }
        else if (m.outOfAttempts)
        {
            m.BeginExercise(m.index);
        }
        else
        {
            m.BeginAttempt();
        }
    }

    void RlTraining::Next()
    {
        Impl& m = *impl_;
        if (m.mode == RlTrainingMode::Pack && m.phase != RlTrainingPhase::PackDone)
        {
            m.GoNext();
        }
    }

    void RlTraining::Previous()
    {
        Impl& m = *impl_;
        if (m.mode != RlTrainingMode::Pack)
        {
            return;
        }
        m.BeginExercise(m.phase == RlTrainingPhase::PackDone ? static_cast<int>(m.pack.exercises.size()) - 1 : m.index - 1);
    }

    void RlTraining::RestartPack()
    {
        if (impl_->mode == RlTrainingMode::Pack)
        {
            impl_->BeginExercise(0);
        }
    }

    void RlTraining::SetInfiniteBoost(const bool on) { impl_->infiniteBoost = on; }
    bool RlTraining::InfiniteBoost() const { return impl_->infiniteBoost; }

    void RlTraining::ResetBallToCar(const float distance)
    {
        Impl& m = *impl_;
        if (m.mode != RlTrainingMode::FreePlay)
        {
            return;
        }
        const RlCarState& car = m.sim->State().cars[0];
        const RlVec3 forward = FlatForward(car.rotation);
        m.sim->ResetBall({car.position.x + forward.x * distance, m.tuning.ball.radius + 0.05f, car.position.z + forward.z * distance});
        m.RefreshWorld();
    }

    void RlTraining::BallInAir(const float distance, const float height)
    {
        Impl& m = *impl_;
        if (m.mode != RlTrainingMode::FreePlay)
        {
            return;
        }
        const RlCarState& car = m.sim->State().cars[0];
        const RlVec3 forward = FlatForward(car.rotation);
        const float limitX = m.tuning.arena.halfWidth - 3.0f;
        const float limitZ = m.tuning.arena.halfLength - 3.0f;
        const float x = std::clamp(car.position.x + forward.x * distance, -limitX, limitX);
        const float z = std::clamp(car.position.z + forward.z * distance, -limitZ, limitZ);
        const float y = std::clamp(height, m.tuning.ball.radius + 1.0f, m.tuning.arena.height - 3.0f);
        m.sim->ResetBall({x, y, z}, {0.0f, 6.0f, 0.0f});
        m.RefreshWorld();
    }

    void RlTraining::ResetCar()
    {
        Impl& m = *impl_;
        if (m.mode != RlTrainingMode::FreePlay)
        {
            return;
        }
        const RlSpot spot = RlKickoffSpot(0, 0);
        m.PutCar(spot.x, spot.z, spot.yaw, m.tuning.car.boostStart);
        m.RefreshWorld();
    }

    void RlTraining::ResetCounters() { impl_->ClearCounters(); }

    const RlWorldState& RlTraining::World() const { return impl_->world; }
    const RlWorldState& RlTraining::PreviousWorld() const { return impl_->previous; }
    const RlEvents& RlTraining::LastEvents() const { return impl_->events; }
    const RlTuning& RlTraining::Tuning() const { return impl_->tuning; }
    RlTrainingMode RlTraining::Mode() const { return impl_->mode; }
    RlTrainingPhase RlTraining::Phase() const { return impl_->phase; }
    const RlTrainingPack& RlTraining::Pack() const { return impl_->pack; }
    const RlTrainingExercise* RlTraining::CurrentExercise() const { return impl_->Exercise(); }
    const RlSim& RlTraining::Sim() const { return impl_->ActiveSim(); }
    RlSim& RlTraining::MutableSim() { return impl_->ActiveSim(); }

    RlTrainingInfo RlTraining::Info() const
    {
        const Impl& m = *impl_;
        RlTrainingInfo info;
        info.mode = m.mode;
        info.phase = m.phase;
        info.missReason = m.missReason;
        info.elapsed = m.elapsed;
        info.bannerTime = m.bannerTime;
        info.infiniteBoost = m.infiniteBoost;
        info.hits = m.hits;
        info.lastHitSpeed = m.lastHitSpeed;
        info.bestHitSpeed = m.bestHitSpeed;
        info.maxTouchHeight = m.maxTouchHeight;
        info.outOfAttempts = m.outOfAttempts;
        info.newBest = m.newBest;
        if (m.mode == RlTrainingMode::FreePlay)
        {
            info.packName = "Free play";
            return info;
        }
        info.packId = m.pack.id;
        info.packName = m.pack.name;
        info.exerciseCount = static_cast<int>(m.pack.exercises.size());
        info.packDoneSuccesses = m.results.PackSuccesses(m.pack);
        const RlTrainingExercise* e = m.Exercise();
        if (e == nullptr)
        {
            info.exerciseIndex = info.exerciseCount - 1;
            return info;
        }
        info.exerciseIndex = m.index;
        info.exerciseName = e->name;
        info.hint = e->hint;
        info.attempts = e->attempts;
        info.attempt = m.phase == RlTrainingPhase::Miss ? m.attemptsUsed : std::min(m.attemptsUsed + 1, e->attempts);
        info.timeLimit = e->timeLimit;
        info.timeLeft = std::max(e->timeLimit - m.elapsed, 0.0f);
        info.best = m.results.Get(m.pack.id, e->id);
        return info;
    }
}
