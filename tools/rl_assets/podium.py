"""The podium of the main menu and of the results (docs/game/rl-ui.md): a low round plinth with a glowing ring on top and a second ring round its foot.
Origin = the centre of its bottom, on the floor of the arena. One tiny atlas: 0.0-0.5 the dark top plate, 0.5-1.0 the glow of the rings, the
lower half is the side. The glow is the emissive part of the lit shader (materials.py), so the bloom of G7b picks it up."""

import math

from PIL import Image

from .mesh import Mesh, cylinder, disc

RADIUS = 2.0
HEIGHT = 0.26

TOP = (0.0, 0.5, 0.5, 1.0)     # (u0, v0, u1, v1): the upper left quarter of the image (v = 1 is the top of the image)
GLOW = (0.5, 0.5, 1.0, 1.0)
SIDE = (0.0, 0.0, 1.0, 0.5)


def texture(size=128):
    img = Image.new("RGB", (size, size), (14, 24, 52))
    px = img.load()
    half = size // 2
    for y in range(size):
        for x in range(size):
            if y < half:                      # the top half of the image: the top plate (left) and the glow (right)
                if x < half:
                    # the plate: a navy gradient from the centre with a thin lighter grid
                    d = math.hypot(x - half / 2, y - half / 2) / (half * 0.7)
                    k = max(0.0, 1.0 - d)
                    c = (12 + int(30 * k), 24 + int(52 * k), 54 + int(86 * k))
                    if x % 16 == 0 or y % 16 == 0:
                        c = tuple(min(255, v + 14) for v in c)
                else:
                    c = (92, 214, 255)            # the glow: the accent colour of the kit
            else:                              # the bottom half: the side of the plinth
                t = (y - half) / half
                c = (int(22 + 18 * (1 - t)), int(34 + 24 * (1 - t)), int(66 + 40 * (1 - t)))
            px[x, y] = c
    return img


def build(ctx):
    ctx.texture("podium", texture())
    m = Mesh("podium")
    # the plinth: the foot is a little wider than the top
    m.extend(cylinder((0, 0.0, 0), (0, HEIGHT, 0), RADIUS + 0.12, RADIUS, 48, caps=(False, True), rect=SIDE, rect_caps=TOP))
    # the glowing ring on the top edge and the ring round the foot
    m.extend(disc((0, HEIGHT + 0.004, 0), RADIUS - 0.02, (0, 1, 0), 48, GLOW, inner=RADIUS - 0.16))
    m.extend(disc((0, 0.012, 0), RADIUS + 0.62, (0, 1, 0), 48, GLOW, inner=RADIUS + 0.52))
    m.extend(disc((0, 0.012, 0), RADIUS + 1.18, (0, 1, 0), 48, GLOW, inner=RADIUS + 1.14))
    ctx.mesh("podium", m, origin=(0, 0, 0), note="plinth of the menu: radius 2 m, 0.26 m high, glowing rings; the origin is the centre of the bottom")
