"""The stadium around the arena: stand blocks with a crowd, the ad boards, the shell with a roof, the ground, floodlights,
scoreboards and the sky dome. Everything is original ("MYENGINE ARENA"), nothing is taken from Rocket League.

A stand block is a local mesh: the front (the ad board) is at z = 0 and looks to +z (towards the field), the rows rise towards
-z. The block origin of the OBJ is the middle of the block; scene.py places it with the yaw that turns +z to the field.

The scene shader draws every entity with one draw call and has no instancing, so the crowd is baked into the block meshes
(a fan is ~50 triangles) and the blocks are the repeated entities.
"""

import math
import random

from .common import (BLUE, BLUE_GLOW, GRAPHITE, ORANGE, ORANGE_GLOW, WHITE, Atlas, canvas, mix, shade)
from .mesh import (FULL, Mesh, Xf, beam, box, cylinder, grid_surface, lerp, revolve, rot_x, rot_y, rot_z, translate, uv_sphere, vadd, vsub,
                   normalize)

ROWS = 16
TREAD = 0.9
RISER = 0.5
FIRST_Y = 1.1
BOARD_H = 1.1
BLOCK_W = 13.3
SEAT_MARGIN = 0.7
APRON = 12.5  # from the wall plane of the arena to the front of the stands
SHELL = 36.0  # from the wall plane to the facade of the stadium shell

PALETTE = ["skin0", "skin1", "skin2", "skin3", "skin4", "skin5", "hair0", "hair1", "hair2", "hair3", "hair4", "blue1", "blue2",
           "blue3", "orange1", "orange2", "orange3", "white", "grey", "black", "red", "green", "yellow"]
PALETTE_RGB = {
    "skin0": (255, 224, 196), "skin1": (241, 194, 152), "skin2": (224, 172, 120), "skin3": (198, 134, 84), "skin4": (141, 85, 52),
    "skin5": (94, 57, 36), "hair0": (28, 22, 20), "hair1": (70, 44, 28), "hair2": (122, 82, 40), "hair3": (190, 150, 80),
    "hair4": (150, 150, 150), "blue1": (42, 109, 244), "blue2": (24, 70, 180), "blue3": (110, 160, 255), "orange1": (255, 122, 26),
    "orange2": (214, 86, 10), "orange3": (255, 170, 90), "white": (240, 242, 245), "grey": (120, 126, 138), "black": (32, 34, 40),
    "red": (210, 40, 44), "green": (40, 170, 90), "yellow": (245, 210, 40),
}
SHIRTS = {
    "blue": [("blue1", 0.30), ("blue2", 0.20), ("blue3", 0.12), ("white", 0.12), ("grey", 0.08), ("black", 0.06), ("orange1", 0.04),
             ("red", 0.02), ("yellow", 0.03), ("green", 0.03)],
    "orange": [("orange1", 0.30), ("orange2", 0.20), ("orange3", 0.12), ("white", 0.12), ("grey", 0.08), ("black", 0.06), ("blue1", 0.04),
               ("red", 0.02), ("yellow", 0.03), ("green", 0.03)],
    "mixed": [("blue1", 0.17), ("blue2", 0.07), ("orange1", 0.17), ("orange2", 0.07), ("white", 0.16), ("grey", 0.10), ("black", 0.08),
              ("red", 0.05), ("yellow", 0.05), ("green", 0.04), ("blue3", 0.02), ("orange3", 0.02)],
}


def font(size, bold=True):
    from PIL import ImageFont
    for name in (("arialbd.ttf", "impact.ttf", "segoeuib.ttf", "DejaVuSans-Bold.ttf") if bold else ("arial.ttf", "DejaVuSans.ttf")):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    try:
        return ImageFont.load_default(size)
    except TypeError:
        return ImageFont.load_default()


