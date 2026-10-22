"""The data and the previews of the cars of the garage: assets/data/cars/<id>.json and assets/ui/cars/<id>_<team>.png.

The json is what the game reads (engine/include/myengine/game/RlCars.h): the name and the class of the car, the hitbox of Rocket League it follows (the
numbers of the car section of rl_tuning.json for that hitbox, from the car configs of RocketSim), the meshes and the paint of the two teams, the
profile previews of the carousel and `physics`: the numbers the car of the match is driven with. Pulse has the values of the car section of
rl_tuning.json, the others differ from it by the factors of CHARACTER below (G4i-physics, docs/game/rl-cars.md); the match reads them (game/rl/RlCars.h)
and the garage draws the bars of the stats from them.
"""

import json
import math

from . import cars
from .common import MAT_DIR, MODEL_DIR, ROOT, TEX_DIR

DATA_DIR = ROOT / "assets" / "data" / "cars"
UI_DIR = ROOT / "assets" / "ui" / "cars"
MODEL = "assets/models/rl2/%s.obj"
MATERIAL = "assets/materials/rl2/%s.material.json"

# the numbers of the car section of rl_tuning.json that make one car different from another (docs/game/rl-cars.md); G4i-physics changes them per car
PHYSICS_KEYS = ["mass", "inertiaMultiplier", "friction", "maxSpeed", "maxAngularSpeed", "supersonicSpeed", "supersonicMaintainSpeed",
                "throttleAccel", "driveSpeed", "driveFactor", "brakeAccel", "steerSpeed", "steerAngle", "powerslideSteerSpeed",
                "powerslideSteerAngle", "latFrictionSlip", "latFriction", "handbrakeLatFriction", "jumpImpulse", "jumpHoldAccel",
                "doubleJumpImpulse", "dodgeImpulse", "airPitchTorque", "airYawTorque", "airRollTorque", "boostStart", "boostMax",
                "boostUsePerSecond", "boostAccelGround", "boostAccelAir"]


# What makes one car feel different from another, as factors on the numbers of Pulse (10 to 20 percent, the balance of the six has to hold: what a car gains
# in one thing it pays in another). The hitbox and the wheels are not in here: they are the ones of the class of Rocket League (cars.hitbox_numbers).
#   mass          the mass of the body: the hits and the bumps (a heavier car pushes the ball and the others around), not the drive
#   inertia       the inertia of the box: how it turns in a collision
#   acceleration  the drive force on the ground (throttleAccel)
#   topSpeed      the speed limit and the supersonic speed with it (maxSpeed, supersonicSpeed, supersonicMaintainSpeed)
#   boost         the push of the boost (boostAccelGround, boostAccelAir)
#   steering      the angle of the front wheels at every speed (steerAngle, powerslideSteerAngle)
#   air           the angular acceleration of the air control (airPitchTorque, airYawTorque, airRollTorque)
CHARACTER = {
    #            mass  inertia accel  top   boost steer air
    "pulse":   (1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00),   # the all-rounder: the numbers of the tuning
    "vector":  (1.08, 1.05, 1.00, 1.00, 0.98, 0.94, 0.97),   # long and low: hits hard, turns a little wide
    "slate":   (1.12, 1.08, 0.96, 0.98, 1.00, 0.92, 0.95),   # flat and wide: a shovel, the heavy body wants a longer run-up
    "dart":    (0.92, 0.95, 1.08, 1.04, 1.03, 0.97, 1.05),   # long and quick: light, the best in the air, easy to push around
    "fusion":  (1.04, 1.03, 1.00, 1.01, 1.00, 1.02, 1.00),   # between Pulse and Vector: a little of everything
    "bulwark": (1.20, 1.15, 0.90, 0.97, 0.97, 0.88, 0.93),   # heavy and tall: wins the 50-50s, turns like a truck
}


def _round(x):
    return float("%.6g" % x)


def names(d):
    prefix = "car" if d.key == "pulse" else "car_" + d.key
    return {"body": prefix + "_body", "lights": prefix + "_lights", "blue": prefix + "_blue", "orange": prefix + "_orange"}


def physics_of_pulse():
    car = json.loads((ROOT / "assets" / "data" / "rl_tuning.json").read_text(encoding="utf-8"))["car"]
    return {k: car[k] for k in PHYSICS_KEYS}


def physics_of(d):
    """The `physics` section of the car: the numbers of Pulse with the factors of the car on top."""
    base = physics_of_pulse()
    mass, inertia, accel, top, boost, steer, air = CHARACTER[d.key]
    p = dict(base)
    p["mass"] = _round(base["mass"] * mass)
    p["inertiaMultiplier"] = _round(base["inertiaMultiplier"] * inertia)
    p["throttleAccel"] = _round(base["throttleAccel"] * accel)
    for key in ("maxSpeed", "supersonicSpeed", "supersonicMaintainSpeed"):
        p[key] = _round(base[key] * top)
    for key in ("boostAccelGround", "boostAccelAir"):
        p[key] = _round(base[key] * boost)
    for key in ("steerAngle", "powerslideSteerAngle"):
        p[key] = [_round(v * steer) for v in base[key]]
    for key in ("airPitchTorque", "airYawTorque", "airRollTorque"):
        p[key] = _round(base[key] * air)
    return p


