// FlySwarm.cpp

#include <doomfly/FlySwarm.h>

#include <doomfly/FlyModes.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

namespace doomfly
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr double kTicSeconds = 1.0 / 35.0;
        constexpr double kTicMs = 1000.0 / 35.0;
        constexpr int kMaxSlots = 32;
        constexpr double kPi = 3.14159265358979323846;

        // ---- the rules of the swarm: the numbers are in one place ----
        constexpr double kBaseSpeed = 10.5;          // map units a tic (the player walks at 8, runs at 16)
        constexpr double kSpeedPerWave = 0.35;
        constexpr double kMaxBaseSpeed = 14.0;
        constexpr double kLeapSpeed = 21.0;
        constexpr int kLeapTics = 7;
        constexpr int kLeapCooldown = 24;
        constexpr double kPursuitTurn = 9.0;         // degrees a tic towards the player, far away (the turning circle is 67 units: it does not catch anything near)
        constexpr double kPursuitTurnNear = 30.0;    // ... and close, where the fly is agile (a wing beat from a bite)
        constexpr double kBrainTurn = 14.0;          // degrees a tic at turn = 1
        constexpr double kBiteRange = 44.0;
        constexpr double kBiteHeight = 46.0;
        constexpr int kBiteCooldown = 24;
        constexpr double kNoticeShot = 0.08;         // the chance a tic that a fly in the line of a firing weapon sees the flash
        constexpr int kFlyHealth = 5;                // any hit of a weapon of the player kills (the pistol does 5..15)
        constexpr int kWaveBreakTics = 105;          // 3 s between the waves
        constexpr int kLostTics = 245;               // a fly that has not seen the player for 7 s is brought back where the player can see it (the walls of DOOM are not a maze it solves)
        constexpr int kCorpseTics = 175;             // a dead fly lies there 5 s
        constexpr double kEyeHeight = 41.0;

        double Wrap180(double degrees)
        {
            degrees = std::fmod(degrees, 360.0);
            if (degrees > 180.0)
            {
                degrees -= 360.0;
            }
            else if (degrees <= -180.0)
            {
                degrees += 360.0;
            }
            return degrees;
        }

        double Wrap360(double degrees)
        {
            degrees = std::fmod(degrees, 360.0);
            return degrees < 0.0 ? degrees + 360.0 : degrees;
        }

        double ToDegrees(const double radians)
        {
            return radians * 180.0 / kPi;
        }

        double ToRadians(const double degrees)
        {
            return degrees * kPi / 180.0;
        }

        struct Agent
        {
            int handle = -1;
            bool used = false;
            bool alive = false;
            double heading = 0.0;          // degrees
            double distance = 0.0;
            double lastDistance = 0.0;
            double lastX = 0.0;
            double lastY = 0.0;
            int leapTics = 0;
            int leapCooldown = 0;
            double leapHeading = 0.0;
            int biteCooldown = 0;
            int deathTics = 0;
            std::uint32_t lastEscapeSpikes = 0;
            int buzzIn = 0;
            int blockedTics = 0;
            int navTics = 0;
            int unseenTics = 0;            // how long the fly has had no line of sight to the player
            double orbit = 0.0;            // where round the player the fly aims: an angle
            double phase = 0.0;            // the bob of the flight
            double speedScale = 1.0;
            flybrain::SwarmInput input;
        };
    }

    struct FlySwarm::Impl
    {
        SwarmConfig config;
        std::unique_ptr<flybrain::FlySwarmBrain> brain;
        std::array<Agent, kMaxSlots> agents;
        SwarmStats stats;
        std::mt19937_64 random;
        int lastLevelTime = -1;
        double accumulator = 0.0;
        bool havePlayer = false;
        double lastPlayerX = 0.0;
        double lastPlayerY = 0.0;
        double playerVx = 0.0;
        double playerVy = 0.0;
        int startWave = 1;
        int bannerTics = 0;
        int deadTics = 0;
        int ticsSinceStart = 0;
        double brainMsAverage = 0.0;
        std::function<void(const std::string&)> log;

        double Uniform(const double from, const double to)
        {
            return std::uniform_real_distribution<double>(from, to)(random);
        }

        void Log(const std::string& text) const
        {
            if (log)
            {
                log(text);
            }
        }

        int WaveSize(const int wave) const
        {
            return std::min(config.maxFlies, 8 + 4 * (wave - 1));
        }

        void ResetFlies()
        {
            for (Agent& agent : agents)
            {
                if (agent.used && agent.handle >= 0)
                {
                    doom::swarm::Remove(agent.handle);
                }
                agent = Agent{};
            }
            doom::swarm::Reset();
            stats.flies = 0;
            stats.wave = startWave - 1;
            stats.waveSize = 0;
            stats.ticsToNextWave = 35;   // the first wave comes a second after the start
            stats.playerDead = false;
            bannerTics = 0;
            deadTics = 0;
            havePlayer = false;
        }

        void SpawnWave(const doom::PlayerState& player)
        {
            ++stats.wave;
            const int wanted = WaveSize(stats.wave);
            int spawned = 0;
            // the slots are the replicas of the brain: no more than it has. The corpses of the wave before give theirs back.
            const int slots = static_cast<int>(brain->FlyCount());
            for (int slot = 0; slot < slots && spawned < wanted; ++slot)
            {
                Agent& taken = agents[static_cast<std::size_t>(slot)];
                if (taken.used && taken.alive)
                {
                    continue;
                }
                if (taken.used)
                {
                    doom::swarm::Remove(taken.handle);
                    taken = Agent{};
                }
                int handle = -1;
                for (int attempt = 0; attempt < 60 && handle < 0; ++attempt)
                {
                    // far at first, nearer when the rooms are small
                    const double near = attempt < 40 ? 380.0 : 140.0;
                    const double far = attempt < 40 ? 950.0 : 420.0;
                    const double angle = Uniform(0.0, 2.0 * kPi);
                    const double distance = Uniform(near, far);
                    const double x = player.x + std::cos(angle) * distance;
                    const double y = player.y + std::sin(angle) * distance;
                    double floorZ = 0.0;
                    double ceilingZ = 0.0;
                    if (!doom::swarm::SpotOk(x, y, floorZ, ceilingZ))
                    {
                        continue;
                    }
                    const double z = std::clamp(player.z + Uniform(10.0, 50.0), floorZ + 8.0, ceilingZ - 52.0);
                    const double toPlayer = ToDegrees(std::atan2(player.y - y, player.x - x));
                    handle = doom::swarm::Spawn(x, y, z, toPlayer, kFlyHealth, attempt < 45);   // the first tries are where the player can see; the rooms with no such place give the rest
                    if (handle >= 0)
                    {
                        Agent& agent = agents[static_cast<std::size_t>(slot)];
                        agent = Agent{};
                        agent.used = true;
                        agent.alive = true;
                        agent.handle = handle;
                        agent.heading = toPlayer;
                        agent.distance = distance;
                        agent.lastDistance = distance;
                        agent.lastX = x;
                        agent.lastY = y;
                        agent.orbit = Uniform(0.0, 2.0 * kPi);
                        agent.phase = Uniform(0.0, 2.0 * kPi);
                        agent.speedScale = Uniform(0.9, 1.1);
                        agent.buzzIn = static_cast<int>(Uniform(2.0, 30.0));
                        // a fly that was in this slot before: the escape spikes of the brain go on counting
                        agent.lastEscapeSpikes = brain->Output(static_cast<std::uint32_t>(slot)).escapeSpikes;
                        ++spawned;
                    }
                }
            }
            stats.waveSize = spawned;
            bannerTics = 70;
            Log("swarm: wave " + std::to_string(stats.wave) + ", " + std::to_string(spawned) + " flies of " + std::to_string(wanted));
        }

        // A fly that is lost behind the walls comes back: a new place that the player can see, 250..700 units away (nothing changes when there is none)
        void Relocate(Agent& agent, const doom::PlayerState& player)
        {
            agent.unseenTics = 0;
            for (int attempt = 0; attempt < 40; ++attempt)
            {
                const double angle = Uniform(0.0, 2.0 * kPi);
                const double distance = Uniform(250.0, 700.0);
                const double x = player.x + std::cos(angle) * distance;
                const double y = player.y + std::sin(angle) * distance;
                double floorZ = 0.0;
                double ceilingZ = 0.0;
                if (!doom::swarm::SpotOk(x, y, floorZ, ceilingZ))
                {
                    continue;
                }
                const double z = std::clamp(player.z + Uniform(10.0, 50.0), floorZ + 8.0, ceilingZ - 52.0);
                const int handle = doom::swarm::Spawn(x, y, z, ToDegrees(std::atan2(player.y - y, player.x - x)), kFlyHealth, true);
                if (handle >= 0)
                {
                    doom::swarm::Remove(agent.handle);
                    agent.handle = handle;
                    agent.lastX = x;
                    agent.lastY = y;
                    agent.heading = ToDegrees(std::atan2(player.y - y, player.x - x));
                    agent.navTics = 0;
                    agent.blockedTics = 0;
                    return;
                }
            }
        }

        // The looming that the fly sees on its left and right: the player rushing at it, a weapon pointing at it
        void ComputeInput(Agent& agent, const doom::PlayerState& player, const bool firing, const doom::swarm::Fly& fly)
        {
            const double dx = player.x - fly.x;
            const double dy = player.y - fly.y;
            const double distance = std::max(1.0, std::hypot(dx, dy));
            const double bearing = Wrap180(ToDegrees(std::atan2(dy, dx)) - agent.heading);   // + : the player is on the left of the fly
            agent.distance = distance;
            double loom = 0.0;
            if (std::fabs(bearing) < 120.0 && distance < 900.0)
            {
                // the speed of the player towards the fly, in units a tic
                const double toFlyX = -dx / distance;
                const double toFlyY = -dy / distance;
                const double rushing = std::max(0.0, playerVx * toFlyX + playerVy * toFlyY);
                loom += std::min(1.0, rushing / 12.0) * std::min(1.0, 0.35 + 110.0 / distance);
                // the muzzle: the player fires and the line of the weapon passes close to the fly. The fly notices it now and then (a shot is a flash, not a
                // steady light): one tic in a dozen of the aim on it. The weapon of DOOM shoots at once, so a fly that has noticed leaves the line a tic after the
                // shot, and the rest are hit.
                if (firing && distance < 1500.0)
                {
                    const double aimOff = std::fabs(Wrap180(ToDegrees(std::atan2(-dy, -dx)) - player.angleDegrees));
                    const double reach = ToDegrees(std::atan2(26.0, distance));   // the angle the fly takes up
                    if (aimOff < reach + 9.0 && Uniform(0.0, 1.0) < kNoticeShot)
                    {
                        loom += 0.5;
                    }
                }
            }
            loom = std::clamp(loom, 0.0, 1.0);
            agent.input.loomLeft = bearing >= 0.0 ? static_cast<float>(loom) : 0.0f;
            agent.input.loomRight = bearing < 0.0 ? static_cast<float>(loom) : 0.0f;
            agent.input.brightness = 0.3f;
        }

        void Think(const doom::PlayerState& player, const bool firing)
        {
            std::array<doom::swarm::Fly, kMaxSlots> flies{};
            double turnSum = 0.0;
            double escapeSum = 0.0;
            double loomSum = 0.0;
            int aliveCount = 0;
            for (int slot = 0; slot < kMaxSlots; ++slot)
            {
                Agent& agent = agents[static_cast<std::size_t>(slot)];
                if (!agent.used)
                {
                    continue;
                }
                doom::swarm::Fly& fly = flies[static_cast<std::size_t>(slot)];
                if (!doom::swarm::Get(agent.handle, fly))
                {
                    agent = Agent{};
                    continue;
                }
                if (agent.alive && !fly.alive)
                {
                    // killed
                    agent.alive = false;
                    agent.deathTics = 0;
                    doom::swarm::Fall(agent.handle);
                    ++stats.killed;
                }
                if (agent.alive)
                {
                    ComputeInput(agent, player, firing, fly);
                    brain->SetInput(static_cast<std::uint32_t>(slot), agent.input);
                    ++aliveCount;
                    loomSum += std::max(agent.input.loomLeft, agent.input.loomRight);
                }
                else
                {
                    brain->SetInput(static_cast<std::uint32_t>(slot), flybrain::SwarmInput{});
                }
            }
            stats.flies = aliveCount;

            const auto began = std::chrono::steady_clock::now();
            brain->AdvanceMs(kTicMs);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            brainMsAverage = brainMsAverage == 0.0 ? ms : brainMsAverage * 0.95 + ms * 0.05;
            stats.brainMsPerTic = brainMsAverage;

            int buzzed = 0;
            for (int slot = 0; slot < kMaxSlots; ++slot)
            {
                Agent& agent = agents[static_cast<std::size_t>(slot)];
                if (!agent.used)
                {
                    continue;
                }
                const doom::swarm::Fly& fly = flies[static_cast<std::size_t>(slot)];
                if (!agent.alive)
                {
                    if (++agent.deathTics > kCorpseTics)
                    {
                        doom::swarm::Remove(agent.handle);
                        agent = Agent{};
                    }
                    continue;
                }
                const flybrain::SwarmOutput& out = brain->Output(static_cast<std::uint32_t>(slot));
                turnSum += out.turn;
                escapeSum += out.escape;

                // ---- the direction: the hunt is a reflex of the game, the brain bends it ----
                const bool sees = doom::swarm::Sees(agent.handle);
                agent.unseenTics = sees ? 0 : agent.unseenTics + 1;
                if (agent.unseenTics > kLostTics)
                {
                    Relocate(agent, player);
                }
                const double toPlayer = ToDegrees(std::atan2(player.y - fly.y, player.x - fly.x));
                double wanted = toPlayer;
                if (!sees || agent.navTics > 0)
                {
                    wanted = doom::swarm::NavAngle(agent.handle, agent.blockedTics >= 2 && agent.navTics <= 0);
                    if (agent.navTics > 0)
                    {
                        --agent.navTics;
                    }
                }
                else
                {
                    // aim at a point on a ring round the player, so that the flies do not come in one line and do not all sit on one spot
                    agent.orbit += 0.02;
                    const double ax = player.x + std::cos(agent.orbit) * 22.0;
                    const double ay = player.y + std::sin(agent.orbit) * 22.0;
                    wanted = ToDegrees(std::atan2(ay - fly.y, ax - fly.x));
                }
                double heading = agent.heading;
                const double nearness = std::clamp((200.0 - agent.distance) / 120.0, 0.0, 1.0);
                const double pursuitRate = kPursuitTurn + (kPursuitTurnNear - kPursuitTurn) * nearness;
                const double pursuit = std::clamp(Wrap180(wanted - heading), -pursuitRate, pursuitRate);
                // the brain: turn > 0 is to the right (clockwise, the angle falls)
                heading += pursuit - out.turn * kBrainTurn;

                // ---- the escape jump of the giant fibre ----
                if (agent.leapCooldown > 0)
                {
                    --agent.leapCooldown;
                }
                if (out.escapeSpikes != agent.lastEscapeSpikes)
                {
                    if (agent.leapTics == 0 && agent.leapCooldown == 0)
                    {
                        agent.leapTics = kLeapTics;
                        agent.leapCooldown = kLeapCooldown;
                        const double away = toPlayer + 180.0;
                        agent.leapHeading = Wrap360(away + Uniform(-65.0, 65.0));   // aside of the line of the weapon
                        ++stats.leaps;
                    }
                    agent.lastEscapeSpikes = out.escapeSpikes;
                }
                double speed = std::min(kMaxBaseSpeed, kBaseSpeed + kSpeedPerWave * static_cast<double>(stats.wave - 1)) * agent.speedScale;
                speed *= 1.0 + 0.35 * std::clamp(static_cast<double>(out.forward), -1.0, 1.0);
                if (sees && agent.distance < 90.0)
                {
                    speed *= std::clamp(agent.distance / 90.0, 0.5, 1.0);   // slows down to land on the player, not to fly past
                }
                if (agent.leapTics > 0)
                {
                    --agent.leapTics;
                    heading = agent.leapHeading;
                    speed = kLeapSpeed;
                }
                heading = Wrap360(heading);
                agent.heading = heading;

                // ---- the other flies: keep a little apart ----
                double pushX = 0.0;
                double pushY = 0.0;
                for (int other = 0; other < kMaxSlots; ++other)
                {
                    if (other == slot || !agents[static_cast<std::size_t>(other)].alive)
                    {
                        continue;
                    }
                    const doom::swarm::Fly& near = flies[static_cast<std::size_t>(other)];
                    const double sx = fly.x - near.x;
                    const double sy = fly.y - near.y;
                    const double separation = std::hypot(sx, sy);
                    if (separation < 26.0 && separation > 0.01)
                    {
                        pushX += sx / separation * (26.0 - separation) * 0.12;
                        pushY += sy / separation * (26.0 - separation) * 0.12;
                    }
                }

                // ---- the height: about the eyes of the player, with a bob ----
                agent.phase += 0.23;
                const double wantedZ = player.z + kEyeHeight - 14.0 + std::sin(agent.phase) * 7.0;
                const double lowest = fly.floorZ + 3.0;
                const double highest = fly.ceilingZ - 40.0 - 3.0;
                const double climb = std::clamp((std::clamp(wantedZ, lowest, std::max(lowest, highest)) - fly.z) * 0.3, -4.5, 4.5);

                const double radians = ToRadians(heading);
                const double momx = std::cos(radians) * speed + pushX;
                const double momy = std::sin(radians) * speed + pushY;
                const int frame = ((ticsSinceStart / 2) + slot) % 2;
                doom::swarm::Drive(agent.handle, heading, momx, momy, climb, frame);

                if (--agent.buzzIn <= 0)
                {
                    agent.buzzIn = static_cast<int>(Uniform(14.0, 34.0));
                    if (agent.distance < 900.0 && buzzed < 3)
                    {
                        doom::swarm::Buzz(agent.handle);
                        ++buzzed;
                    }
                }
            }
            stats.meanTurn = aliveCount > 0 ? static_cast<float>(turnSum / aliveCount) : 0.0f;
            stats.meanEscape = aliveCount > 0 ? static_cast<float>(escapeSum / aliveCount) : 0.0f;
            stats.meanLoom = aliveCount > 0 ? static_cast<float>(loomSum / aliveCount) : 0.0f;
        }

        // After the tic of DOOM: the bites, and which of the flies are stuck on a wall
        void Contacts(const doom::PlayerState& player, const int skill)
        {
            for (Agent& agent : agents)
            {
                if (!agent.used || !agent.alive)
                {
                    continue;
                }
                doom::swarm::Fly fly;
                if (!doom::swarm::Get(agent.handle, fly) || !fly.alive)
                {
                    continue;
                }
                const double dx = player.x - fly.x;
                const double dy = player.y - fly.y;
                const double distance = std::hypot(dx, dy);
                const double rise = fly.z - player.z;
                if (agent.biteCooldown > 0)
                {
                    --agent.biteCooldown;
                }
                if (distance < kBiteRange && rise > -24.0 && rise < kBiteHeight && agent.biteCooldown == 0 && player.health > 0)
                {
                    const int damage = (skill <= 1 ? 1 : 1 + static_cast<int>(Uniform(0.0, 2.0)));   // 1..2 (1 on the easiest levels)
                    doom::swarm::Bite(agent.handle, damage);
                    agent.biteCooldown = kBiteCooldown;
                    stats.biteDamage += damage;
                    ++stats.bites;
                }
                // stuck: the fly meant to move and did not
                const double moved = std::hypot(fly.x - agent.lastX, fly.y - agent.lastY);
                agent.lastX = fly.x;
                agent.lastY = fly.y;
                if (distance > kBiteRange + 12.0 && moved < 2.0 && agent.leapTics == 0)
                {
                    if (++agent.blockedTics >= 2)
                    {
                        agent.navTics = 18;
                    }
                }
                else
                {
                    agent.blockedTics = 0;
                }
            }
        }
    };

    FlySwarm::FlySwarm(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    FlySwarm::~FlySwarm()
    {
        // the flies are mobjs of the game; the game is destroyed after the controller (DoomGameFlow), nothing to take out here
    }

    const SwarmStats& FlySwarm::Stats() const
    {
        return impl_->stats;
    }

    std::unique_ptr<FlySwarm> FlySwarm::Create(const SwarmConfig& config, std::string& error)
    {
        if (!config.circuit)
        {
            error = "the circuit of the swarm is not loaded";
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        impl->config = config;
        impl->config.maxFlies = std::clamp(config.maxFlies, 8, kMaxSlots);
        impl->log = config.log;
        impl->random.seed(config.seed);
        flybrain::SwarmOptions options;
        options.backend = config.backend;
        options.parallel = config.parallel;
        impl->brain = flybrain::FlySwarmBrain::Create(config.circuit, static_cast<std::uint32_t>(impl->config.maxFlies), options, &error);
        if (!impl->brain)
        {
            return nullptr;
        }
        impl->startWave = std::max(1, config.startWave);
        impl->stats.wave = impl->startWave - 1;
        impl->stats.backend = impl->brain->GetBackend();
        impl->stats.ticsToNextWave = 35;
        impl->Log(std::string("swarm: ") + std::to_string(impl->config.maxFlies) + " brains of " + std::to_string(config.circuit->NeuronCount()) + " neurons on the " +
                  flybrain::ToString(impl->stats.backend));
        return std::unique_ptr<FlySwarm>(new FlySwarm(std::move(impl)));
    }

    bool FlySwarm::StepTic(doom::DoomGame& game)
    {
        Impl& s = *impl_;
        const doom::GameInfo info = game.Info();
        const int levelTime = doom::swarm::LevelTime();
        if (info.state != doom::GameState::Level || levelTime < 0)
        {
            s.lastLevelTime = -1;
            return game.StepTic();
        }
        if (levelTime < s.lastLevelTime || s.lastLevelTime < 0)
        {
            // a new level (or the same one again after a death): DOOM has freed the mobjs
            s.ResetFlies();
            s.accumulator = 0.0;
        }
        s.lastLevelTime = levelTime;

        const doom::PlayerState player = game.Player(info.consolePlayer);
        if (s.havePlayer)
        {
            s.playerVx = player.x - s.lastPlayerX;
            s.playerVy = player.y - s.lastPlayerY;
        }
        s.havePlayer = true;
        s.lastPlayerX = player.x;
        s.lastPlayerY = player.y;
        doom::TicCmd lastCommand;
        game.GetTicCmd(info.consolePlayer, lastCommand);
        const bool firing = (lastCommand.buttons & doom::kButtonAttack) != 0;

        ++s.ticsSinceStart;
        ++s.stats.tics;
        s.stats.playerDead = player.health <= 0;
        if (s.bannerTics > 0)
        {
            --s.bannerTics;
        }
        if (!s.stats.playerDead)
        {
            if (s.stats.flies == 0 && s.stats.ticsToNextWave > 0)
            {
                --s.stats.ticsToNextWave;
                if (s.stats.ticsToNextWave == 0)
                {
                    s.SpawnWave(player);
                }
            }
            s.Think(player, firing);
        }
        else
        {
            ++s.deadTics;
            // the flies hang about: they do not hunt a body
            for (Agent& agent : s.agents)
            {
                if (agent.used && agent.alive)
                {
                    doom::swarm::Drive(agent.handle, agent.heading, 0.0, 0.0, 0.0, (s.ticsSinceStart / 2) % 2);
                }
            }
        }
        const bool frame = game.StepTic();
        s.Contacts(game.Player(info.consolePlayer), info.skill);
        if (s.stats.flies == 0 && s.stats.wave > 0 && s.stats.ticsToNextWave == 0 && !s.stats.playerDead)
        {
            // the field is clear: the next wave in a few seconds
            bool anyAlive = false;
            for (const Agent& agent : s.agents)
            {
                anyAlive = anyAlive || (agent.used && agent.alive);
            }
            if (!anyAlive)
            {
                s.stats.ticsToNextWave = kWaveBreakTics;
            }
        }
        return frame;
    }

    bool FlySwarm::Advance(doom::DoomGame& game, const double deltaSeconds)
    {
        Impl& s = *impl_;
        const doom::GameInfo info = game.Info();
        const bool playing = info.state == doom::GameState::Level && !info.menuActive && !info.paused && !info.demoPlayback;
        if (!playing)
        {
            // the menu, the title screen, the pause: DOOM keeps its own clock
            return game.Advance(deltaSeconds);
        }
        s.accumulator += std::min(deltaSeconds, 0.25);
        bool frame = false;
        for (int steps = 0; s.accumulator >= kTicSeconds && steps < 5; ++steps)
        {
            frame = StepTic(game) || frame;
            s.accumulator -= kTicSeconds;
        }
        return frame;
    }

    std::vector<FlySwarm::FlyView> FlySwarm::Flies() const
    {
        std::vector<FlyView> views;
        for (int slot = 0; slot < kMaxSlots; ++slot)
        {
            const Agent& agent = impl_->agents[static_cast<std::size_t>(slot)];
            if (!agent.used)
            {
                continue;
            }
            doom::swarm::Fly fly;
            if (!doom::swarm::Get(agent.handle, fly))
            {
                continue;
            }
            FlyView view;
            view.slot = slot;
            view.alive = agent.alive;
            view.x = fly.x;
            view.y = fly.y;
            view.z = fly.z;
            view.heading = agent.heading;
            view.distance = agent.distance;
            view.sees = agent.alive && doom::swarm::Sees(agent.handle);
            view.input = agent.input;
            view.output = impl_->brain->Output(static_cast<std::uint32_t>(slot));
            views.push_back(view);
        }
        return views;
    }

    std::string FlySwarm::Status() const
    {
        const SwarmStats& stats = impl_->stats;
        char text[160];
        std::snprintf(text, sizeof(text), "Swarm: wave %d, %d flies, %d killed, brain %s %.1f ms/tic", stats.wave, stats.flies, stats.killed, flybrain::ToString(stats.backend),
                      stats.brainMsPerTic);
        return text;
    }

    void FlySwarm::DrawPanel(ImDrawList*, float, float, float, float) {}

    void FlySwarm::DrawOverlay(ImDrawList* list, const float width, const float height)
    {
        const SwarmStats& stats = impl_->stats;
        ImFont* font = ImGui::GetFont();
        const float size = std::max(14.0f, std::floor(height / 34.0f));
        char line[160];
        std::snprintf(line, sizeof(line), "SWARM   WAVE %d   FLIES %d / %d   KILLED %d", stats.wave, stats.flies, stats.waveSize, stats.killed);
        const float margin = size * 0.6f;
        list->AddText(font, size, ImVec2(margin + 1.0f, margin + 1.0f), IM_COL32(0, 0, 0, 200), line);
        list->AddText(font, size, ImVec2(margin, margin), IM_COL32(255, 220, 90, 255), line);
        std::snprintf(line, sizeof(line), "brain %s  %.1f ms per tic  turn %+.2f  escape %.2f", flybrain::ToString(stats.backend), stats.brainMsPerTic, stats.meanTurn, stats.meanEscape);
        list->AddText(font, size * 0.8f, ImVec2(margin, margin + size * 1.2f), IM_COL32(160, 200, 255, 220), line);

        auto centred = [&](const char* text, const float y, const float scale, const ImU32 colour)
        {
            const float fontSize = size * scale;
            const ImVec2 extent = font->CalcTextSizeA(fontSize, 1.0e9f, 0.0f, text);
            const float x = (width - extent.x) * 0.5f;
            list->AddText(font, fontSize, ImVec2(x + 2.0f, y + 2.0f), IM_COL32(0, 0, 0, 220), text);
            list->AddText(font, fontSize, ImVec2(x, y), colour, text);
        };
        if (stats.playerDead)
        {
            std::snprintf(line, sizeof(line), "THE SWARM GOT YOU   wave %d, %d flies killed", stats.wave, stats.killed);
            centred(line, height * 0.35f, 1.6f, IM_COL32(255, 80, 60, 255));
            centred("press USE to try again", height * 0.35f + size * 2.6f, 1.0f, IM_COL32(230, 230, 230, 255));
        }
        else if (impl_->bannerTics > 0)
        {
            std::snprintf(line, sizeof(line), "WAVE %d", stats.wave);
            centred(line, height * 0.3f, 2.6f, IM_COL32(255, 220, 90, 255));
        }
        else if (stats.flies == 0 && stats.wave > 0 && stats.ticsToNextWave > 0)
        {
            std::snprintf(line, sizeof(line), "WAVE %d CLEARED   next in %d", stats.wave, (stats.ticsToNextWave + 34) / 35);
            centred(line, height * 0.3f, 1.5f, IM_COL32(120, 255, 140, 255));
        }
    }

    std::filesystem::path FindSwarmCircuit(const FlyModeOptions& options)
    {
        std::vector<fs::path> candidates;
        if (!options.exeDirectory.empty())
        {
            candidates.push_back(options.exeDirectory / "assets" / "flybrain" / "swarm_circuit.fbrain");
        }
        if (!options.sourceDirectory.empty())
        {
            candidates.push_back(options.sourceDirectory / "assets" / "flybrain" / "swarm_circuit.fbrain");
        }
        std::error_code ec;
        for (const fs::path& candidate : candidates)
        {
            if (fs::is_regular_file(candidate, ec))
            {
                return candidate;
            }
        }
        return candidates.empty() ? fs::path("assets/flybrain/swarm_circuit.fbrain") : candidates.front();
    }

    bool SwarmAvailable(const FlyModeOptions& options, std::string& reason)
    {
        std::error_code ec;
        const fs::path path = FindSwarmCircuit(options);
        if (!fs::is_regular_file(path, ec))
        {
            reason = "no circuit of the swarm: " + path.string() + " (build/tests/RelWithDebInfo/myengine_flybrain_circuit.exe makes it from the connectome)";
            return false;
        }
        return true;
    }

    const std::vector<std::string>& SwarmDoomArgs()
    {
        // no monsters of DOOM (only the flies attack), the sprites and the buzz of the flies in DOOM (game/doom DoomSwarm.h), E1M1 at once (a -warp of the player
        // in --doom-args comes first in the command line and wins)
        static const std::vector<std::string> args = {"-nomonsters", doom::swarm::kStartArgument, "-warp", "1", "1"};
        return args;
    }

    std::unique_ptr<myengine::game::IDoomController> MakeSwarmController(const FlyModeOptions& options, const int flies, const std::string& backendName, const std::uint64_t seed, std::string& error)
    {
        const fs::path path = FindSwarmCircuit(options);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec))
        {
            error = "the circuit of the swarm is not found: " + path.string();
            return nullptr;
        }
        SwarmConfig config;
        config.circuit = flybrain::Connectome::Load(path.u8string(), &error);
        if (!config.circuit)
        {
            return nullptr;
        }
        config.maxFlies = flies;
        config.seed = seed != 0 ? seed : options.seed;
        config.log = options.log;
        flybrain::BackendChoice backend = flybrain::BackendChoice::Auto;
        flybrain::ParseBackendChoice(backendName, backend);
        config.backend = backend;
        return FlySwarm::Create(config, error);
    }
}