# --------------------------------------------------------------------------- the stands atlas
def stands_atlas():
    a = Atlas(1024, 512, (40, 44, 54))
    for i, n in enumerate(PALETTE):
        a.cell(n, i * 32, 0, i * 32 + 32, 32)
    a.cell("concrete", 0, 32, 256, 160)
    a.cell("aisle", 256, 32, 384, 160)
    a.cell("seat", 384, 32, 512, 160)
    a.cell("metal", 512, 32, 640, 160)
    a.cell("ad0", 0, 160, 1024, 256)
    a.cell("ad1", 0, 256, 1024, 352)
    cv = a.cv
    for n in PALETTE:
        x0, y0, x1, y1 = a.cells[n]
        cv.rect(x0, y0, x1, y1, fill=PALETTE_RGB[n])
    x0, y0, x1, y1 = a.cells["concrete"]
    cv.vgrad(x0, y0, x1, y1, (120, 124, 132), (92, 96, 106))
    for k in range(1, 8):  # expansion joints
        cv.rect(x0 + k * 32, y0, x0 + k * 32 + 1, y1, fill=(76, 80, 90))
    x0, y0, x1, y1 = a.cells["aisle"]
    cv.rect(x0, y0, x1, y1, fill=(70, 74, 84))
    cv.rect(x0 + 4, y0, x0 + 12, y1, fill=(235, 205, 60))
    cv.rect(x1 - 12, y0, x1 - 4, y1, fill=(235, 205, 60))
    x0, y0, x1, y1 = a.cells["seat"]
    cv.vgrad(x0, y0, x1, y1, (34, 40, 66), (22, 26, 44))
    x0, y0, x1, y1 = a.cells["metal"]
    cv.vgrad(x0, y0, x1, y1, (86, 92, 104), (44, 48, 58))
    return a


def ad_strips(img, atlas):
    """Draws the boards (the text is drawn on the finished image: ImageDraw with a TrueType font)."""
    from PIL import ImageDraw
    d = ImageDraw.Draw(img)
    specs = [
        ("ad0", [("MYENGINE ARENA", (255, 255, 255), (20, 52, 140), 1.5), ("TURBO COLA", (255, 238, 150), (190, 40, 30), 1.0),
                 ("NEON FUEL", (120, 255, 200), (18, 58, 52), 1.0), ("BOOST UP", (255, 255, 255), (230, 110, 20), 1.0)]),
        ("ad1", [("ITMO ENGINES", (255, 255, 255), (36, 36, 44), 1.4), ("PIXEL BANK", (255, 255, 255), (30, 110, 70), 1.0),
                 ("DX12 LEAGUE", (255, 214, 96), (30, 30, 90), 1.2), ("GOAL!", (255, 255, 255), (150, 24, 110), 0.8)]),
    ]
    for name, panels in specs:
        x0, y0, x1, y1 = atlas.cells[name]
        total = sum(p[3] for p in panels)
        x = x0
        for text, fg, bg, weight in panels:
            w = int((x1 - x0) * weight / total)
            d.rectangle([x, y0, x + w - 1, y1 - 1], fill=bg)
            d.rectangle([x, y0, x + w - 1, y0 + 4], fill=tuple(min(255, c + 60) for c in bg))
            d.rectangle([x, y1 - 5, x + w - 1, y1 - 1], fill=tuple(max(0, c - 30) for c in bg))
            f = font(60)
            tw = d.textlength(text, font=f)
            size = 60
            while tw > w - 24 and size > 14:
                size -= 4
                f = font(size)
                tw = d.textlength(text, font=f)
            d.text((x + (w - tw) / 2, y0 + (y1 - y0) / 2), text, fill=fg, font=f, anchor="lm")
            d.rectangle([x + w - 2, y0, x + w - 1, y1 - 1], fill=(10, 10, 14))
            x += w
    return img


# --------------------------------------------------------------------------- the stand block
def solid(atlas, name):
    u, v = atlas.uv_point(name)
    return (u, v, u, v)


def build_fan(atlas, shirt, skin, hair, hat, flag):
    """One seated fan facing +z, the origin at the seat (centre of the seat on the tread)."""
    shirt_r, skin_r, hair_r = solid(atlas, shirt), solid(atlas, skin), solid(atlas, hair)
    m = Mesh("fan")
    m.extend(box((0, 0.69, 0.0), (0.42, 0.50, 0.26), rect=shirt_r, skip=("-y", "-z")))
    m.extend(uv_sphere((0, 1.08, 0.02), 0.125, 6, 3, rect=skin_r))
    m.extend(box((0, 1.13, -0.035), (0.20, 0.17, 0.12), rect=hair_r, skip=("-y", "+z")))  # hair at the back of the head
    if hat:
        m.extend(box((0, 1.20, 0.02), (0.27, 0.05, 0.27), rect=solid(atlas, hat), skip=("-y",)))
    if flag:
        m.extend(box((0.26, 0.95, 0.12), (0.025, 1.10, 0.025), rect=solid(atlas, "grey"), skip=("-y", "-z")))
        m.extend(box((0.26, 1.42, 0.12), (0.01, 0.30, 0.46), rect=solid(atlas, flag)))
    return m


