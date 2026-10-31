// FlyLearnMeasure.cpp
// N3b (FlyDoom): does the fly learn in the game? The fly plays one map life after life with the learning brain (FlyPlayerConfig::learning,
// controls_learn.json); the arms differ by learning on or off only, with the same seed. While the player is dead the tool presses "use" to come
// back. Prints every life and a summary; docs/flybrain/flybrain.md, section "Муха учится в игре".
//   per life: seconds, damage, damage per second, kills, the KC spikes per second, the mean learned scale, and the turn away from the attacker:
//   for every hit with a known attacker, how much the angle between the view and the attacker grew in the second after the hit (degrees)
//
//   myengine_fly_learn_measure --learning 0|1 [--lives 20] [--max-minutes 30] [--seed 1] [--skill 4] [--map 1] [--backend auto|cpu|gpu]
//                              [--trace <file>] (one line per tic: the player, the command, the frame hash, the brain step)
//                              [--controls controls_learn.json] [--plain] (the brain of D1a, without the learning mode: a reference)
//                              [--kc-rate 120] (KcVisionConfig::maxRateHz; 0: the KCs see nothing)
//                              [--calibrate] (the rates of the controls on even frames, for the offsets of the controls)
// N4: pushing against a wall (FlyStatus::wallHits / wallTics) and the trained fly:
//                              [--eta 5e-4] (the learning rate) [--wall 0|1] (pushing against a wall is punishment, default 1)
//                              [--memory-in <file>] (start with these weights) [--memory-out <file>] (save the weights at the end)
//                              [--bin-seconds 30] (a line every this many seconds of the game: wall hits per minute, wall tics, way, turns, lives)
//                              --lives 0: no limit of lives, the run ends after --max-minutes of the game
//                              [--wall-sense 0|1] (our layer WallSense, not the connectome) [--wall-learn 0|1] [--wall-in <file>]... (the places put together)
//                              [--wall-out <file>] [--iwad <wad>] (default %LOCALAPPDATA%\FlyDoom\wads\doom1.wad); --max-minutes 0 only puts the --wall-in files together
//                              [--substeps 4] (FlyPlayerConfig::substeps)

#include <doom/DoomGame.h>
#include <doomfly/FlyControls.h>
#include <doomfly/FlyPlayer.h>
#include <flybrain/FlyBrain.h>
#include <myengine/jobs/JobSystem.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // Forwards the commands of the fly; while the player is dead it presses "use" every other tic to come back
    struct Respawner : doom::ITicCmdSource
    {
        doomfly::FlyPlayer* fly = nullptr;
        bool dead = false;
        int tic = 0;
        doom::TicCmd last;
        void BuildTicCmd(int player, doom::TicCmd& cmd) override
        {
            fly->BuildTicCmd(player, cmd);
            if (player == 0 && dead)
            {
                cmd.buttons = (++tic % 2) ? doom::kButtonUse : 0;
                cmd.forwardMove = 0;
                cmd.sideMove = 0;
                cmd.angleTurn = 0;
            }
            if (player == 0)
            {
                last = cmd;
            }
        }
    };

    // The angle (degrees, 0 .. 180) between the view of the player and the direction to the attacker
    double OffAttacker(const doom::PlayerState& p)
    {
        const double to = std::atan2(p.attackerY - p.y, p.attackerX - p.x) * 180.0 / 3.14159265358979;
        double d = std::fmod(std::fabs(to - p.angleDegrees), 360.0);
        return d > 180.0 ? 360.0 - d : d;
    }

    struct Hit
    {
        int tic = 0;
        double offAtHit = 0.0;
    };

    struct Life
    {
        int tics = 0;
        int damage = 0;
        int kills = 0;
        bool died = false;
        double kcSum = 0.0;          // KC spikes per second, summed over the tics
        double scale = 1.0;
        std::vector<Hit> open;       // hits waiting for their second
        double awaySum = 0.0;
        int awayCount = 0;
    };

    double Mean(const std::vector<double>& v)
    {
        double s = 0.0;
        for (double x : v)
        {
            s += x;
        }
        return v.empty() ? 0.0 : s / static_cast<double>(v.size());
    }
}

