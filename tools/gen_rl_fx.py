#!/usr/bin/env python3
"""Assets of the particle system (docs/render/particles.md).

Writes
  assets/textures/fx/particles.tga          6 frames of 128 px side by side: soft dot, smoke puff, spark streak, ring, confetti, star flash
  assets/models/fx/quad.obj                 the unit quad the particles are drawn with (one per particle, instanced)
  assets/materials/fx/particles_additive.material.json, particles_alpha.material.json   the two blend modes of the particle shader
  assets/scenes/fx_showcase.json            the showcase: the arena of rl_showcase.json with every effect and every sound playing in turns
Run from anywhere:  python tools/gen_rl_fx.py          Needs numpy and Pillow.
"""

import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
FRAME = 128
FRAMES = 6
RNG = np.random.default_rng(7)


def grid():
    ys, xs = np.mgrid[0:FRAME, 0:FRAME]
    # u, v in -1..1, v up (the top of the image is +1)
    return (xs + 0.5) / FRAME * 2 - 1, 1 - (ys + 0.5) / FRAME * 2


def smooth(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)


def edge_fade(alpha):
    """The frame ends in transparency so that neighbours and mip levels do not bleed into each other."""
    u, v = grid()
    fade = smooth((1 - np.maximum(np.abs(u), np.abs(v))) / 0.12)
    return alpha * fade


def noise_field(scale, octaves=4):
    out = np.zeros((FRAME, FRAME))
    amplitude = 1.0
    total = 0.0
    for o in range(octaves):
        cells = max(2, int(scale * (2 ** o)))
        coarse = RNG.random((cells + 1, cells + 1))
        img = Image.fromarray((coarse * 255).astype(np.uint8)).resize((FRAME, FRAME), Image.BICUBIC)
        out += np.asarray(img, dtype=np.float64) / 255.0 * amplitude
        total += amplitude
        amplitude *= 0.5
    return out / total


def frame_dot():
    u, v = grid()
    r = np.sqrt(u * u + v * v)
    alpha = np.exp(-(r / 0.42) ** 2 * 2.6)
    return np.ones((FRAME, FRAME, 3)), edge_fade(alpha)


def frame_smoke():
    u, v = grid()
    r = np.sqrt(u * u + v * v)
    base = smooth((0.9 - r) / 0.7)
    alpha = base * (0.45 + 0.75 * noise_field(3))
    shade = 0.78 + 0.22 * noise_field(4)[..., None]
    return np.repeat(shade, 3, axis=2), edge_fade(np.clip(alpha, 0, 1) * 0.9)


def frame_spark():
    # the head at the top (the quad runs along the velocity, +y of the quad is the way it flies), a long tail below
    u, v = grid()
    t = (1 - v) / 2                                   # 0 at the top, 1 at the bottom
    width = 0.06 + 0.10 * t
    core = np.exp(-(u / width) ** 2)
    tail = smooth(1 - t) ** 1.5
    head = np.exp(-((t - 0.12) / 0.07) ** 2)
    alpha = core * np.clip(tail * 0.85 + head, 0, 1)
    rgb = np.ones((FRAME, FRAME, 3))
    rgb[..., 2] = 0.75 + 0.25 * (1 - t)               # a little warmer along the tail
    return rgb, edge_fade(alpha)


def frame_ring():
    u, v = grid()
    r = np.sqrt(u * u + v * v)
    ring = np.exp(-((r - 0.62) / 0.07) ** 2)
    inner = 0.12 * smooth((0.6 - r) / 0.6)
    return np.ones((FRAME, FRAME, 3)), edge_fade(np.clip(ring + inner, 0, 1))


def frame_confetti():
    u, v = grid()
    alpha = smooth((0.62 - np.abs(u)) / 0.04) * smooth((0.34 - np.abs(v)) / 0.04)
    shade = 0.82 + 0.18 * (v * 0.5 + 0.5)
    return np.repeat(shade[..., None], 3, axis=2), edge_fade(alpha)


