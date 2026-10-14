"""Assertions run inside the engine's embedded interpreter against the real Rocket League script."""
import json

import myengine as me

WHEELS = ["RlWheelFL", "RlWheelFR", "RlWheelRL", "RlWheelRR"]
KEY_W, KEY_S, KEY_D, KEY_M, KEY_1, KEY_2, KEY_SPACE = ord("W"), ord("S"), ord("D"), ord("M"), ord("1"), ord("2"), ord(" ")
KEY_SHIFT, KEY_CTRL = 0x10, 0x11
DETAILS = {  # part of the prefab -> material file
    "RlCarBumperF": "car_trim", "RlCarBumperR": "car_trim", "RlCarHeadlightL": "car_headlight", "RlCarHeadlightR": "car_headlight",
    "RlCarTaillightL": "car_taillight", "RlCarTaillightR": "car_taillight", "RlCarSpoilerWing": "car_trim",
    "RlCarSpoilerStrutL": "car_trim", "RlCarSpoilerStrutR": "car_trim", "RlCarNozzle": "car_trim", "RlCarFlame": "fx_boost",
}


def wrap(angle):
    return (angle + 180.0) % 360.0 - 180.0


def press(code):
    key(code, True)
    tick()
    key(code, False)


def step(game):
    """One frame, then the checks that hold at every moment: the suspension limits and the untouched root."""
    tick()
    for car in game.cars:
        pitch, _, roll = car.chassis.transform.rotation.x, 0, car.chassis.transform.rotation.z
        heave = car.chassis.transform.position.y
        eps = 1e-3
        assert abs(pitch) <= rocket_league.PITCH_MAX + eps and abs(roll) <= rocket_league.ROLL_MAX + eps, (pitch, roll)
        assert rocket_league.HEAVE_MIN - eps <= heave <= rocket_league.HEAVE_MAX + eps, heave
        assert car.chassis.transform.rotation.y == 0.0 and car.chassis.transform.position.x == 0.0 and car.chassis.transform.position.z == 0.0
        # the suspension moves the chassis only: the root keeps the orientation that drive() gives it
        rotation = car.e.transform.rotation
        assert abs(rotation.x - car.pitch) < 1e-2 and abs(rotation.y - car.yaw) < 1e-2 and abs(rotation.z - car.roll) < 1e-2


def drop(car, roll=0.0, pitch=0.0, height=0.9):
    """Put a car in the air above the floor in the given attitude (the physics root is set from the script state)."""
    car.place(0.0, -10.0)
    car.was_grounded = False
    car.jumps = 1
    car.pitch, car.roll = pitch, roll
    car.e.transform.position = me.Vec3(0.0, height, -10.0)


def fall_holding(game, car, roll):
    """Let the car fall and keep its roll the same until the touchdown, as a driver who holds the air roll would."""
    for _ in range(90):
        car.roll = roll
        step(game)
        if car.grounded:
            return
    assert False, "the car did not land"


def flat_speed(car):
    v = car.e.rigidbody.velocity
    return (v.x ** 2 + v.z ** 2) ** 0.5


def attitude(car):
    return wrap(car.pitch), wrap(car.roll)


def is_attitude(car, pitch, roll, tolerance=1.0):
    p, r = attitude(car)
    return abs(p - pitch) < tolerance and abs(abs(r) - abs(roll)) < tolerance and (roll in (0.0, 180.0) or r * roll > 0)


def check_car_parts(car):
    """The parts of the prefab are reached through the hierarchy, and the root has no mesh of its own."""
    root = car.e
    assert sorted(w.name for w in root.children if w.name.startswith("RlWheel")) == sorted(WHEELS)
    assert [root.find_child(name) for name in WHEELS] == car.wheels
    assert all(w.parent == root for w in car.wheels), "the wheels hang on the root, not on the chassis"
    assert car.chassis == root.find_child("RlCarChassis") and car.chassis.parent == root
    assert car.body.parent == car.chassis and car.cabin.parent == car.chassis
    try:
        root.mesh
        assert False, "The root of the car must not have a mesh"
    except AttributeError:
        pass
    assert root.transform.scale == me.Vec3(1, 1, 1)
    assert root.collider.half_extents == me.Vec3(0.7, 0.3, 1.2)
    # lights, bumpers, spoiler, nozzle and the flame hang on the chassis, so they move with the body
    for name, material in DETAILS.items():
        part = car.chassis.find_child(name)
        assert part is not None and part.parent == car.chassis, name
        assert part.mesh.material.endswith("%s.material.json" % material), name
        # nothing hangs below the lower face of the collider (y = -0.3), even with the body at the bottom of its travel
        bottom = part.transform.position.y - part.transform.scale.y / 2 + rocket_league.HEAVE_MIN
        assert bottom >= -0.3, (name, bottom)
    assert car.flame == car.chassis.find_child("RlCarFlame") and not car.flame.mesh.visible, "the flame is hidden without boost"
    assert [l.name for l in car.taillights] == ["RlCarTaillightL", "RlCarTaillightR"]
    team = car.team
    assert car.body.mesh.material.endswith("car_%s.material.json" % team)
    gold = "cabin_gold.material.json" if car.player else "cabin_glass.material.json"
    assert car.cabin.mesh.material.endswith(gold)