int main(int argc, char** argv)
{
    int learning = 1;
    int lifeTarget = 20;
    double maxMinutes = 30.0;
    uint64_t seed = 1;
    std::string skill = "4";
    std::string map = "1";
    std::string backend = "auto";
    std::string tracePath;
    std::string controls = "controls_learn.json";
    bool plain = false;
    bool calibrate = false;
    float kcRate = -1.0f;
    double eta = -1.0;
    int wallPunish = 1;
    std::string memoryIn, memoryOut;
    double binSeconds = 30.0;
    int substeps = 0;
    int wallSense = 0;
    int whiskers = 0;   // N5: the whiskers (authored sensor)
    int assist = 1;     // the authored visible-target combat assist (on, as in the Fly modes)
    std::string dumpPath;
    int wallLearn = 1;
    std::vector<std::string> wallIn;
    std::string wallOut;
    std::string iwad;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (arg == "--learning") learning = std::atoi(next().c_str());
        else if (arg == "--lives") lifeTarget = std::atoi(next().c_str());
        else if (arg == "--max-minutes") maxMinutes = std::atof(next().c_str());
        else if (arg == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (arg == "--skill") skill = next();
        else if (arg == "--map") map = next();
        else if (arg == "--backend") backend = next();
        else if (arg == "--trace") tracePath = next();
        else if (arg == "--controls") controls = next();
        else if (arg == "--plain") plain = true;
        else if (arg == "--calibrate") calibrate = true;
        else if (arg == "--kc-rate") kcRate = static_cast<float>(std::atof(next().c_str()));
        else if (arg == "--eta") eta = std::atof(next().c_str());
        else if (arg == "--wall") wallPunish = std::atoi(next().c_str());
        else if (arg == "--memory-in") memoryIn = next();
        else if (arg == "--memory-out") memoryOut = next();
        else if (arg == "--bin-seconds") binSeconds = std::atof(next().c_str());
        else if (arg == "--substeps") substeps = std::atoi(next().c_str());
        else if (arg == "--wall-sense") wallSense = std::atoi(next().c_str());
        else if (arg == "--whiskers") whiskers = std::atoi(next().c_str());
        else if (arg == "--assist") assist = std::atoi(next().c_str());
        else if (arg == "--dump") dumpPath = next();
        else if (arg == "--wall-learn") wallLearn = std::atoi(next().c_str());
        else if (arg == "--wall-in") wallIn.push_back(next());
        else if (arg == "--iwad") iwad = next();
        else if (arg == "--wall-out") wallOut = next();
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    myengine::jobs::Initialize();

    const fs::path source = fs::path(MYENGINE_SOURCE_DIR);
    std::string error;
    auto connectome = flybrain::Connectome::Load((source / "assets/flybrain/malecns_v1.fbrain").u8string(), &error);
    if (!connectome)
    {
        std::printf("connectome: %s\n", error.c_str());
        return 77;
    }
    wchar_t* local = nullptr;
    size_t length = 0;
    _wdupenv_s(&local, &length, L"LOCALAPPDATA");
    const fs::path wad = !iwad.empty() ? fs::u8path(iwad) : fs::path(local ? local : L"") / L"FlyDoom" / L"wads" / L"doom1.wad";
    std::free(local);
    if (!fs::exists(wad))
    {
        std::printf("no WAD at %s\n", wad.u8string().c_str());
        return 77;
    }

    doom::DoomConfig config;
    config.iwad = wad;
    config.dataDirectory = fs::temp_directory_path() / "myengine_fly_learn";
    std::error_code removed;
    fs::remove_all(config.dataDirectory, removed);   // no default.cfg of an earlier run: every run starts the same
    config.extraArgs = { "-warp", "1", map, "-skill", skill, "-nomusic" };
    auto game = doom::DoomGame::Create(config, error);
    if (!game)
    {
        std::printf("DOOM: %s\n", error.c_str());
        return 1;
    }
    // the seed also shifts the start: the brain of D1a has no noise of its own, so the game must differ (the monsters have moved for 13 x (seed - 1) tics)
    for (int i = 0; i < 10 + 13 * static_cast<int>(seed - 1); ++i)
    {
        game->StepTic();
    }

    doomfly::FlyPlayerConfig fly;
    fly.connectome = connectome;
    fly.seed = seed;
    fly.backend = backend == "cpu" ? flybrain::BackendChoice::Cpu : backend == "gpu" ? flybrain::BackendChoice::Gpu : flybrain::BackendChoice::Auto;
    fly.pace = false;
    fly.learning.enabled = !plain;
    fly.learning.learning = learning != 0;
    if (kcRate >= 0.0f)
    {
        fly.learning.vision.maxRateHz = kcRate;
    }
    if (eta >= 0.0)
    {
        fly.learning.learningRate = eta;
    }
    fly.learning.wallPunishment = wallPunish != 0;
    fly.wallSense.enabled = wallSense != 0;
    fly.wallSense.learning = wallLearn != 0;
    fly.wallSenseLoad = wallIn;
    fly.wallSenseSave = wallOut;
    // Match interactive Fly modes: this authored target gate/steering is
    // outside the connectome and is reported separately below.
    fly.combatAssist.enabled = assist != 0;
    fly.whiskers.enabled = whiskers != 0;
    if (substeps > 0)
    {
        fly.substeps = substeps;
    }
    // the player loads its memory with the first brain and saves it in its destructor: a copy of --memory-in in the data folder, so the input stays as it is
    const fs::path memoryWork = config.dataDirectory / "fly_memory.bin";
    fs::create_directories(config.dataDirectory, removed);
    if (!memoryIn.empty() && !fs::copy_file(fs::u8path(memoryIn), memoryWork, fs::copy_options::overwrite_existing, removed))
    {
        std::printf("no memory at %s\n", memoryIn.c_str());
        return 1;
    }
    fly.learning.memoryPath = memoryIn.empty() && memoryOut.empty() ? std::string() : memoryWork.u8string();
    fly.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
    if (!doomfly::FlyControls::Load(source / "assets/flybrain" / controls, fly.controls, error))
    {
        std::printf("controls: %s\n", error.c_str());
        return 1;
    }
    auto player = doomfly::FlyPlayer::Create(fly, error);
    if (!player)
    {
        std::printf("player: %s\n", error.c_str());
        return 1;
    }
    if (calibrate)
    {
        // The rates of the groups of the controls on even grey, dark and bright frames: the offsets of the controls of this brain (as N1 did for DOOMFLY)
        for (const float grey : { 0.5f, 0.1f, 0.9f })
        {
            const uint32_t c = static_cast<uint32_t>(grey * 255.0f);
            std::vector<uint32_t> frame(static_cast<size_t>(doom::kScreenWidth) * doom::kScreenHeight, (c << 16) | (c << 8) | c);
            auto still = doomfly::FlyPlayer::Create(fly, error);
            const doomfly::FlyStatus status = still->EvaluateFrameForTest(frame.data(), 140);
            std::printf("calibrate grey %.1f: turn %.2f Hz, forward %.2f Hz, fire %.2f Hz;", grey, status.turnHz, status.forwardHz, status.fireHz);
            for (const doomfly::RateReading& r : status.rates)
            {
                std::printf(" %s %.1f;", r.name.c_str(), r.hz);
            }
            std::printf("\n");
        }
        return 0;
    }
    player->Attach(*game);
    Respawner respawner;
    respawner.fly = player.get();
    game->SetTicCmdSource(&respawner);
    FILE* trace = nullptr;
    if (!tracePath.empty() && fopen_s(&trace, tracePath.c_str(), "w") != 0)
    {
        trace = nullptr;
    }

    const int maxTics = static_cast<int>(maxMinutes * 60.0 * 35.0);
    std::vector<Life> lives(1);
    doom::PlayerState prev = game->Player(0);
    const auto begin = std::chrono::steady_clock::now();
    // N4: the bins of the game time
    struct Bin
    {
        uint32_t wallHits = 0;
        uint64_t wallTics = 0;
        int aliveTics = 0;
        double way = 0.0;
        int left = 0, right = 0;   // tics of the brain's turn (not the reflex) to the left / right, more than 0.5 degrees
        int standing = 0;          // alive tics when the player has gone less than 16 units in the last second (stands, at a wall or not)
        int damage = 0;
        int deaths = 0;
    };
    const int binTics = std::max(35, static_cast<int>(binSeconds * 35.0));
    Bin bin, total;
    uint32_t lastHits = 0;
    uint64_t lastWallTics = 0;
    auto addBin = [](Bin& to, const Bin& b)
    {
        to.wallHits += b.wallHits; to.wallTics += b.wallTics; to.aliveTics += b.aliveTics; to.way += b.way;
        to.left += b.left; to.right += b.right; to.damage += b.damage; to.deaths += b.deaths; to.standing += b.standing;
    };
    auto printBin = [](const char* what, const Bin& b, double fromS, double toS, double scale)
    {
        const double minutes = b.aliveTics / 35.0 / 60.0;
        std::printf("%s %.0f-%.0f s: wall hits %u (%.1f / min), wall tics %llu (%.1f %%), way %.0f (%.0f / min, %.1f wall hits per 1000), turns left %d right %d, standing %.1f %%, damage %d, deaths %d, mean scale %.5f\n",
            what, fromS, toS, b.wallHits, minutes > 0 ? b.wallHits / minutes : 0.0, static_cast<unsigned long long>(b.wallTics),
            b.aliveTics ? 100.0 * b.wallTics / b.aliveTics : 0.0, b.way, minutes > 0 ? b.way / minutes : 0.0, b.way > 0 ? 1000.0 * b.wallHits / b.way : 0.0, b.left, b.right, b.aliveTics ? 100.0 * b.standing / b.aliveTics : 0.0, b.damage, b.deaths, scale);
    };
    std::vector<std::pair<double, double>> lastSecond;   // the positions of the last 35 tics
    // --dump: the columns (x, y as floats once), then per tic: alive, touching, walking (bytes), forward, turn (floats), the light of every column (bytes)
    FILE* dump = nullptr;
    if (!dumpPath.empty() && fopen_s(&dump, dumpPath.c_str(), "wb") == 0)
    {
        const uint32_t n = static_cast<uint32_t>(player->Eyes().Columns().size());
        std::fwrite(&n, 4, 1, dump);
        for (const doomfly::Column& c : player->Eyes().Columns())
        {
            std::fwrite(&c.x, 4, 1, dump);
            std::fwrite(&c.y, 4, 1, dump);
        }
    }
    uint64_t dumpWallTics = 0;
    int brainFireTics = 0;
    int targetVisibleTics = 0;
    int actualFireTics = 0;
    // N5: the attacks of the player over the run (DoomGame counts them natively: bullets and pellets, punches, missiles; hits on living hostiles)
    const doom::PlayerState combatStart = game->Player(0);
    int killsAll = 0;
    int doorPasses = 0;      // N5: the times the player walked into a door sector (out of another sector)
    int tic = 0;
    for (; tic < maxTics; ++tic)
    {
        const uint64_t frame = trace != nullptr ? game->FrameHash() : 0;
        player->StepBlocking(*game);
        const doom::PlayerState p = game->Player(0);
        const doomfly::FlyStatus status = player->Status();
        brainFireTics += status.brainFire ? 1 : 0;
        targetVisibleTics += status.targetVisible ? 1 : 0;
        actualFireTics += status.fire ? 1 : 0;
        Life& life = lives.back();
        if (dump != nullptr)
        {
            const std::vector<float>& columns = player->LastColumns();
            const uint8_t flags[3] = {static_cast<uint8_t>(!respawner.dead && p.health > 0), static_cast<uint8_t>(status.wallTics > dumpWallTics),
                                      static_cast<uint8_t>(status.forwardUnits > 3.0f && !status.reflex)};
            std::fwrite(flags, 1, 3, dump);
            std::fwrite(&status.forwardUnits, 4, 1, dump);
            std::fwrite(&status.turnDegrees, 4, 1, dump);
            std::vector<uint8_t> bytes(player->Eyes().Columns().size(), 0);
            for (size_t c = 0; c < bytes.size() && c < columns.size(); ++c)
            {
                bytes[c] = static_cast<uint8_t>(std::clamp(columns[c], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            std::fwrite(bytes.data(), 1, bytes.size(), dump);
            dumpWallTics = status.wallTics;
        }
        bin.wallHits += status.wallHits - lastHits;
        bin.wallTics += status.wallTics - lastWallTics;
        lastHits = status.wallHits;
        lastWallTics = status.wallTics;
        if (!respawner.dead && p.health > 0)
        {
            ++bin.aliveTics;
            lastSecond.push_back({p.x, p.y});
            if (lastSecond.size() > 35)
            {
                lastSecond.erase(lastSecond.begin());
            }
            if (lastSecond.size() == 35 && std::hypot(lastSecond.back().first - lastSecond.front().first, lastSecond.back().second - lastSecond.front().second) < 16.0)
            {
                ++bin.standing;
            }
            bin.way += std::hypot(p.x - prev.x, p.y - prev.y) < 64.0 ? std::hypot(p.x - prev.x, p.y - prev.y) : 0.0;   // not a teleport or a respawn
            if (!status.reflex && status.turnDegrees > 0.5f) ++bin.right;
            if (!status.reflex && status.turnDegrees < -0.5f) ++bin.left;
            bin.damage += std::max(0, prev.health - p.health);
        }
        if (!respawner.dead && p.health <= 0 && prev.health > 0)
        {
            lastSecond.clear();
            ++bin.deaths;
        }
        if ((tic + 1) % binTics == 0)
        {
            printBin("bin", bin, (tic + 1 - binTics) / 35.0, (tic + 1) / 35.0, status.learnedScale);
            addBin(total, bin);
            bin = Bin{};
        }
        if (trace != nullptr)
        {
            std::fprintf(trace, "%d hp %d pos %.4f %.4f ang %.4f cmd %d %d %u frame %016llx step %llu turnHz %.1f forwardHz %.1f central %.0f kc %.0f\n", tic, p.health, p.x,
                p.y, p.angleDegrees, respawner.last.forwardMove, respawner.last.angleTurn, respawner.last.buttons, static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(player->Snapshot().step), status.turnHz, status.forwardHz, status.centralSpikesPerSecond, status.kcSpikesPerSecond);
            // N5: what steered this tic: the brain, our layer WallSense (chance, brake), the reflex of the legs, the push against a wall
            std::fprintf(trace, "%d n5 turnDeg %.2f fwdUnits %.1f reflex %d wallChance %.2f wallBrake %.0f wallTics %llu target %d fire %d brainFire %d whisker %d ahead %.0f\n", tic, status.turnDegrees,
                status.forwardUnits, status.reflex ? 1 : 0, status.wallChance, status.wallBrake, static_cast<unsigned long long>(status.wallTics), status.targetVisible ? 1 : 0,
                status.fire ? 1 : 0, status.brainFire ? 1 : 0, status.whiskerTurn ? 1 : 0, status.whiskerAhead);
        }
        if (!respawner.dead)
        {
            ++life.tics;
            const int hurt = std::max(0, prev.health - p.health);
            life.damage += hurt;
            life.kills += std::max(0, p.kills - prev.kills);
            life.kcSum += status.kcSpikesPerSecond;
            if (hurt > 0 && p.hasAttacker && p.health > 0)
            {
                life.open.push_back({ tic, OffAttacker(p) });
            }
            for (auto it = life.open.begin(); it != life.open.end();)
            {
                if (tic - it->tic >= 35)
                {
                    if (p.hasAttacker)
                    {
                        life.awaySum += OffAttacker(p) - it->offAtHit;
                        ++life.awayCount;
                    }
                    it = life.open.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            if (p.health <= 0)
            {
                life.died = true;
                life.scale = status.learnedScale;
                respawner.dead = true;
                const double seconds = life.tics / 35.0;
                std::printf("life %zu: %.1f s, damage %d (%.2f / s), kills %d, KC %.0f spikes / s, turn away %+.1f deg over %d hits, mean scale %.5f\n", lives.size(),
                    seconds, life.damage, seconds > 0 ? life.damage / seconds : 0.0, life.kills, life.tics ? life.kcSum / life.tics : 0.0,
                    life.awayCount ? life.awaySum / life.awayCount : 0.0, life.awayCount, life.scale);
                if (lifeTarget > 0 && static_cast<int>(lives.size()) >= lifeTarget)
                {
                    break;
                }
            }
        }
        else if (p.health > 0)
        {
            respawner.dead = false;
            lives.push_back(Life{});
        }
        killsAll += std::max(0, p.kills - prev.kills);
        doorPasses += p.inDoorSector && p.sector != prev.sector && prev.sector >= 0 && p.health > 0 ? 1 : 0;
        prev = p;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    if (trace != nullptr)
    {
        std::fclose(trace);
    }

    // Summary over the lives that ended in death, and the first and the second half of them
    std::vector<double> seconds, damageRate, away;
    double awaySum = 0.0;
    int awayCount = 0;
    for (const Life& life : lives)
    {
        if (!life.died)
        {
            continue;
        }
        seconds.push_back(life.tics / 35.0);
        damageRate.push_back(life.tics ? life.damage / (life.tics / 35.0) : 0.0);
        awaySum += life.awaySum;
        awayCount += life.awayCount;
    }
    const size_t half = seconds.size() / 2;
    const std::vector<double> firstHalf(seconds.begin(), seconds.begin() + half), secondHalf(seconds.begin() + half, seconds.end());
    std::vector<double> sorted = seconds;
    std::sort(sorted.begin(), sorted.end());
    std::printf("SUMMARY learning=%d skill=%s map=%s seed=%llu lives_died=%zu mean_life_s=%.1f median_life_s=%.1f first_half_mean_s=%.1f "
                "second_half_mean_s=%.1f damage_per_s=%.3f turn_away_deg=%+.2f hits=%d mean_scale=%.5f tics=%d wall_s=%.0f resets=%u seizures=%u\n",
        learning, skill.c_str(), map.c_str(), static_cast<unsigned long long>(seed), seconds.size(), Mean(seconds), sorted.empty() ? 0.0 : sorted[sorted.size() / 2],
        Mean(firstHalf), Mean(secondHalf), Mean(damageRate), awayCount ? awaySum / awayCount : 0.0, awayCount, player->Status().learnedScale, tic, wall,
        player->Status().resets, player->Status().seizures);
    if (dump != nullptr)
    {
        std::fclose(dump);
    }
    {
        const doom::PlayerState combatEnd = game->Player(0);
        const int shots = combatEnd.shots - combatStart.shots;
        const int hits = combatEnd.hits - combatStart.hits;
        std::printf("ATTACKS shots %d hits %d (%.1f %%) damage %d kills %d\n", shots, hits, shots > 0 ? 100.0 * hits / shots : 0.0,
                    combatEnd.damageDealt - combatStart.damageDealt, killsAll);
        std::printf("DOORS opened %d passes %d\n", combatEnd.doorsOpened - combatStart.doorsOpened, doorPasses);
    }
    addBin(total, bin);
    printBin("TOTAL", total, 0.0, tic / 35.0, player->Status().learnedScale);
    std::printf("COMBAT authored_visible_target: brain_fire_intent %d, visible_target %d, actual_aligned_fire %d (outside connectome)\n",
                brainFireTics, targetVisibleTics, actualFireTics);
    if (player->Status().wallSense)
    {
        std::printf("wall sense: learned from %u pictures before a touch, chance now %.2f\n", player->Status().wallPositives, player->Status().wallChance);
    }
    game->SetTicCmdSource(nullptr);
    player->Detach(*game);
    player.reset();   // saves the memory
    if (!memoryOut.empty())
    {
        std::error_code copied;
        fs::copy_file(memoryWork, fs::u8path(memoryOut), fs::copy_options::overwrite_existing, copied);
        std::printf("memory -> %s%s\n", memoryOut.c_str(), copied ? (" FAILED: " + copied.message()).c_str() : "");
    }
    game.reset();
    myengine::jobs::Shutdown();
    return 0;
}