def build_arms(atlas, skin, arms, pose):
    """Raised arms as boxes tilted outwards (arms = 1: only the left one); `pose` is the tilt in degrees."""
    skin_r = solid(atlas, skin)
    m = Mesh("arms")
    for sx in (-1, 1):
        if arms == 1 and sx == 1:
            continue
        a = box((0, 0.25, 0), (0.085, 0.50, 0.085), rect=skin_r, skip=("-y", "-z"))
        m.extend(a.transformed(translate(sx * 0.255, 0.92, 0.04) * rot_z(-sx * pose)))
    return m


def build_stand(variant, seed, atlas):
    rng = random.Random(seed)
    W = BLOCK_W
    m = Mesh("stand_" + variant)
    conc, aisle, seat, metal = atlas.uv("concrete"), atlas.uv("aisle"), atlas.uv("seat"), atlas.uv("metal")
    ad = atlas.uv("ad0" if seed % 2 == 0 else "ad1")
    hw = W / 2
    up, toward = ((0, 1, 0),) * 4, ((0, 0, 1),) * 4
    m.extend(box((0, BOARD_H / 2, 0.10), (W, BOARD_H, 0.20), rects={"+z": ad}, rect=metal, skip=("-y", "-z")))  # the ad board
    seat_n = int((W - 2 * SEAT_MARGIN) / 0.86)
    pitch = (W - 2 * SEAT_MARGIN) / seat_n
    aisle_k = seat_n // 2
    names = [w[0] for w in SHIRTS[variant]]
    probs = [w[1] for w in SHIRTS[variant]]
    y_prev, z_front = 0.0, -0.2
    for row in range(ROWS):
        y = FIRST_Y + row * RISER
        z0, z1 = z_front, z_front - TREAD
        m.quad((-hw, y_prev, z0), (hw, y_prev, z0), (hw, y, z0), (-hw, y, z0), conc, normals=toward)  # the riser
        m.quad((-hw, y, z0), (hw, y, z0), (hw, y, z1), (-hw, y, z1), conc, normals=up)  # the tread
        for sx in (-1, 1):  # the side caps of the stairs: a column under every tread
            m.quad((sx * hw, 0, z0), (sx * hw, y, z0), (sx * hw, y, z1), (sx * hw, 0, z1), conc, normals=((sx, 0, 0),) * 4)
        # the aisle stripe over the riser and the tread in the middle of the block
        ax0 = (aisle_k - (seat_n - 1) / 2.0) * pitch - 0.45
        m.quad((ax0, y_prev + 0.002, z0 + 0.002), (ax0 + 0.9, y_prev + 0.002, z0 + 0.002), (ax0 + 0.9, y + 0.002, z0 + 0.002),
               (ax0, y + 0.002, z0 + 0.002), aisle, normals=toward)
        m.quad((ax0, y + 0.003, z0), (ax0 + 0.9, y + 0.003, z0), (ax0 + 0.9, y + 0.003, z1), (ax0, y + 0.003, z1), aisle, normals=up)
        # the bench seat and the backrest, in two strips left and right of the aisle
        zc = z0 - TREAD * 0.55
        for lo, hi in ((-hw + 0.35, ax0 - 0.05), (ax0 + 0.95, hw - 0.35)):
            m.extend(box(((lo + hi) / 2, y + 0.40, zc + 0.05), (hi - lo, 0.07, 0.40), rect=seat, skip=("-y", "-z")))
            m.extend(box(((lo + hi) / 2, y + 0.62, zc - 0.17), (hi - lo, 0.42, 0.05), rect=seat, skip=("-y", "-z", "+y")))
        # the crowd
        for k in range(seat_n):
            if k == aisle_k or rng.random() > 0.93:
                continue
            x = -hw + SEAT_MARGIN + (k + 0.5) * pitch
            shirt = rng.choices(names, probs)[0]
            skin = "skin%d" % rng.choices(range(6), [3, 4, 4, 3, 2, 1])[0]
            hair = "hair%d" % rng.choices(range(5), [4, 3, 2, 1, 1])[0]
            r = rng.random()
            arms = 2 if r < 0.34 else (1 if r < 0.52 else 0)
            hat = rng.choice(["blue1", "orange1", "white", "black"]) if rng.random() < 0.10 else None
            flag = (shirt if shirt in ("blue1", "orange1", "white") else None) if rng.random() < 0.08 else None
            fan = build_fan(atlas, shirt, skin, hair, hat, flag)
            if arms:
                fan.extend(build_arms(atlas, skin, arms, rng.uniform(8, 26)))
            m.extend(fan, translate(x, y, zc + 0.02) * rot_y(rng.uniform(-14, 14)))
        y_prev, z_front = y, z1
    m.quad((-hw, 0, z_front), (hw, 0, z_front), (hw, y_prev + 1.0, z_front), (-hw, y_prev + 1.0, z_front), metal, normals=toward)  # the back
    m.smooth(35.0)
    return m