def run_checks():
    global rocket_league
    import rocket_league

    scene = json.loads(base_scene)
    for entity in scene["entities"]:
        if entity["Tag"]["name"] == "RocketLeague":
            entity["Script"]["scripts"][0]["props"]["start_mode"] = 1
    load_scene(json.dumps(scene))
    tick()
    game = me.world.find("RocketLeague").get_script(rocket_league.RocketLeague)
    assert game.state == "countdown" and len(game.cars) == 2
    player = next(c for c in game.cars if c.player)
    for car in game.cars:
        check_car_parts(car)

    # 1v1 for about 300 frames: the player holds gas and steers right, the bot plays by itself.
    key(KEY_W, True)
    key(KEY_D, True)
    spins = {car.index + 10 * (car.team == "orange"): [] for car in game.cars}
    top_speed = {name: 0.0 for name in spins}
    front_yaw, rear_yaw = [], []
    forward_steps = []
    previous = None
    pitches, rolls = [], []
    for frame in range(300):
        step(game)
        for car in game.cars:
            name = car.index + 10 * (car.team == "orange")
            spin = car.wheels[0].transform.rotation.x
            spins[name].append(spin)
            speed = car.e.rigidbody.velocity.length()
            top_speed[name] = max(top_speed[name], speed)
            for wheel in car.wheels:
                assert 0.0 <= wheel.transform.rotation.x < 360.0, "the wheel angle must stay wrapped"
            assert abs(car.wheels[0].transform.rotation.y) <= rocket_league.MAX_STEER + 0.001
            assert abs(car.wheels[1].transform.rotation.y) <= rocket_league.MAX_STEER + 0.001
            assert car.wheels[2].transform.rotation.y == 0.0 and car.wheels[3].transform.rotation.y == 0.0
            if car is player:
                pitches.append(car.chassis.transform.rotation.x)
                rolls.append(car.chassis.transform.rotation.z)
                front_yaw.append(car.wheels[0].transform.rotation.y)
                rear_yaw.append(car.wheels[2].transform.rotation.y)
                forward = car.e.rigidbody.velocity.dot(car.forward())
                # slow enough that one frame turns a wheel by less than half a turn: the direction can be read
                if previous is not None and 0.5 < forward < 6.0 and car.grounded:
                    forward_steps.append(wrap(spin - previous))
                previous = spin
    assert game.state in ("play", "goal"), game.state

    assert len(forward_steps) >= 3, "the player never drove slowly forward"
    assert all(step > 0.0 for step in forward_steps), "driving forward must turn the wheels forward (growing pitch)"
    assert top_speed[player.index] > 5.0, "the player car did not drive"
    assert len(set(spins[player.index])) > 20, "the wheels of the driving car did not turn"
    for name, angles in spins.items():
        if top_speed[name] > 2.0:
            assert len(set(angles)) > 10, "a moving car must roll its wheels"
    assert max(front_yaw) > 15.0, "the front wheels did not follow the steering (D = to the right = positive yaw)"
    assert max(abs(y) for y in rear_yaw) == 0.0, "the rear wheels must not steer"

    # Suspension: the gas lifts the nose (pitch < 0), a right turn at speed leans the body to the left (roll > 0).
    assert min(pitches) < -1.0, "the body must pitch nose up when the car accelerates: %s" % min(pitches)
    assert max(rolls) > 1.0, "the body must roll to the outside of a right turn: %s" % max(rolls)
    key(KEY_W, False)
    key(KEY_D, False)

    # A new 1v1 from the menu: wait for the kickoff, jump on the spot and watch the chassis land.
    press(KEY_M)
    assert game.state == "menu" and not game.cars
    press(KEY_1)
    assert game.state == "countdown" and len(game.cars) == 2
    for _ in range(400):
        step(game)
        if game.state == "play":
            break
    assert game.state == "play"
    player = next(c for c in game.cars if c.player)
    # Take the bot out of the game for this part, so that nothing bumps or demolishes the jumping car.
    bot = next(c for c in game.cars if not c.player)
    game._demolish(bot, player)
    bot.dead = 1.0e6
    for _ in range(30):
        step(game)
    assert abs(player.chassis.transform.position.y) < 1e-3, "the chassis must rest at the start"
    key(KEY_SPACE, True)
    step(game)
    key(KEY_SPACE, False)
    heave, grounded = [], []
    for _ in range(300):
        step(game)
        heave.append(player.chassis.transform.position.y)
        grounded.append(player.grounded)
    assert not all(grounded) and any(grounded[i] and not grounded[i - 1] for i in range(1, len(grounded))), "the car must jump and land"
    landing = next(i for i in range(1, len(grounded)) if grounded[i] and not grounded[i - 1])
    assert all(abs(h) < 0.005 for h in heave[:landing]), "the chassis stays level in the air"
    after = heave[landing:landing + 30]
    assert min(after) < -0.01, "the landing must push the chassis down: %s" % min(after)
    assert after.index(min(after)) < 12 and after[0] < 0.01, "the first move after the touchdown is downwards"
    assert max(after) <= rocket_league.HEAVE_MAX + 1e-3
    assert all(abs(h) < 0.005 for h in heave[-30:]), "the chassis returns to rest: %s" % heave[-1]

    # Details of the car, the bot is still out of the game: stop lights, the flame of the boost, sparks of a slide.
    def lights():
        return [l.mesh.material.rsplit("/", 1)[-1] for l in player.taillights]

    def forward_speed():
        return player.e.rigidbody.velocity.dot(player.forward())

    tail, brake = ["car_taillight.material.json"] * 2, ["car_brakelight.material.json"] * 2
    assert lights() == tail and not player.flame.mesh.visible
    key(KEY_W, True)
    for _ in range(80):
        step(game)
    assert forward_speed() > 8.0 and lights() == tail and not player.flame.mesh.visible, "the gas alone shows no stop light and no flame"
    key(KEY_SHIFT, True)
    lengths = set()
    for _ in range(20):
        step(game)
        assert player.flame.mesh.visible, "the boost lights the flame"
        flame = player.flame.transform
        lengths.add(round(flame.scale.z, 4))
        assert abs(flame.position.z + flame.scale.z - rocket_league.FLAME_Z) < 1e-4, "the flame grows back from the nozzle"
    assert len(lengths) > 5, "the flame must flicker"
    key(KEY_SHIFT, False)
    step(game)
    assert not player.flame.mesh.visible, "no boost, no flame"
    key(KEY_W, False)
    key(KEY_S, True)
    for _ in range(5):
        step(game)
    assert forward_speed() > rocket_league.LIGHT_SPEED and lights() == brake, "braking lights the stop lights"
    for _ in range(90):
        step(game)
    assert forward_speed() < -rocket_league.LIGHT_SPEED and lights() == brake, "reversing keeps the stop lights on"
    key(KEY_S, False)
    key(KEY_W, True)
    for _ in range(60):
        step(game)
    assert forward_speed() > rocket_league.LIGHT_SPEED and lights() == tail, "driving forward switches the tail lights back"
    sparks = lambda: {f[0].id for f in game.fx if f[0].name == "RlDeco"}
    for _ in range(25):
        step(game)  # speed up
    before = sparks()
    for _ in range(15):  # fast, but no slide: no sparks
        step(game)
        velocity = player.e.rigidbody.velocity
        assert (velocity.x ** 2 + velocity.z ** 2) ** 0.5 > rocket_league.SPARK_SPEED, "the car must be fast, or this proves nothing"
        assert not (sparks() - before), "no slide, no sparks"
    before, thrown, most = sparks(), set(), 0
    key(KEY_CTRL, True)
    key(KEY_D, True)
    for _ in range(40):
        step(game)
        thrown |= sparks() - before
        most = max(most, len(game.fx))
    assert len(thrown) >= 6, "a slide at speed must throw sparks: %d" % len(thrown)
    assert most <= 261, "the limit of the effects was exceeded: %d" % most
    key(KEY_CTRL, False)
    key(KEY_D, False)
    key(KEY_W, False)

    # A bad landing. The player car is dropped from a small height; its roll is held until the touchdown.
    # Upside down: it ends on its roof and stays there, the controls do nothing.
    drop(player, 180.0)
    fall_holding(game, player, 180.0)
    for _ in range(40):
        step(game)
    assert player.tilt == "lying" and is_attitude(player, 0.0, 180.0), (player.tilt, attitude(player))
    assert hud_line("6flip") == "SPACE - flip back"
    boost_before = player.boost
    key(KEY_W, True)
    key(KEY_D, True)
    key(KEY_SHIFT, True)
    for _ in range(90):
        step(game)
        assert player.tilt == "lying" and is_attitude(player, 0.0, 180.0), "a car on its roof must not level out by itself"
        assert flat_speed(player) < 0.5, "a car on its roof must not drive"
        assert not player.flame.mesh.visible and player.boost == boost_before, "a car on its roof must not boost"
    key(KEY_W, False)
    key(KEY_D, False)
    key(KEY_SHIFT, False)
    # Space: a hop and a turn onto the wheels; then it drives again.
    key(KEY_SPACE, True)
    step(game)
    key(KEY_SPACE, False)
    assert player.tilt == "righting" and not player.grounded
    for _ in range(150):
        step(game)
    assert player.tilt == "wheels" and player.grounded and abs(wrap(player.pitch)) < 5.0 and abs(wrap(player.roll)) < 5.0, attitude(player)
    assert hud_line("6flip") == "", "the hint goes away after the flip back"
    key(KEY_W, True)
    for _ in range(60):
        step(game)
    assert flat_speed(player) > 5.0, "after the flip back the car drives again"
    key(KEY_W, False)

    # Landing on a side: a stable attitude (a side), on either side, not something between.
    for roll in (80.0, -80.0):
        drop(player, roll)
        fall_holding(game, player, roll)
        for _ in range(40):
            step(game)
        assert player.tilt == "lying" and is_attitude(player, 0.0, 90.0 * (1 if roll > 0 else -1)), (roll, player.tilt, attitude(player))
    # A small tilt is levelled out on the wheels as before.
    drop(player, 30.0)
    fall_holding(game, player, 30.0)
    for _ in range(30):
        step(game)
    assert player.tilt == "wheels" and is_attitude(player, 0.0, 0.0), (player.tilt, attitude(player))
    # A dodge that the ground interrupts does not turn the car onto its wheels any more.
    drop(player, 0.0, height=1.3)
    player.jumps, player.flip, player.flip_dir = 2, 0.5, (0.0, 1.0)
    for _ in range(60):
        step(game)
        if player.grounded:
            break
    assert player.grounded and player.flip == 0.0
    for _ in range(40):
        step(game)
    assert player.tilt == "lying" and abs(wrap(player.roll)) > 80.0 and wrap(player.pitch) == 0.0, (player.tilt, attitude(player))

    # Back to the menu and a new 2v2 match: fresh cars get all their parts again.
    press(KEY_M)
    assert game.state == "menu" and not game.cars
    press(KEY_2)
    assert game.state == "countdown" and len(game.cars) == 4
    tick()
    assert len(me.world.find_all("Car_")) == 4, "the cars of the first match must be gone"
    for car in game.cars:
        check_car_parts(car)
    for _ in range(30):
        step(game)
    assert all(car.e.alive for car in game.cars)

    # A bot on its roof flips back by itself, within about a second.
    for _ in range(400):
        step(game)
        if game.state == "play":
            break
    assert game.state == "play"
    bot = next(c for c in game.cars if not c.player)
    drop(bot, 180.0)
    fall_holding(game, bot, 180.0)
    lying = 0
    while bot.tilt != "lying":
        step(game)
        lying += 1
        assert lying < 60, "the bot did not come to rest on its roof"
    for frames in range(1, 100):
        step(game)
        if bot.tilt == "wheels":
            break
    assert bot.tilt == "wheels" and frames < 90 and abs(wrap(bot.roll)) < 5.0, (frames, bot.tilt, attitude(bot))