def car_json(d, meshes):
    n = names(d)
    hb = cars.hitbox_numbers(d.hitbox)
    rs = cars.ROCKETSIM[d.hitbox]
    return {
        "id": d.key, "name": d.name, "class": d.klass, "hitboxClass": d.hitbox_label, "description": d.description,
        "order": [x.key for x in cars.DESIGNS].index(d.key),
        "hitbox": dict(hb, rocketSim={"size": list(rs["size"]), "offset": list(rs["offset"])}),
        "mesh": {
            "body": MODEL % n["body"], "bodyScale": meshes[n["body"]]["scale"],
            "lights": MODEL % n["lights"], "lightsScale": meshes[n["lights"]]["scale"],
            "materials": {"blue": MATERIAL % n["blue"], "orange": MATERIAL % n["orange"]},
            "boostNozzle": list(cars.nozzle_position(d)),
        },
        "preview": {"blue": "assets/ui/cars/%s_blue.png" % d.key, "orange": "assets/ui/cars/%s_orange.png" % d.key},
        "physics": physics_of(d),
    }


def write_data(meshes):
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    for d in cars.DESIGNS:
        text = json.dumps(car_json(d, meshes), indent=2) + "\n"
        text = _compact_numbers(text)
        (DATA_DIR / (d.key + ".json")).write_text(text, encoding="utf-8", newline="\n")
    return len(cars.DESIGNS)


def _compact_numbers(text):
    """Number arrays on one line."""
    import re
    return re.sub(r"\[\s*(-?[0-9.eE+-]+(?:,\s*-?[0-9.eE+-]+)*)\s*\]", lambda m: "[" + re.sub(r"\s+", " ", m.group(1)) + "]", text)


# --------------------------------------------------------------------------- previews
def render_profile(d, meshes, team, size=(512, 256), scale_up=3):
    """The car in profile (the nose to the right) on a transparent background, the same frame for every car so that the sizes compare."""
    import numpy as np
    from PIL import Image
    from . import raster as R
    n = names(d)
    front, rear = d.wheels()
    items = []

    def add(mesh, material, pos, scale_mul=1.0):
        info = meshes[mesh]
        items.append((ROOT / (MODEL % mesh), ROOT / (MATERIAL % material), pos, info["scale"] * scale_mul))

    add(n["body"], n[team], (0.0, 0.0, 0.0))
    add(n["lights"], "car_lights", (0.0, 0.0, 0.0))
    for w in (front, rear):
        for sx in (-1, 1):
            add("car_wheel", "car_wheel", (sx * w["x"], w["y"], w["z"]), w["r"] / 0.15)
    w, h = size[0] * scale_up, size[1] * scale_up
    frames = []
    for sky in ((0, 0, 0), (255, 255, 255)):
        s = R.Scene(w, h, sky=(sky, sky))
        for mesh, mat, pos, sc in items:
            s.add(mesh, mat, pos, (0, 0, 0), (sc, sc, sc), cull_sphere=False)
        # a camera on the right of the car (+X) looks at it; with +Z forward the nose is on the right of the picture seen from -X, so the eye is at -X
        cam = R.Camera.look_at((-14.0, -0.04, 0.0), (0.0, -0.04, 0.0), 2 * math.degrees(math.atan(0.475 / 14.0)), far=60.0)  # a window of 1.9 x 0.95 m
        frames.append(np.asarray(s.render(cam), dtype=np.float64) / 255.0)
    black, white = frames
    alpha = np.clip(1.0 - (white - black).mean(axis=2), 0.0, 1.0)
    color = np.where(alpha[..., None] > 1e-3, black / np.maximum(alpha[..., None], 1e-3), 0.0)
    rgba = np.dstack([np.clip(color, 0, 1), alpha])
    img = Image.fromarray((rgba * 255 + 0.5).astype(np.uint8), "RGBA")
    # premultiplied downsampling: no dark fringe at the edge
    arr = np.asarray(img, dtype=np.float64) / 255.0
    pre = arr[..., :3] * arr[..., 3:4]
    small = Image.fromarray(np.dstack([pre, arr[..., 3:4]]).__mul__(255).astype(np.uint8), "RGBA").resize(size, Image.LANCZOS)
    s_arr = np.asarray(small, dtype=np.float64) / 255.0
    out = np.where(s_arr[..., 3:4] > 1e-3, s_arr[..., :3] / np.maximum(s_arr[..., 3:4], 1e-3), 0.0)
    result = Image.fromarray((np.dstack([np.clip(out, 0, 1), s_arr[..., 3:4]]) * 255 + 0.5).astype(np.uint8), "RGBA")
    return result.transpose(Image.FLIP_LEFT_RIGHT)  # the camera on the left side sees the nose on the left; the cars are symmetric: the carousel wants it on the right


def write_previews(meshes):
    UI_DIR.mkdir(parents=True, exist_ok=True)
    count = 0
    for d in cars.DESIGNS:
        for team in ("blue", "orange"):
            render_profile(d, meshes, team).save(UI_DIR / ("%s_%s.png" % (d.key, team)), optimize=True)
            count += 1
    return count
