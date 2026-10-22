"""Paths, palette, the texture/material writer and the atlas helper of the RL asset pack (assets/*/rl2)."""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "tools"))

PACK = "rl2"  # the folder of the pack inside assets/models, assets/textures and assets/materials
MODEL_DIR = ROOT / "assets" / "models" / PACK
TEX_DIR = ROOT / "assets" / "textures" / PACK
MAT_DIR = ROOT / "assets" / "materials" / PACK
SCENE_DIR = ROOT / "assets" / "scenes"

BLUE = (42, 109, 244)
BLUE_DARK = (14, 40, 110)
BLUE_GLOW = (120, 180, 255)
ORANGE = (255, 122, 26)
ORANGE_DARK = (120, 52, 8)
ORANGE_GLOW = (255, 190, 100)
WHITE = (240, 242, 245)
GOLD = (245, 197, 24)
STEEL = (150, 158, 170)
GRAPHITE = (34, 38, 46)

_canvas = None


def gen():
    """The legacy generator module (Canvas, mix, shade, rgb). Imported late so that Pillow is only needed to generate."""
    global _canvas
    if _canvas is None:
        import gen_rl_assets as g
        _canvas = g
    return _canvas


def canvas(w, h, base):
    return gen().Canvas(w, h, base)


def mix(a, b, t):
    return gen().mix(a, b, t)


def shade(c, k):
    return gen().shade(c, k)


def save_texture(name, img, tint=(1.0, 1.0, 1.0, 1.0)):
    """Writes assets/textures/rl2/<name>.tga (RLE) and the textured_lit material of the same name."""
    TEX_DIR.mkdir(parents=True, exist_ok=True)
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    path = TEX_DIR / (name + ".tga")
    img.convert("RGBA").save(path, compression="tga_rle")
    write_material(name, name, tint)
    return path.stat().st_size


def write_material(name, texture, tint=(1.0, 1.0, 1.0, 1.0)):
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    material = {"shader": "../../shaders/textured_lit.shader.json", "texture": "../../textures/%s/%s.tga" % (PACK, texture),
                "tint": [float(v) for v in tint]}
    (MAT_DIR / (name + ".material.json")).write_text(json.dumps(material, indent=2) + "\n", encoding="utf-8")


class Atlas:
    """A texture split into named cells. Cells are given in pixels (x0, y0, x1, y1; y down like the image); the UV rect
    handed to the meshes (v up, as OBJ) is inset by half a texel so a bilinear lookup does not bleed into a neighbour."""

    def __init__(self, w, h, base=(128, 128, 128)):
        self.w, self.h = w, h
        self.cv = canvas(w, h, base)
        self.cells = {}

    def cell(self, name, x0, y0, x1, y1):
        self.cells[name] = (x0, y0, x1, y1)
        return self.cv, (x0, y0, x1, y1)

    def uv(self, name, inset=0.5):
        x0, y0, x1, y1 = self.cells[name]
        return ((x0 + inset) / self.w, 1.0 - (y1 - inset) / self.h, (x1 - inset) / self.w, 1.0 - (y0 + inset) / self.h)

    def uv_point(self, name, fx=0.5, fy=0.5):
        """A point inside a cell (fractions from its top left) as (u, v) for a solid colour cell."""
        x0, y0, x1, y1 = self.cells[name]
        return ((x0 + (x1 - x0) * fx) / self.w, 1.0 - (y0 + (y1 - y0) * fy) / self.h)

    def image(self):
        return self.cv.finish()