# --------------------------------------------------------------------------- the rest
def facade_texture():
    """512x512: a dark steel facade, lit window bands and cyan light strips; 24 m wide, 44 m high."""
    cv = canvas(512, 512, (24, 28, 40))
    cv.vgrad(0, 0, 512, 512, (18, 22, 34), (46, 54, 76))
    for r in range(11):
        y = 40 + r * 40
        cv.rect(0, y, 512, y + 6, fill=(14, 18, 28))
        for c in range(16):
            lit = (r * 7 + c * 13) % 5 != 0
            cv.rect(c * 32 + 6, y + 10, c * 32 + 26, y + 30, fill=(176, 142, 92) if lit else (30, 38, 58))
    for c in range(0, 512, 128):
        cv.rect(c + 60, 0, c + 68, 512, fill=(90, 220, 255))
    cv.rect(0, 20, 512, 32, fill=(240, 244, 255))
    return cv.finish()


def roof_texture():
    cv = canvas(256, 256, (22, 26, 36))
    for k in range(8):
        y = 16 + k * 32
        cv.vgrad(0, y, 256, y + 10, (255, 250, 230), (255, 236, 190))
        cv.rect(0, y + 10, 256, y + 18, fill=(40, 46, 60))
    return cv.finish()


def ground_texture():
    cv = canvas(256, 256, (40, 44, 52))
    cv.vgrad(0, 0, 256, 256, (46, 50, 58), (36, 40, 48))
    for k in range(0, 256, 64):
        cv.rect(k, 0, k + 2, 256, fill=(30, 33, 40))
        cv.rect(0, k, 256, k + 2, fill=(30, 33, 40))
    return cv.finish()


def sky_texture(w=2048, h=1024):
    import numpy as np
    from PIL import Image
    y = (np.arange(h) + 0.5) / h  # 0 = the top of the image = the zenith
    zen = np.array((6, 10, 28), dtype=np.float64)
    mid = np.array((18, 34, 78), dtype=np.float64)
    hor = np.array((72, 98, 150), dtype=np.float64)
    below = np.array((14, 18, 30), dtype=np.float64)
    img = np.zeros((h, w, 3), dtype=np.float64)
    for i, t in enumerate(y):
        if t < 0.5:
            f = t / 0.5
            c = zen + (mid - zen) * min(1.0, f * 1.5) if f < 0.67 else mid + (hor - mid) * ((f - 0.67) / 0.33) ** 1.6
        else:
            f = min(1.0, (t - 0.5) / 0.08)
            c = hor + (below - hor) * f
        img[i, :, :] = c
    rng = np.random.RandomState(11)
    for _ in range(900):  # stars in the upper part
        sx, sy = rng.randint(0, w), int(rng.beta(1.0, 1.8) * h * 0.42)
        b = rng.uniform(120, 255)
        img[sy:sy + 1 + (rng.rand() < 0.15), sx:sx + 1 + (rng.rand() < 0.15)] = (b, b, min(255, b + 10))
    # a soft moon
    cx, cy = int(w * 0.62), int(h * 0.20)
    yy, xx = np.mgrid[0:h, 0:w]
    d = np.hypot(xx - cx, yy - cy)
    glow = np.clip(1 - d / 90.0, 0, 1) ** 2 * 80
    img += glow[..., None] * np.array((0.8, 0.9, 1.0))
    img[d < 16] = (238, 242, 250)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def floodlight_atlas():
    a = Atlas(128, 64, GRAPHITE)
    a.cell("metal", 0, 0, 64, 64)
    a.cell("lamp", 64, 0, 128, 64)
    cv = a.cv
    cv.vgrad(0, 0, 64, 64, (96, 102, 114), (50, 54, 64))
    cv.vgrad(64, 0, 128, 64, (255, 255, 244), (255, 232, 170))
    return a


