"""What each material of the pack asks of the renderer (the lit shader of G7b: assets/shaders/lit.hlsl).

shader     "lit" (anisotropic, wrap) or "lit_clamp" (anisotropic, clamp: the sky)
params     the variables of the shader by name: gSpecular, gGloss, gFresnel (rim), gEmissiveStrength (the albedo, again, as light:
           above 1 it blooms), gEmissiveColor, gAlphaCutoff (alpha test), gUnlit (1: no lighting at all)
blend      "opaque" (default), "alpha", "additive"
doubleSided / castShadow   the state of the pass (castShadow false: the arena shell and the stands do not shadow the field)
"""

DEFAULT = {"shader": "lit", "params": {"gSpecular": 0.15, "gGloss": 16, "gFresnel": 0.0}, "castShadow": False}

OPTIONS = {
    # the car: clear coat paint, dark tyres, lights that glow
    "car_blue": {"params": {"gSpecular": 1.1, "gGloss": 90, "gFresnel": 0.55}, "castShadow": True},
    "car_orange": {"params": {"gSpecular": 1.1, "gGloss": 90, "gFresnel": 0.55}, "castShadow": True},
    "car_lights": {"params": {"gSpecular": 0.5, "gGloss": 40, "gEmissiveStrength": 1.6}, "castShadow": True},
    "car_wheel": {"params": {"gSpecular": 0.25, "gGloss": 24, "gFresnel": 0.15}, "castShadow": True},
    "car_boost": {"params": {"gUnlit": 1, "gEmissiveStrength": 1.4}, "blend": "additive", "doubleSided": True, "castShadow": False},
    "ball": {"params": {"gSpecular": 1.4, "gGloss": 80, "gFresnel": 0.7, "gEmissiveStrength": 0.22}, "castShadow": True},
    # the arena: the turf receives shadows, the walls glow along their team lines
    "arena_floor": {"params": {"gSpecular": 0.18, "gGloss": 28, "gFresnel": 0.0}},
    "arena_wall_blue": {"params": {"gSpecular": 0.35, "gGloss": 28, "gFresnel": 0.3, "gEmissiveStrength": 0.45}},
    "arena_wall_orange": {"params": {"gSpecular": 0.35, "gGloss": 28, "gFresnel": 0.3, "gEmissiveStrength": 0.45}},
    "arena_frame_blue": {"params": {"gSpecular": 0.7, "gGloss": 48, "gFresnel": 0.3, "gEmissiveStrength": 0.35}},
    "arena_frame_orange": {"params": {"gSpecular": 0.7, "gGloss": 48, "gFresnel": 0.3, "gEmissiveStrength": 0.35}},
    "arena_glass_blue": {"params": {"gSpecular": 1.2, "gGloss": 110, "gFresnel": 1.1}, "blend": "alpha", "doubleSided": True},
    "arena_glass_orange": {"params": {"gSpecular": 1.2, "gGloss": 110, "gFresnel": 1.1}, "blend": "alpha", "doubleSided": True},
    "arena_glass_roof": {"params": {"gSpecular": 1.0, "gGloss": 110, "gFresnel": 0.8}, "blend": "alpha", "doubleSided": True},
    "arena_goal_blue": {"params": {"gSpecular": 0.2, "gGloss": 16, "gEmissiveStrength": 1.0, "gAlphaCutoff": 0.35, "gUnlit": 0.3},
                        "doubleSided": True},
    "arena_goal_orange": {"params": {"gSpecular": 0.2, "gGloss": 16, "gEmissiveStrength": 1.0, "gAlphaCutoff": 0.35, "gUnlit": 0.3},
                          "doubleSided": True},
    "pads": {"params": {"gSpecular": 0.6, "gGloss": 40, "gEmissiveStrength": 1.3}},
    "pad_orb": {"params": {"gUnlit": 1, "gEmissiveStrength": 1.1}, "blend": "additive", "doubleSided": True},
    # the stadium: nothing of it casts a shadow; the facade windows, the roof lamps and the board glow
    "stands": {"params": {"gSpecular": 0.05, "gGloss": 8, "gFresnel": 0.0, "gEmissiveStrength": 0.12}},
    "stadium_facade": {"params": {"gSpecular": 0.0, "gGloss": 8, "gEmissiveStrength": 0.6}},
    "stadium_roof": {"params": {"gSpecular": 0.0, "gGloss": 8, "gEmissiveStrength": 1.1, "gUnlit": 0.5}},
    "stadium_ground": {"params": {"gSpecular": 0.1, "gGloss": 20, "gFresnel": 0.0}},
    "sky": {"shader": "lit_clamp", "params": {"gUnlit": 1}},
    "floodlight": {"params": {"gSpecular": 0.2, "gGloss": 20, "gEmissiveStrength": 1.4}},
    "scoreboard": {"params": {"gSpecular": 0.1, "gGloss": 12, "gEmissiveStrength": 0.9}},
}


def options(name):
    merged = {"shader": DEFAULT["shader"], "params": dict(DEFAULT["params"]), "castShadow": DEFAULT["castShadow"]}
    entry = OPTIONS.get(name, {})
    merged["shader"] = entry.get("shader", merged["shader"])
    merged["params"].update(entry.get("params", {}))
    merged["castShadow"] = entry.get("castShadow", merged["castShadow"])
    merged["blend"] = entry.get("blend", "opaque")
    merged["doubleSided"] = entry.get("doubleSided", False)
    return merged
