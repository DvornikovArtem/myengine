"""The showcase scene (assets/scenes/rl_showcase.json): the whole pack placed as the game will place it, static.

An entity is {name, mesh, material, position, rotation (deg), scale}; the same list feeds the scene JSON and the preview
renderer. A mesh is placed at its manifest origin (the point of the mesh space that is the entity origin) with the manifest scale.
"""

import json
import math

from .arena import Footprint, pad_positions
from .common import ROOT, SCENE_DIR
from .stadium import APRON, BLOCK_W, SHELL

MODEL = "assets/models/rl2/%s.obj"
MATERIAL = "assets/materials/rl2/%s.material.json"

CAR_Y = 0.40  # the centre of the hitbox above the floor when the car stands on its wheels
WHEELS = ((0.37374, 0.259, -0.26755, 0.125), (-0.47626, 0.295, -0.25055, 0.150))  # z, x, y, radius (rl_tuning.json)


class Entities:
    def __init__(self, meshes):
        self.meshes = meshes
        self.items = []

    def add(self, name, mesh, material, local_pos=(0.0, 0.0, 0.0), yaw=0.0, base=(0.0, 0.0, 0.0), scale_mul=(1.0, 1.0, 1.0), pitch=0.0):
        info = self.meshes[mesh]
        ox, oy, oz = info["origin"]
        s = info["scale"]
        # the entity position is base + R(yaw) * (local_pos + origin offset): the mesh origin is where the mesh space is centred
        a = math.radians(yaw)
        c, sn = math.cos(a), math.sin(a)
        lx, ly, lz = local_pos[0] + ox, local_pos[1] + oy, local_pos[2] + oz
        wx = base[0] + c * lx + sn * lz
        wz = base[2] - sn * lx + c * lz
        wy = base[1] + ly
        self.items.append({"name": name, "mesh": MODEL % mesh, "material": MATERIAL % material, "position": [round(wx, 4), round(wy, 4), round(wz, 4)],
                           "rotation": [pitch, round(yaw, 3), 0.0], "scale": [round(s * scale_mul[0], 4), round(s * scale_mul[1], 4), round(s * scale_mul[2], 4)]})


def car(ents, team, base, yaw, boost=False):
    """base = the centre of the hitbox in the world; the wheels are placed as RlSim defines them."""
    t = "car_" + team
    ents.add("Car_" + team, "car_body", t, (0, 0, 0), yaw, base)
    ents.add("CarLights_" + team, "car_lights", "car_lights", (0, 0, 0), yaw, base)
    for zz, xx, yy, rr in WHEELS:
        for sx in (-1, 1):
            ents.add("Wheel_%s_%s%s" % (team, "F" if zz > 0 else "R", "R" if sx > 0 else "L"), "car_wheel", "car_wheel", (sx * xx, yy, zz), yaw, base,
                     scale_mul=(rr / 0.15,) * 3)
    if boost:
        ents.add("Boost_" + team, "car_boost", "car_boost", (0.0, -0.06, -0.74), yaw, base)


def showcase(meshes, tuning):
    ents = Entities(meshes)
    foot = Footprint(tuning["arena"])
    for n in ("arena_floor", "arena_wall_blue", "arena_wall_orange", "arena_frame_blue", "arena_frame_orange", "arena_goal_blue", "arena_goal_orange"):
        ents.add(n, n, n)
    # pads and orbs
    for i, (x, z, big) in enumerate(pad_positions()):
        ents.add("Pad%02d" % i, "pad_big" if big else "pad_small", "pads", base=(x, 0.0, z))
        if big:
            ents.add("Orb%02d" % i, "pad_orb", "pads", base=(x, 1.7, z))
    ents.add("Ball", "ball", "ball", base=(0.0, 0.98, 0.0))
    car(ents, "blue", (-6.0, CAR_Y, -9.0), 12.0)
    car(ents, "orange", (7.5, CAR_Y, -2.0), -146.0, boost=True)
    # the stands
    count = 0
    rim = []
    for edge in range(8):
        a, b = foot.point(edge, 0.0, -APRON, 0.0), foot.point(edge, 1.0, -APRON, 0.0)
        ln = math.dist(a, b)
        n = max(1, int(round(ln / BLOCK_W)))
        sx = (ln / n) / BLOCK_W
        n_in = foot.n[edge]
        yaw = math.degrees(math.atan2(n_in[0], n_in[2]))
        for i in range(n):
            f = (i + 0.5) / n
            p = (a[0] + (b[0] - a[0]) * f, 0.0, a[2] + (b[2] - a[2]) * f)
            variant = "blue" if p[2] < -15 else ("orange" if p[2] > 15 else "mixed")
            # the stand mesh is built with its front at z = 0 and looks to +z: yaw turns it to the inward normal
            ents.add("Stand%02d" % count, "stand_" + variant, "stands", (0, 0, 0), yaw, p, scale_mul=(sx, 1.0, 1.0))
            count += 1
        # a floodlight tower in the middle of every edge, behind the stands
        m = foot.point(edge, 0.5, -(SHELL - 3.0), 0.0)
        ents.add("Floodlight%d" % edge, "floodlight", "floodlight", (0, 0, 0), yaw, m)
    ents.add("StadiumShell", "stadium_shell", "stadium_facade")
    ents.add("StadiumRoof", "stadium_roof", "stadium_roof")
    ents.add("StadiumGround", "stadium_ground", "stadium_ground")
    ents.add("SkyDome", "sky_dome", "sky")
    hz = tuning["arena"]["halfLength"]
    ents.add("ScoreboardBlue", "scoreboard", "scoreboard", (0, 0, 0), 0.0, (0.0, 27.0, -(hz + SHELL - 1.5)))
    ents.add("ScoreboardOrange", "scoreboard", "scoreboard", (0, 0, 0), 180.0, (0.0, 27.0, hz + SHELL - 1.5))
    return ents.items


def write_scene(items, path=None):
    path = path or (SCENE_DIR / "rl_showcase.json")
    entities = [{
        "Tag": {"name": "ShowcaseCamera"},
        "Camera": {"position": [-34.0, 15.0, -40.0], "rotationDeg": [14.0, 40.0, 0.0], "fovYDeg": 75.0, "nearPlane": 0.1, "farPlane": 300.0,
                   "orthographicHalfHeight": 1.0, "isPrimary": True},
        "CameraController": {"moveSpeed": 14.0, "mouseSensitivityDeg": 0.12, "rotateSpeedDeg": 80.0, "zoomSpeed": 1.0},
        "WindowBinding": {"windowId": 1},
        "id": 1,
    }]
    for i, it in enumerate(items):
        entities.append({
            "Tag": {"name": it["name"]},
            "Transform": {"position": it["position"], "rotationDeg": it["rotation"], "scale": it["scale"]},
            "MeshRenderer": {"meshPath": it["mesh"], "materialPath": it["material"], "visible": True},
            "id": i + 2,
        })
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"entities": entities}, indent=2) + "\n", encoding="utf-8")
    return len(entities)