def scoreboard_atlas():
    a = Atlas(768, 256, GRAPHITE)
    a.cell("screen", 0, 0, 512, 256)
    a.cell("metal", 512, 0, 640, 256)
    a.cell("dark", 640, 0, 768, 256)
    cv = a.cv
    cv.rect(0, 0, 512, 256, fill=(8, 12, 22))
    cv.rect(0, 0, 256, 256, fill=(14, 36, 96))
    cv.rect(256, 0, 512, 256, fill=(100, 44, 8))
    cv.rect(0, 0, 512, 12, fill=(255, 255, 255))
    cv.vgrad(512, 0, 640, 256, (96, 102, 114), (44, 48, 58))
    cv.rect(640, 0, 768, 256, fill=(20, 22, 28))
    return a


def draw_scoreboard(img, atlas):
    from PIL import ImageDraw
    d = ImageDraw.Draw(img)
    f = font(120)
    d.text((128, 140), "0", fill=(255, 255, 255), font=f, anchor="mm")
    d.text((384, 140), "0", fill=(255, 255, 255), font=f, anchor="mm")
    d.text((256, 190), "05:00", fill=(255, 235, 150), font=font(54), anchor="mm")
    d.text((128, 50), "BLUE", fill=(150, 200, 255), font=font(40), anchor="mm")
    d.text((384, 50), "ORANGE", fill=(255, 200, 130), font=font(40), anchor="mm")
    return img


def build_shell(foot, atlas_unused=None):
    """The facade (an inward facing wall of the offset octagon) and the roof ring under which the stands sit."""
    wall_h = 44.0
    m = Mesh("stadium_shell")
    centre = (0.0, 6.0, 0.0)
    tile = 24.0
    for edge in range(8):
        a, b = foot.point(edge, 0.0, -SHELL, 0.0), foot.point(edge, 1.0, -SHELL, 0.0)
        a2, b2 = (a[0], wall_h, a[2]), (b[0], wall_h, b[2])
        u0, u1 = foot.dist(edge, 0.0) / tile, foot.dist(edge, 1.0) / tile
        u1 = u0 + (math.dist(a, b)) / tile
        m.quad(a, b, b2, a2, ((u0, 0.0), (u1, 0.0), (u1, 1.0), (u0, 1.0)), outside=centre)
    return m


def build_roof(foot):
    m = Mesh("stadium_roof")
    centre = (0.0, 6.0, 0.0)
    for edge in range(8):
        a, b = foot.point(edge, 0.0, -SHELL, 44.0), foot.point(edge, 1.0, -SHELL, 44.0)
        a2, b2 = foot.point(edge, 0.0, -(APRON + 3.0), 31.5), foot.point(edge, 1.0, -(APRON + 3.0), 31.5)
        ln = math.dist(a, b) / 24.0
        m.quad(a, b, b2, a2, ((0, 0), (ln, 0), (ln, 1), (0, 1)), outside=centre)
        # the lip
        a3, b3 = (a2[0], 29.5, a2[2]), (b2[0], 29.5, b2[2])
        m.quad(a2, b2, b3, a3, ((0, 0.95), (ln, 0.95), (ln, 1.0), (0, 1.0)), outside=centre)
    return m


