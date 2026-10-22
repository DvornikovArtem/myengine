"""The ball: r = 0.9275 m, a smooth low poly sphere with a football like pattern of 12 dark pentagons and 20 light
hexagons (the 32 faces of a truncated icosahedron, as a spherical Voronoi) and glowing seams. Origin = the centre,
unit scale = the radius: entity scale = mesh scale (0.9275 / 0.85 ~ 1.09)."""

import math

import numpy as np
from PIL import Image

from .mesh import uv_sphere

RADIUS = 0.9275


def _centers():
    phi = (1 + 5 ** 0.5) / 2
    ico = [(0, 1, phi), (0, -1, phi), (0, 1, -phi), (0, -1, -phi), (1, phi, 0), (-1, phi, 0), (1, -phi, 0), (-1, -phi, 0),
           (phi, 0, 1), (-phi, 0, 1), (phi, 0, -1), (-phi, 0, -1)]
    verts = [np.array(v, dtype=np.float64) / np.linalg.norm(v) for v in ico]
    faces = []
    for a in range(12):
        for b in range(a + 1, 12):
            for c in range(b + 1, 12):
                # a face of the icosahedron: three vertices at the edge length (2 / sqrt(1 + phi^2))
                e = 2 / (1 + phi * phi) ** 0.5 * 1.0001
                if all(np.linalg.norm(verts[i] - verts[j]) < e for i, j in ((a, b), (b, c), (a, c))):
                    f = verts[a] + verts[b] + verts[c]
                    faces.append(f / np.linalg.norm(f))
    return verts, faces


def texture(w=1024, h=512, accent=(110, 200, 255)):
    pent, hexa = _centers()
    # a pentagon is smaller than a hexagon on the real ball: bias the distance of the pentagon cells
    cells = [(c, 0) for c in pent] + [(c, 1) for c in hexa]
    lon = (np.arange(w) + 0.5) / w * 2 * math.pi
    lat = (np.arange(h) + 0.5) / h * math.pi  # 0 = the top
    LON, LAT = np.meshgrid(lon, lat)
    d = np.stack([np.sin(LAT) * np.sin(LON), np.cos(LAT), np.sin(LAT) * np.cos(LON)], axis=-1)  # matches uv_sphere's axes
    dots = np.stack([d @ c for c, _ in cells], axis=-1)
    bias = np.array([0.0 if k == 0 else -0.028 for _, k in cells])  # hexagons win a little more: even panel areas
    score = dots + (-bias)
    order = np.argsort(-score, axis=-1)
    best = np.take_along_axis(score, order[..., :1], -1)[..., 0]
    second = np.take_along_axis(score, order[..., 1:2], -1)[..., 0]
    idx = order[..., 0]
    gap = (best - second)  # 0 on a seam
    kind = np.array([k for _, k in cells])[idx]
    rng = np.random.RandomState(7)
    tone = rng.uniform(0.0, 1.0, len(cells))[idx]
    # panel colours
    hex_base = np.array([236, 240, 246], dtype=np.float64)
    hex_tint = np.array([214, 224, 240], dtype=np.float64)
    pent_base = np.array([34, 44, 74], dtype=np.float64)
    img = np.where((kind == 1)[..., None], hex_base * (1 - 0.35 * tone[..., None]) + hex_tint * 0.35 * tone[..., None],
                   pent_base * (0.8 + 0.4 * tone[..., None]))
    # a soft inner shading of every panel: darker toward the seams
    soft = np.clip(gap / 0.05, 0.0, 1.0) ** 0.7
    img = img * (0.78 + 0.22 * soft[..., None])
    # seams: a dark groove with a thin glowing line in the middle
    groove = np.clip(1.0 - gap / 0.016, 0.0, 1.0)
    glow = np.clip(1.0 - gap / 0.0075, 0.0, 1.0)
    img = img * (1.0 - 0.65 * groove[..., None]) + np.array(accent, dtype=np.float64) * glow[..., None] * 0.95
    # a faint bright rim on the pentagons
    rim = np.clip(1.0 - np.abs(gap - 0.035) / 0.006, 0.0, 1.0) * (kind == 0)
    img = img + np.array(accent, dtype=np.float64) * 0.35 * rim[..., None]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def build(ctx):
    ctx.texture("ball", texture())
    m = uv_sphere((0, 0, 0), RADIUS, segments=32, rings=20)
    m.name = "ball"
    ctx.mesh("ball", m, origin=(0, 0, 0), note="smooth UV sphere r 0.9275 m; equirectangular ball.tga with panels and seam glow")
