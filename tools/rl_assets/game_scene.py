"""The map of the game (assets/scenes/rocket_league.json): the arena, the stadium and the sky of the pack placed as in the showcase, plus the entities
that RlGameSystem moves, found by their tags: RlArena, RlBall, RlBlue0..3 / RlOrange0..3 (body, lights, boost flame, four wheels each), RlPad0..33 with
RlPadOrb for the big ones, RlPodium (the plinth of the menu) and RlCamera. Their first positions do not matter, the game sets them every frame."""

import json

from . import scene
from .common import SCENE_DIR

CAMERA = {
    "Tag": {"name": "RlCamera"},
    "Camera": {"position": [0.0, 16.0, -48.0], "rotationDeg": [22.0, 0.0, 0.0], "fovYDeg": 95.0, "nearPlane": 0.1, "farPlane": 300.0,
               "orthographicHalfHeight": 1.0, "isPrimary": True},
    "CameraController": {"moveSpeed": 12.0, "mouseSensitivityDeg": 0.12, "rotateSpeedDeg": 80.0, "zoomSpeed": 1.0},
    "WindowBinding": {"windowId": 1},
}

TEAMS = (("Blue", "blue"), ("Orange", "orange"))
WHEEL_ORDER = ((0.37374, 0.259, -0.26755, 0.125, -1), (0.37374, 0.259, -0.26755, 0.125, 1),
               (-0.47626, 0.295, -0.25055, 0.150, -1), (-0.47626, 0.295, -0.25055, 0.150, 1))  # z, x, y, radius, side: the order the game expects


def game_items(meshes, tuning):
    showcase = scene.showcase(meshes, tuning)
    drop = ("Pad", "Orb", "Ball", "Car_", "CarLights_", "Wheel_", "Boost_")
    items = []
    for it in showcase:
        if it["name"].startswith(drop):
            continue
        if it["name"] == "arena_floor":
            it = dict(it, name="RlArena")  # the tag the game looks for to know that this is its map
        items.append(it)

    ents = scene.Entities(meshes)
    for i, (x, z, big) in enumerate(scene.pad_positions()):
        ents.add("RlPad%d" % i, "pad_big" if big else "pad_small", "pads", base=(x, 0.0, z))
        if big:
            ents.add("RlPadOrb%d" % i, "pad_orb", "pad_orb", base=(x, 1.7, z))
    ents.add("RlBall", "ball", "ball", base=(0.0, 0.98, 0.0))
    for team, material in TEAMS:
        for n in range(4):
            tag = "Rl%s%d" % (team, n)
            ents.add(tag, "car_body", "car_" + material, base=(0.0, scene.CAR_Y, 0.0))
            ents.add(tag + "Lights", "car_lights", "car_lights", base=(0.0, scene.CAR_Y, 0.0))
            ents.add(tag + "Boost", "car_boost", "car_boost", (0.0, -0.06, -0.74), 0.0, (0.0, scene.CAR_Y, 0.0))
            for w, (zz, xx, yy, rr, side) in enumerate(WHEEL_ORDER):
                ents.add("%sWheel%d" % (tag, w), "car_wheel", "car_wheel", (side * xx, yy, zz), 0.0, (0.0, scene.CAR_Y, 0.0), scale_mul=(rr / 0.15,) * 3)
    ents.add("RlPodium", "podium", "podium")
    return items + ents.items


def write_game_scene(items, path=None):
    path = path or (SCENE_DIR / "rocket_league.json")
    entities = [dict(CAMERA, id=1)]
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