def build_ground(foot):
    m = Mesh("stadium_ground")
    ring = [foot.point(e, 0.0, -SHELL, -0.05) for e in range(8)]
    c = (0.0, -0.05, 0.0)
    uv = lambda p: (p[0] / 12.0, p[2] / 12.0)
    for i in range(8):
        m.tri(c, ring[i], ring[(i + 1) % 8], uv(c), uv(ring[i]), uv(ring[(i + 1) % 8]), normals=((0, 1, 0),) * 3)
    return m


def build_sky(radius=250.0):
    def fn(u, v):
        ang = 2 * math.pi * u
        lat = math.pi * (v - 0.5)  # -90 .. +90 degrees
        return (radius * math.cos(lat) * math.sin(ang), radius * math.sin(lat), radius * math.cos(lat) * math.cos(ang))
    m = grid_surface(fn, 48, 24, outside=(0, 0, 0))
    m.name = "sky_dome"
    return m


def build_floodlight(atlas):
    metal, lamp = atlas.uv("metal"), atlas.uv("lamp")
    m = Mesh("floodlight")
    m.extend(cylinder((0, 0, 0), (0, 34, 0), 0.55, 0.26, 8, caps=(False, True), rect=metal))
    m.extend(box((0, 34.6, 0.15), (7.4, 4.6, 0.4), rect=metal, skip=("-z",)))
    for i in range(5):
        for j in range(3):
            m.extend(box((-3.0 + i * 1.5, 33.2 + j * 1.4 + 0.9, 0.5), (1.2, 1.1, 0.3), rect=lamp, skip=("-z", "-y")))
    return m


def build_scoreboard(atlas):
    screen, metal, dark = atlas.uv("screen"), atlas.uv("metal"), atlas.uv("dark")
    m = Mesh("scoreboard")
    m.extend(box((0, 0, 0), (19.0, 8.0, 1.6), rects={"+z": dark}, rect=metal, skip=("-z",)))
    m.extend(box((0, 0, 0.82), (17.6, 6.6, 0.06), rects={"+z": screen}, rect=metal, skip=("-z",)))
    return m


def build(ctx):
    import json
    from .arena import Footprint
    from .common import ROOT
    tuning = json.loads((ROOT / "assets" / "data" / "rl_tuning.json").read_text(encoding="utf-8"))["arena"]
    foot = Footprint(tuning)

    atlas = stands_atlas()
    img = ad_strips(atlas.image(), atlas)
    ctx.texture("stands", img)
    seeds = {"blue": 101, "orange": 202, "mixed": 303}
    for variant, seed in seeds.items():
        mesh = build_stand(variant, seed, atlas)
        lo, hi = mesh.bounds()
        origin = (0.0, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2)
        ctx.mesh("stand_" + variant, mesh, origin=origin, part_of="stadium",
                 note="stand block %.1f m wide, %d rows, baked crowd; front (ad board) at z=0 looks to +z" % (BLOCK_W, ROWS))
    ctx.texture("stadium_facade", facade_texture())
    ctx.mesh("stadium_shell", build_shell(foot), origin=(0.0, 22.0, 0.0), part_of="stadium", note="inward facing facade ring, 44 m")
    ctx.texture("stadium_roof", roof_texture())
    ctx.mesh("stadium_roof", build_roof(foot), origin=(0.0, 37.0, 0.0), part_of="stadium", note="roof ring over the stands")
    ctx.texture("stadium_ground", ground_texture())
    ctx.mesh("stadium_ground", build_ground(foot), origin=(0.0, -0.05, 0.0), part_of="stadium", note="concrete under everything, y = -0.05")
    ctx.texture("sky", sky_texture())
    ctx.mesh("sky_dome", build_sky(), origin=(0.0, 0.0, 0.0), part_of="stadium", note="inward facing sphere r 250 (far plane 300)")
    fa = floodlight_atlas()
    ctx.texture("floodlight", fa.image())
    ctx.mesh("floodlight", build_floodlight(fa), origin=(0.0, 18.0, 0.0), part_of="stadium", note="a 34 m tower, the lamps look to +z")
    sa = scoreboard_atlas()
    ctx.texture("scoreboard", draw_scoreboard(sa.image(), sa))
    ctx.mesh("scoreboard", build_scoreboard(sa), origin=(0.0, 0.0, 0.0), part_of="stadium", note="a hanging board, the screen looks to +z")
