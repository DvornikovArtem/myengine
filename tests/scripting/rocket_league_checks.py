"""Assertions run inside the engine's embedded interpreter against the real Rocket League script."""
import json

import myengine as me

WHEELS = ["RlWheelFL", "RlWheelFR", "RlWheelRL", "RlWheelRR"]
KEY_W, KEY_D, KEY_M, KEY_2 = ord("W"), ord("D"), ord("M"), ord("2")


def wrap(angle):
    return (angle + 180.0) % 360.0 - 180.0


def press(code):
    key(code, True)
    tick()
    key(code, False)


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
    team = car.team
    assert car.body.mesh.material.endswith("car_%s.material.json" % team)
    gold = "cabin_gold.material.json" if car.player else "cabin_glass.material.json"
    assert car.cabin.mesh.material.endswith(gold)


def run_checks():
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
    for frame in range(300):
        tick()
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

    # Back to the menu and a new 2v2 match: fresh cars get all their parts again.
    key(KEY_W, False)
    key(KEY_D, False)
    press(KEY_M)
    assert game.state == "menu" and not game.cars
    press(KEY_2)
    assert game.state == "countdown" and len(game.cars) == 4
    tick()
    assert len(me.world.find_all("Car_")) == 4, "the cars of the first match must be gone"
    for car in game.cars:
        check_car_parts(car)
    for _ in range(30):
        tick()
    assert all(car.e.alive for car in game.cars)
