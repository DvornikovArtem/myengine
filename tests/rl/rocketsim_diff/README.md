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
