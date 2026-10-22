# Reference traces of RocketSim for the differential test

The test `rl_rocketsim_diff` (`tests/rl/RlRocketSimDiffTests.cpp`) drives the car of the game with the same scripted inputs as
[RocketSim](https://github.com/ZealanL/RocketSim) (MIT, Copyright (c) 2022 ZealanL) and compares them tick by tick. RocketSim is **not** a
dependency of the engine build and of the test: the traces it wrote are in `traces/` (one csv per scenario, every 4th tick), the test only
reads them. The scenarios and their inputs are in `tests/rl/RlDiffScenarios.h`, shared by the generator and the test.

RocketSim runs in `GameMode::THE_VOID`: no collision mesh of the game is loaded (the meshes of the arena of Rocket League are an asset of the
game and are not used anywhere in this repository). A flat floor is added to its bullet world: a static plane at z = 0 with the friction (0.6)
and the restitution (0.3) RocketSim gives to the floor of the real arena.

## Writing the traces again

Needed only when the scenarios change or RocketSim is updated. The commit the traces were written with is
`c2baacb8f4b441dd8505e63c2aeb5a1679b60b02`.

```
git clone https://github.com/ZealanL/RocketSim.git <somewhere outside of the repository>
git -C <RocketSim> checkout c2baacb8f4b441dd8505e63c2aeb5a1679b60b02
cmake -S tests/rl/rocketsim_diff -B <build dir> -DRS_DIR=<RocketSim>
cmake --build <build dir> --config Release --target rsgen
<build dir>/Release/rsgen tests/rl/rocketsim_diff/traces
```

`rsgen` converts everything to the axes of the engine (x right, y up, z forward, metres) and writes the position of the car as the centre of
its hitbox (RocketSim's own position is the centre of mass).

## The flights of G4j

`traces/*_recover.csv`, `air_roll_land_throttle.csv` (the five flights of a player who flips or rolls and lands; all six car classes), and `fuzz_*.csv`, `spin_*.csv` (Octane only: 60 random flights each with the throttle at a random time, the same with the throttle late, and the same without it, see `rldiff::kFuzzFirst` and `kSpinFirst`, the inputs are made from the index by a fixed hash). `rsgen <dir>` for the Octane writes all of them, `rsgen <dir> <class>` the five flights for a class. The test compares the five tick by tick (the three that end with a car spinning into the floor only until the first touchdown, then by the end of the flight) and the random flights by where the car ends (on its wheels, on its roof, on its side) and when it stood on its wheels for good: docs/game/rl-cars.md, "Воздух, кувырки и посадка против RocketSim (G4j)".

G4j2: `ext_*.csv` (Octane only, `rldiff::kExtFirst`): 100 random flights without the throttle and the same 100 with the throttle held from the first tick (the car starts in the middle of the field so that it does not reach a wall). The test prints, for every family of the random flights, the ends (wheels, roof, side) and the ticks from the first touchdown to the last tick off the wheels (median / 90 % / worst) of RocketSim and of ours, and checks that they stay close.