def frame_star():
    u, v = grid()
    r = np.sqrt(u * u + v * v)
    cross = np.exp(-(u / 0.05) ** 2) * smooth(1 - np.abs(v)) + np.exp(-(v / 0.05) ** 2) * smooth(1 - np.abs(u))
    diag = 0.5 * (np.exp(-(((u + v) / 1.414) / 0.05) ** 2) + np.exp(-(((u - v) / 1.414) / 0.05) ** 2)) * smooth(1 - r)
    core = np.exp(-(r / 0.16) ** 2 * 2.0)
    return np.ones((FRAME, FRAME, 3)), edge_fade(np.clip(0.8 * cross + diag * 0.6 + core, 0, 1))


def build_atlas():
    atlas = np.zeros((FRAME, FRAME * FRAMES, 4))
    for i, make in enumerate((frame_dot, frame_smoke, frame_spark, frame_ring, frame_confetti, frame_star)):
        rgb, alpha = make()
        atlas[:, i * FRAME:(i + 1) * FRAME, :3] = rgb
        atlas[:, i * FRAME:(i + 1) * FRAME, 3] = alpha
    # where a texel is transparent its colour is white too: filtering never pulls a dark fringe in
    atlas[atlas[..., 3] < 0.004, :3] = 1.0
    return Image.fromarray((np.clip(atlas, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def write_quad(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# The unit quad of the particles (tools/gen_rl_fx.py): -0.5..0.5 in x and y, v = 1 at the top", "o fx_quad",
             "v -0.5 -0.5 0", "v 0.5 -0.5 0", "v 0.5 0.5 0", "v -0.5 0.5 0",
             "vt 0 0", "vt 1 0", "vt 1 1", "vt 0 1", "vn 0 0 -1",
             "f 1/1/1 2/2/1 3/3/1", "f 1/1/1 3/3/1 4/4/1"]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def write_material(path, blend, columns):
    material = {"shader": "../../shaders/particle.shader.json", "texture": "../../textures/fx/particles.tga", "tint": [1, 1, 1, 1],
                "params": {"gIntensity": 1.0, "gAtlasColumns": float(columns)}, "blend": blend, "doubleSided": True, "castShadow": False}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(material, indent=2) + "\n", encoding="utf-8")


def emitter_entity(entity_id, name, position, **emitter):
    return {"Tag": {"name": name}, "Transform": {"position": position, "rotationDeg": [0, 0, 0], "scale": [1, 1, 1]},
            "ParticleEmitter": emitter, "id": entity_id}


def audio_entity(entity_id, name, position, **source):
    return {"Tag": {"name": name}, "Transform": {"position": position, "rotationDeg": [0, 0, 0], "scale": [1, 1, 1]},
            "AudioSource": source, "id": entity_id}


def write_showcase_scene():
    """The arena of rl_showcase.json (without its camera) and, in front of a new camera, every effect and every sound of the project,
    one after the other: the one-shot presets are started again every few seconds, the loops run (the boost, the trail, the engine)."""
    source = ROOT / "assets/scenes/rl_showcase.json"
    scene = json.loads(source.read_text(encoding="utf-8"))
    entities = [e for e in scene["entities"] if "Camera" not in e]
    next_id = max(e["id"] for e in entities) + 1
    camera = {"Tag": {"name": "FxCamera"},
              "Camera": {"position": [0.0, 4.2, -25.0], "rotationDeg": [6.0, 0.0, 0.0], "fovYDeg": 64.0, "nearPlane": 0.1, "farPlane": 300.0,
                         "orthographicHalfHeight": 1.0, "isPrimary": True},
              "CameraController": {"moveSpeed": 14.0, "mouseSensitivityDeg": 0.12, "rotateSpeedDeg": 80.0, "zoomSpeed": 1.0},
              "WindowBinding": {"windowId": 1}, "id": 1}
    new = []

    def add(entity):
        nonlocal next_id
        entity["id"] = next_id
        next_id += 1
        new.append(entity)

    z = -10.0
    up = {"x": 0.0, "y": 1.0, "z": 0.0}
    vec = lambda v: [v[0], v[1], v[2]]
    # one-shot presets: an effect and its sound, again every few seconds
    add(emitter_entity(0, "Fx_sparks_hit", [-19.0, 1.4, z], effect="sparks_hit", retriggerInterval=1.3, direction=vec((0.4, 1.0, -0.3)), scale=1.0))
    add(audio_entity(0, "Snd_ball_hit", [-19.0, 1.4, z], event="ball_hit", retriggerInterval=1.3))
    add(emitter_entity(0, "Fx_landing", [-12.5, 0.15, z], effect="landing", retriggerInterval=1.6, direction=vec((0, 1, 0)), scale=1.0))
    add(audio_entity(0, "Snd_land", [-12.5, 0.15, z], event="land", retriggerInterval=1.6))
    add(emitter_entity(0, "Fx_pad_pickup", [-6.0, 0.1, z], effect="pad_pickup", retriggerInterval=1.2, direction=vec((0, 1, 0)), scale=1.4))
    add(audio_entity(0, "Snd_pad_big", [-6.0, 0.1, z], event="pad_big", retriggerInterval=1.2))
    add(emitter_entity(0, "Fx_demolish", [0.0, 1.0, z], effect="demolish", retriggerInterval=3.6, direction=vec((0, 1, 0)), scale=1.0))
    add(audio_entity(0, "Snd_demolish", [0.0, 1.0, z], event="demolish", retriggerInterval=3.6))
    add(emitter_entity(0, "Fx_goal_explosion", [7.5, 1.0, z], effect="goal_explosion", retriggerInterval=4.8, direction=vec((0, 1, 0)), tint=[0.25, 0.55, 1.0, 1.0]))
    add(audio_entity(0, "Snd_goal_horn", [7.5, 1.0, z], event="goal_horn", retriggerInterval=4.8, spatial=False))
    add(audio_entity(0, "Snd_crowd_roar", [7.5, 1.0, z], event="crowd_roar", retriggerInterval=4.8, spatial=False))
    # the ball on a circle with its trail, the boost and the supersonic trail at the cars, the engine and the boost of the cars, the crowd
    ball = {"Tag": {"name": "Fx_ball_trail"}, "Transform": {"position": [18.0, 2.5, z], "rotationDeg": [0, 0, 0], "scale": [1.092, 1.092, 1.092]},
            "MeshRenderer": {"meshPath": "assets/models/rl2/ball.obj", "materialPath": "assets/materials/rl2/ball.material.json", "visible": True},
            "ParticleEmitter": {"effect": "ball_trail", "playing": True, "direction": vec((0, 1, 0)), "tint": [0.55, 0.8, 1.0, 1.0], "orbitRadius": 6.0,
                                "orbitSpeedDeg": 70.0}, "id": 0}
    add(ball)
    add(emitter_entity(0, "Fx_boost", [7.91, 0.34, -1.39], effect="boost", direction=vec((0.559, 0.05, 0.829))))
    add(audio_entity(0, "Snd_boost", [7.91, 0.34, -1.39], event="boost", volume=0.8))
    add(emitter_entity(0, "Fx_supersonic", [-6.15, 0.45, -9.75], effect="supersonic", direction=vec((-0.208, 0.0, -0.978)), tint=[0.4, 0.7, 1.0, 1.0]))
    add(audio_entity(0, "Snd_engine", [-6.0, 0.4, -9.0], event="engine", pitch=1.2))
    add(audio_entity(0, "Snd_crowd", [0.0, 5.0, 0.0], event="crowd_ambience", spatial=False))
    add(audio_entity(0, "Snd_music", [0.0, 5.0, 0.0], event="menu_music", playing=False, spatial=False))
    scene["entities"] = [camera] + entities + new
    path = ROOT / "assets/scenes/fx_showcase.json"
    path.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
    return len(new)


def main():
    texture = ROOT / "assets/textures/fx/particles.tga"
    texture.parent.mkdir(parents=True, exist_ok=True)
    build_atlas().save(texture, compression="tga_rle")
    write_quad(ROOT / "assets/models/fx/quad.obj")
    write_material(ROOT / "assets/materials/fx/particles_additive.material.json", "additive", FRAMES)
    write_material(ROOT / "assets/materials/fx/particles_alpha.material.json", "alpha", FRAMES)
    print("particles.tga %dx%d (%d KB), quad.obj, 2 materials" % (FRAME * FRAMES, FRAME, texture.stat().st_size // 1024))
    print("assets/scenes/fx_showcase.json: %d entities added to the showcase" % write_showcase_scene())


if __name__ == "__main__":
    main()
