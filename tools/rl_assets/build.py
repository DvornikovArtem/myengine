"""Orchestrates the RL asset pack: runs the part builders with a context that writes OBJ, TGA, materials and the manifest."""

import json
import time

from . import common
from .mesh import write_obj


class Ctx:
    def __init__(self):
        self.meshes = {}
        self.textures = {}
        self.extra_materials = {}

    def mesh(self, name, mesh, origin=(0.0, 0.0, 0.0), unit_scale=None, part_of=None, note=""):
        info = write_obj(common.MODEL_DIR / (name + ".obj"), mesh, origin, unit_scale, comment=note)
        info["orientation"] = round(mesh.orientation_report(), 4)
        info["part_of"] = part_of
        info["note"] = note
        self.meshes[name] = info
        return info

    def texture(self, name, img):
        size = common.save_texture(name, img)
        self.textures[name] = {"size": [img.width, img.height], "bytes": size}
        return size

    def material(self, name, texture, tint=(1, 1, 1, 1)):
        common.write_material(name, texture, tint)
        self.extra_materials[name] = {"texture": texture, "tint": list(tint)}


def run(parts=None):
    from . import arena, ball, car, podium, stadium
    ctx = Ctx()
    t0 = time.time()
    modules = {"car": car, "ball": ball, "arena": arena, "stadium": stadium, "podium": podium}
    for name, mod in modules.items():
        if parts and name not in parts:
            continue
        mod.build(ctx)
        print("  %-8s done (%.1f s)" % (name, time.time() - t0))
    return ctx
