#!/usr/bin/env python3
"""Procedural assets of the Rocket League game (assets/scripts/rocket_league.py).

Writes
  assets/textures/rl/*.tga            RLE TGA, the format DirectXTex in ResourceManager loads
  assets/materials/rl/*.material.json textured_lit materials, one per texture
  assets/models/rl_box.obj            unit cube, every face has an upright 0..1 UV
  assets/models/rl_sphere.obj         UV sphere of radius 1 (the stock sphere.obj has no UV)

Run from anywhere:  python tools/gen_rl_assets.py
Needs Pillow (pip install pillow); nothing else.

Texture conventions (rl_box.obj): on every face u runs left to right and the image top is up when the face is
looked at from outside. On the top face (+y) the image top is +z (the orange end), the left edge is -x.
Sizes are authored for the stretched crate of the game, e.g. a 42 x 74 m floor is 756 x 1332 px (18 px/m).
"""

import json
import math
import random
import sys
from pathlib import Path

try:
    from PIL import Image, ImageChops, ImageDraw, ImageFilter
except ImportError:  # pragma: no cover
    sys.exit("Pillow is required: pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
TEX_DIR = ROOT / "assets" / "textures" / "rl"
MAT_DIR = ROOT / "assets" / "materials" / "rl"
MODEL_DIR = ROOT / "assets" / "models"

SS = 2  # supersampling of the drawn shapes
NOISE = False  # film grain: nicer, but it makes the RLE TGA files several times bigger

BLUE = (42, 109, 244)
BLUE_GLOW = (120, 180, 255)
ORANGE = (255, 122, 26)
ORANGE_GLOW = (255, 190, 100)
GOLD = (245, 197, 24)
WHITE = (240, 242, 245)

# field geometry of the script (metres): floor box 42 x 74, goal lines at z = +-30
FIELD_W, FIELD_L = 42.0, 74.0
HX, HZ, GOAL_W = 20.0, 30.0, 6.0
PX_PER_M = 18


def rgb(c, a=255):
    return (c[0], c[1], c[2], a)


def mix(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def shade(c, k):
    return tuple(max(0, min(255, int(round(v * k)))) for v in c[:3])


class Canvas:
    """Draws in output pixels, internally supersampled."""

    def __init__(self, w, h, base):
        self.w, self.h = w, h
        self.img = Image.new("RGBA", (w * SS, h * SS), rgb(base))
        self.d = ImageDraw.Draw(self.img)

    def _p(self, v):
        return v * SS

    def rect(self, x0, y0, x1, y1, fill=None, outline=None, width=1):
        self.d.rectangle([x0 * SS, y0 * SS, x1 * SS - 1, y1 * SS - 1],
                         fill=rgb(fill) if fill else None,
                         outline=rgb(outline) if outline else None, width=max(1, int(width * SS)))

    def line(self, pts, fill, width=1.0):
        self.d.line([(x * SS, y * SS) for x, y in pts], fill=rgb(fill), width=max(1, int(round(width * SS))))

    def ellipse(self, cx, cy, rx, ry, fill=None, outline=None, width=1.0):
        self.d.ellipse([(cx - rx) * SS, (cy - ry) * SS, (cx + rx) * SS, (cy + ry) * SS],
                       fill=rgb(fill) if fill else None, outline=rgb(outline) if outline else None,
                       width=max(1, int(round(width * SS))))

    def poly(self, pts, fill=None, outline=None):
        self.d.polygon([(x * SS, y * SS) for x, y in pts], fill=rgb(fill) if fill else None,
                       outline=rgb(outline) if outline else None)

    def hgrad(self, x0, y0, x1, y1, c0, c1):
        """Horizontal blend c0 -> c1 over [x0, x1]."""
        steps = max(1, int((x1 - x0) * SS))
        for i in range(steps):
            t = i / max(1, steps - 1)
            x = x0 * SS + i
            self.d.rectangle([x, y0 * SS, x, y1 * SS - 1], fill=rgb(mix(c0, c1, t)))

    def vgrad(self, x0, y0, x1, y1, c0, c1):
        steps = max(1, int((y1 - y0) * SS))
        for i in range(steps):
            t = i / max(1, steps - 1)
            y = y0 * SS + i
            self.d.rectangle([x0 * SS, y, x1 * SS - 1, y], fill=rgb(mix(c0, c1, t)))

    def glow_line(self, pts, color, width, blur=3.0, alpha=140):
        """A line with a soft halo (neon)."""
        halo = Image.new("RGBA", self.img.size, (0, 0, 0, 0))
        hd = ImageDraw.Draw(halo)
        hd.line([(x * SS, y * SS) for x, y in pts], fill=rgb(color, alpha), width=max(1, int(round(width * 3 * SS))))
        halo = halo.filter(ImageFilter.GaussianBlur(blur * SS))
        self.img = Image.alpha_composite(self.img, halo)
        self.d = ImageDraw.Draw(self.img)
        self.line(pts, color, width)

    def noise(self, amount, seed):
        # RLE TGA stays small only without noise: this is a no-op unless NOISE is switched on
        if not NOISE:
            return
        rnd = random.Random(seed)
        w, h = self.img.size
        small = Image.frombytes("L", (w // SS, h // SS), rnd.randbytes((w // SS) * (h // SS)))
        small = small.resize((w, h), Image.NEAREST)
        # centre the noise at 0: add to RGB through an offset image
        n = small.point(lambda v: int((v - 128) * amount / 128.0 + 128))
        rgb_img = self.img.convert("RGB")
        gray = Image.merge("RGB", (n, n, n))
        # overlay-like: base + (n - 128)
        out = ImageChops.add(rgb_img, gray, 1.0, -128)
        out.putalpha(255)
        self.img = out
        self.d = ImageDraw.Draw(self.img)

    def finish(self):
        return self.img.resize((self.w, self.h), Image.LANCZOS).convert("RGBA")


def save_tga(name, img):
    TEX_DIR.mkdir(parents=True, exist_ok=True)
    path = TEX_DIR / (name + ".tga")
    img.save(path, compression="tga_rle")
    print("  texture %-28s %dx%d" % (path.name, img.width, img.height))
    # a material of the same name
    MAT_DIR.mkdir(parents=True, exist_ok=True)
    material = {
        "shader": "../../shaders/textured_lit.shader.json",
        "texture": "../../textures/rl/%s.tga" % name,
        "tint": [1.0, 1.0, 1.0, 1.0],
    }
    (MAT_DIR / (name + ".material.json")).write_text(json.dumps(material, indent=2) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------- themes
THEMES = {
    "classic": {
        "floor_a": (38, 128, 52), "floor_b": (46, 148, 62), "line": WHITE, "pad_ground": (30, 100, 44),
        "wall": (24, 30, 44), "wall_line": (70, 84, 112), "grid": None, "stand": True,
    },
    "neon": {
        "floor_a": (8, 12, 30), "floor_b": (12, 18, 42), "line": (90, 230, 255), "pad_ground": (6, 8, 20),
        "wall": (10, 10, 24), "wall_line": (40, 44, 90), "grid": (30, 70, 140), "stand": False,
    },
    "desert": {
        "floor_a": (214, 176, 108), "floor_b": (203, 163, 94), "line": (250, 244, 228), "pad_ground": (180, 140, 78),
        "wall": (120, 66, 38), "wall_line": (86, 44, 24), "grid": None, "stand": False,
    },
}


def field_texture(theme, t):
    w, h = int(FIELD_W * PX_PER_M), int(FIELD_L * PX_PER_M)
    cv = Canvas(w, h, t["floor_a"])

    def X(x):  # metres -> px
        return (x + FIELD_W / 2) * PX_PER_M

    def Z(z):
        return (FIELD_L / 2 - z) * PX_PER_M

    # mowing bands across the field
    bands = 14
    band_h = h / bands
    for i in range(bands):
        cv.rect(0, i * band_h, w, (i + 1) * band_h, fill=t["floor_a"] if i % 2 == 0 else t["floor_b"])
    if t["grid"] is not None:  # neon: a faint grid every 2 m
        for i in range(-10, 11):
            cv.line([(X(i * 2), 0), (X(i * 2), h)], t["grid"], 1)
        for i in range(-18, 19):
            cv.line([(0, Z(i * 2)), (w, Z(i * 2))], t["grid"], 1)
    if theme == "desert":  # a few darker patches of sand
        rnd = random.Random(7)
        for _ in range(260):
            cx, cy = rnd.uniform(0, w), rnd.uniform(0, h)
            r = rnd.uniform(6, 26)
            cv.ellipse(cx, cy, r, r * rnd.uniform(0.5, 1.0), fill=mix(t["floor_a"], t["floor_b"], rnd.random()) )

    # goal mouths: floor inside the nets in team colour
    for sign, col in ((-1, BLUE), (1, ORANGE)):
        z0, z1 = sign * HZ, sign * (HZ + 5.0)
        top, bottom = sorted((Z(z0), Z(z1)))
        cv.rect(X(-GOAL_W), top, X(GOAL_W), bottom, fill=mix(t["floor_a"], col, 0.55))
        # team coloured goal line band
        cv.rect(X(-HX), Z(z0) - 0.6 * PX_PER_M if sign > 0 else Z(z0) - 0.6 * PX_PER_M,
                X(HX), Z(z0) + 0.6 * PX_PER_M, fill=mix(t["floor_a"], col, 0.7))

    line, lw = t["line"], 0.22 * PX_PER_M
    glow = theme == "neon"

    def L(pts, col=line, width=lw):
        if glow:
            cv.glow_line(pts, col, width * 0.6, blur=2.5)
        else:
            cv.line(pts, col, width)

    # border, centre line, centre circle and dot
    L([(X(-HX), Z(-HZ)), (X(HX), Z(-HZ)), (X(HX), Z(HZ)), (X(-HX), Z(HZ)), (X(-HX), Z(-HZ))])
    L([(X(-HX), Z(0)), (X(HX), Z(0))])
    circle = [(X(6 * math.cos(math.radians(a))), Z(6 * math.sin(math.radians(a)))) for a in range(0, 361, 4)]
    L(circle)
    cv.ellipse(X(0), Z(0), 0.7 * PX_PER_M, 0.7 * PX_PER_M, fill=line)
    # penalty areas
    for sign in (-1, 1):
        a, b = X(-(GOAL_W + 3)), X(GOAL_W + 3)
        zin, zout = sign * (HZ - 7.0), sign * HZ
        L([(a, Z(zout)), (a, Z(zin)), (b, Z(zin)), (b, Z(zout))])
        # corner boost pad rings are drawn by the pad materials
    cv.noise(10 if theme != "neon" else 4, seed=hash(theme) & 0xFFFF)
    return cv.finish()


def hex_points(cx, cy, r, rot=0.0):
    return [(cx + r * math.cos(math.radians(60 * i + rot)), cy + r * math.sin(math.radians(60 * i + rot)))
            for i in range(6)]


def wall_texture(theme, t, w, h, left_color, right_color, flip=False):
    """Side wall, 1 px = 1/30 m. Left half of the image is the 'left' colour, the right half 'right'."""
    cv = Canvas(w, h, t["wall"])
    ppm = h / 4.0
    cv.vgrad(0, 0, w, h, shade(t["wall"], 1.25), shade(t["wall"], 0.7))
    # panels: a vertical divider every 3 m and a horizontal one at the middle
    step = 3.0 * ppm
    x = 0.0
    while x < w:
        cv.line([(x, 0), (x, h)], t["wall_line"], max(1.5, ppm * 0.06))
        x += step
    cv.line([(0, h * 0.5), (w, h * 0.5)], t["wall_line"], max(1.0, ppm * 0.04))
    if theme == "desert":  # sandstone blocks
        for row in range(4):
            off = (row % 2) * step / 2
            xx = -off
            while xx < w:
                cv.line([(xx, row * ppm), (xx, (row + 1) * ppm)], t["wall_line"], 1.5)
                xx += step
            cv.line([(0, row * ppm), (w, row * ppm)], t["wall_line"], 1.5)
    # neon stripes top and bottom, team colours per half
    for (c0, c1, x0, x1) in ((left_color[0], left_color[1], 0, w / 2), (right_color[0], right_color[1], w / 2, w)):
        cv.rect(x0, 0.06 * h, x1, 0.06 * h + 0.07 * ppm * 1.0, fill=c0)
        cv.rect(x0, 0.86 * h, x1, 0.86 * h + 0.12 * ppm, fill=c0)
        cv.glow_line([(x0, 0.06 * h + 3), (x1, 0.06 * h + 3)], c1, max(1.0, 0.05 * ppm), blur=4, alpha=90)
        # a soft team coloured glow rising from the floor
        grad = Image.new("RGBA", (int(x1 - x0) * SS, int(h * 0.5) * SS))
        gd = ImageDraw.Draw(grad)
        for yy in range(grad.height):
            a = int(90 * (yy / grad.height) ** 2)
            gd.line([(0, yy), (grad.width, yy)], fill=rgb(c0, a))
        cv.img.alpha_composite(grad, (int(x0) * SS, int(h * 0.5) * SS))
        cv.d = ImageDraw.Draw(cv.img)
    cv.rect(w / 2 - 2, 0, w / 2 + 2, h, fill=(220, 220, 230))
    cv.noise(8, seed=len(theme) * 31 + w)
    img = cv.finish()
    # the image top is the top of the wall as the mesh shows it
    if flip:
        img = img.transpose(Image.FLIP_LEFT_RIGHT)
    return img


def end_wall_texture(theme, t, team_color, glow, w=700, h=200):
    cv = Canvas(w, h, t["wall"])
    ppm = h / 4.0
    cv.vgrad(0, 0, w, h, shade(t["wall"], 1.25), shade(t["wall"], 0.7))
    step = 3.0 * ppm
    x = 0.0
    while x < w:
        cv.line([(x, 0), (x, h)], t["wall_line"], max(1.5, ppm * 0.06))
        x += step
    cv.line([(0, h * 0.5), (w, h * 0.5)], t["wall_line"], max(1.0, ppm * 0.04))
    cv.rect(0, 0.06 * h, w, 0.06 * h + 0.07 * ppm, fill=team_color)
    cv.rect(0, 0.82 * h, w, 0.82 * h + 0.16 * ppm, fill=team_color)
    # big chevrons in the team colour
    for i in range(0, int(w / step) + 1):
        cx = i * step + step / 2
        cv.line([(cx - 0.6 * ppm, 0.62 * h), (cx, 0.4 * h), (cx + 0.6 * ppm, 0.62 * h)], mix(team_color, t["wall"], 0.35),
                max(2.0, 0.12 * ppm))
    cv.noise(8, seed=w + h)
    return cv.finish()


def goal_net_texture(team_color, size=256):
    cv = Canvas(size, size, shade(team_color, 0.18))
    n = 8
    step = size / n
    for i in range(-n, 2 * n + 1):
        cv.line([(i * step, 0), (i * step + size, size)], mix(team_color, WHITE, 0.25), 2.0)
        cv.line([(i * step + size, 0), (i * step, size)], mix(team_color, WHITE, 0.25), 2.0)
    cv.rect(0, 0, size, size, outline=team_color, width=size * 0.07)
    return cv.finish()


def ball_texture(w=1024, h=512):
    cv = Canvas(w, h, (236, 238, 242))
    r = 56.0
    dx, dy = r * 1.5, r * math.sqrt(3) / 2 * 2
    rnd = random.Random(3)
    cols = [(245, 247, 250), (214, 226, 242), (255, 222, 190), (228, 232, 238)]
    row = 0
    y = 0.0
    while y < h + dy:
        x = -r if row % 2 == 0 else -r + dx
        while x < w + r:
            cv.poly(hex_points(x, y, r - 3), fill=rnd.choice(cols), outline=(70, 80, 100))
            x += 2 * dx
        y += dy / 2
        row += 1
    # an equator band so that the spin is easy to read
    cv.rect(0, h * 0.47, w, h * 0.53, fill=(48, 56, 78))
    cv.rect(0, h * 0.485, w, h * 0.515, fill=(250, 250, 252))
    cv.noise(6, seed=11)
    return cv.finish()


def car_texture(team, accent, size=512):
    base = BLUE if team == "blue" else ORANGE
    cv = Canvas(size, size, base)
    cv.vgrad(0, 0, size, size, shade(base, 1.15), shade(base, 0.8))
    # racing stripes
    cv.rect(size * 0.40, 0, size * 0.46, size, fill=WHITE)
    cv.rect(size * 0.54, 0, size * 0.60, size, fill=WHITE)
    # panel lines and an accent frame
    cv.rect(0, 0, size, size, outline=shade(base, 0.4), width=size * 0.05)
    cv.rect(size * 0.05, size * 0.05, size * 0.95, size * 0.95, outline=accent, width=size * 0.015)
    cv.line([(0, size * 0.5), (size, size * 0.5)], shade(base, 0.5), 3)
    cv.noise(8, seed=len(team))
    return cv.finish()


def cabin_texture(kind, size=256):
    if kind == "glass":
        cv = Canvas(size, size, (18, 26, 44))
        cv.vgrad(0, 0, size, size, (70, 96, 140), (14, 20, 36))
        cv.poly([(size * 0.1, size), (size * 0.35, size), (size * 0.65, 0), (size * 0.4, 0)], fill=(120, 150, 200))
        cv.rect(0, 0, size, size, outline=(10, 12, 20), width=size * 0.06)
    else:  # the roof of the player's car
        cv = Canvas(size, size, GOLD)
        cv.vgrad(0, 0, size, size, (255, 226, 90), (200, 148, 10))
        cv.rect(0, 0, size, size, outline=(150, 100, 0), width=size * 0.06)
        cv.line([(size * 0.5, 0), (size * 0.5, size)], (255, 244, 170), 5)
        cv.noise(6, seed=5)
    return cv.finish()


def pad_texture(big, size=256):
    cv = Canvas(size, size, (14, 14, 18))
    col = ORANGE if big else GOLD
    glow = ORANGE_GLOW if big else (255, 236, 150)
    cv.rect(0, 0, size, size, fill=mix((14, 14, 18), col, 0.35))
    cv.poly(hex_points(size / 2, size / 2, size * 0.46, 30), fill=mix(col, (0, 0, 0), 0.25), outline=glow)
    cv.poly(hex_points(size / 2, size / 2, size * 0.30, 30), fill=col, outline=glow)
    cv.ellipse(size / 2, size / 2, size * 0.1, size * 0.1, fill=(255, 250, 220))
    return cv.finish()


def glow_texture(color, size=64):
    cv = Canvas(size, size, shade(color, 0.7))
    steps = 24
    for i in range(steps):
        t = i / (steps - 1)
        r = size * 0.7 * (1 - t)
        cv.ellipse(size / 2, size / 2, r, r, fill=mix(shade(color, 0.7), mix(color, WHITE, 0.55), t))
    return cv.finish()


def crowd_texture(color, size=256):
    cv = Canvas(size, size, (18, 20, 28))
    rnd = random.Random(sum(color))
    for gy in range(0, size, 8):
        for gx in range(0, size, 8):
            c = rnd.choice([color, shade(color, 0.6), WHITE, (200, 160, 120), shade(color, 1.2)])
            cv.ellipse(gx + 4, gy + 3, 2.5, 2.5, fill=c)
            cv.rect(gx + 1.5, gy + 5, gx + 6.5, gy + 8, fill=shade(c, 0.7))
    return cv.finish()


def neon_strip_texture(color, size=128):
    cv = Canvas(size, size, shade(color, 0.35))
    cv.vgrad(0, 0, size, size, mix(color, WHITE, 0.55), color)
    cv.rect(0, 0, size, size, outline=shade(color, 0.5), width=6)
    return cv.finish()


def sandstone_texture(size=256, base=(188, 140, 82)):
    cv = Canvas(size, size, base)
    step = size / 4
    for row in range(4):
        off = (row % 2) * step / 2
        for k in range(-1, 5):
            x = k * step + off
            cv.rect(x + 2, row * step + 2, x + step - 2, (row + 1) * step - 2,
                    fill=shade(base, 0.9 + 0.2 * ((row * 7 + k * 3) % 5) / 5))
    cv.noise(10, seed=9)
    return cv.finish()


def panel_texture(size=256):
    cv = Canvas(size, size, (28, 34, 48))
    cv.vgrad(0, 0, size, size, (44, 52, 72), (20, 24, 36))
    cv.rect(0, 0, size, size, outline=(90, 100, 130), width=4)
    cv.line([(0, size / 2), (size, size / 2)], (60, 70, 96), 2)
    return cv.finish()


# --------------------------------------------------------------------------- meshes
# ResourceManager loads OBJ like assimp's ConvertToLeftHanded: z and the z of the normals are negated and the winding is
# reversed. Meshes below are described in the world space of the engine and written with z mirrored.
def obj_space(p):
    return (p[0], p[1], -p[2])


def write_box_obj(path):
    """Unit cube: 24 vertices, upright 0..1 UV on every face, triangles wound like assets/models/crate.obj."""
    # (normal, origin corner, u axis, v axis) for a face looked at from outside: u to the right, v up.
    faces = [
        # +x seen from +x: right is +z
        ((1, 0, 0), (0.5, -0.5, -0.5), (0, 0, 1), (0, 1, 0)),
        # -x seen from -x: right is -z
        ((-1, 0, 0), (-0.5, -0.5, 0.5), (0, 0, -1), (0, 1, 0)),
        # +y seen from above with +z at the top of the image: right is +x, up is +z
        ((0, 1, 0), (-0.5, 0.5, -0.5), (1, 0, 0), (0, 0, 1)),
        # -y seen from below: right is +x, up is -z
        ((0, -1, 0), (-0.5, -0.5, 0.5), (1, 0, 0), (0, 0, -1)),
        # +z seen from +z (looking -z): right is -x
        ((0, 0, 1), (0.5, -0.5, 0.5), (-1, 0, 0), (0, 1, 0)),
        # -z seen from -z (looking +z): right is +x
        ((0, 0, -1), (-0.5, -0.5, -0.5), (1, 0, 0), (0, 1, 0)),
    ]
    out = ["# Unit cube with an upright 0..1 UV on every face (tools/gen_rl_assets.py)", "o rl_box"]
    verts, uvs, normals, tris = [], [(0, 0), (1, 0), (1, 1), (0, 1)], [], []
    for ni, (n, o, u, v) in enumerate(faces):
        normals.append(obj_space(n))
        quad = []
        for (su, sv) in ((0, 0), (1, 0), (1, 1), (0, 1)):
            p = tuple(o[i] + u[i] * su + v[i] * sv for i in range(3))
            verts.append(obj_space(p))
            quad.append(len(verts))
        a, b, c, d = quad
        # wind so that (b - a) x (c - b) points along the outward normal (the crate's convention)
        n = normals[ni]

        def cross_dot(i0, i1, i2):
            p0, p1, p2 = verts[i0 - 1], verts[i1 - 1], verts[i2 - 1]
            e1 = [p1[k] - p0[k] for k in range(3)]
            e2 = [p2[k] - p1[k] for k in range(3)]
            cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            return cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2]
        order = [(a, b, c), (a, c, d)]
        for tri in order:
            if cross_dot(*tri) < 0:
                tri = (tri[0], tri[2], tri[1])
            tris.append((tri, ni))
    for p in verts:
        out.append("v %.4f %.4f %.4f" % p)
    for uv in uvs:
        out.append("vt %.4f %.4f" % uv)
    for n in normals:
        out.append("vn %.4f %.4f %.4f" % n)
    for (tri, ni) in tris:
        # the UV index follows the corner of the quad: vertices are numbered 4 per face in (0,0),(1,0),(1,1),(0,1)
        refs = []
        for vi in tri:
            corner = (vi - 1) % 4
            refs.append("%d/%d/%d" % (vi, corner + 1, ni + 1))
        out.append("f " + " ".join(refs))
    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    print("  mesh    %s" % path.name)


def write_sphere_obj(path, seg=32, rings=16):
    out = ["# UV sphere of radius 1: u around Y, v from the south to the north pole (tools/gen_rl_assets.py)", "o rl_sphere"]
    verts, uvs, tris = [], [], []
    for r in range(rings + 1):
        phi = math.pi * r / rings  # 0 = north pole
        for s in range(seg + 1):
            theta = 2 * math.pi * s / seg
            x, y, z = math.sin(phi) * math.cos(theta), math.cos(phi), math.sin(phi) * math.sin(theta)
            verts.append(obj_space((x, y, z)))
            uvs.append((s / seg, 1.0 - r / rings))
    stride = seg + 1
    for r in range(rings):
        for s in range(seg):
            a = r * stride + s + 1
            b = a + stride
            c = b + 1
            d = a + 1
            for tri in ((a, b, c), (a, c, d)):
                p0, p1, p2 = (verts[i - 1] for i in tri)
                e1 = [p1[k] - p0[k] for k in range(3)]
                e2 = [p2[k] - p1[k] for k in range(3)]
                cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                centre = [(p0[k] + p1[k] + p2[k]) / 3 for k in range(3)]
                if cr[0] * centre[0] + cr[1] * centre[1] + cr[2] * centre[2] < 0:
                    tri = (tri[0], tri[2], tri[1])
                tris.append(tri)
    for p in verts:
        out.append("v %.5f %.5f %.5f" % p)
    for uv in uvs:
        out.append("vt %.5f %.5f" % uv)
    for p in verts:
        out.append("vn %.5f %.5f %.5f" % p)
    for tri in tris:
        out.append("f " + " ".join("%d/%d/%d" % (i, i, i) for i in tri))
    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    print("  mesh    %s" % path.name)


def main():
    print("meshes")
    MODEL_DIR.mkdir(parents=True, exist_ok=True)
    write_box_obj(MODEL_DIR / "rl_box.obj")
    write_sphere_obj(MODEL_DIR / "rl_sphere.obj")

    print("textures")
    for theme, t in THEMES.items():
        save_tga("%s_field" % theme, field_texture(theme, t))
        # left wall (x < 0): the blue half of the image is z < 0; the right wall is the mirrored image
        blue = (BLUE, BLUE_GLOW)
        orange = (ORANGE, ORANGE_GLOW)
        save_tga("%s_wall_left" % theme, wall_texture(theme, t, 1800, 120, blue, orange, flip=False))
        save_tga("%s_wall_right" % theme, wall_texture(theme, t, 1800, 120, blue, orange, flip=True))
        save_tga("%s_wall_end_blue" % theme, end_wall_texture(theme, t, BLUE, BLUE_GLOW))
        save_tga("%s_wall_end_orange" % theme, end_wall_texture(theme, t, ORANGE, ORANGE_GLOW))
    save_tga("goal_blue", goal_net_texture(BLUE))
    save_tga("goal_orange", goal_net_texture(ORANGE))
    save_tga("ball", ball_texture())
    save_tga("car_blue", car_texture("blue", BLUE_GLOW))
    save_tga("car_orange", car_texture("orange", ORANGE_GLOW))
    save_tga("cabin_glass", cabin_texture("glass"))
    save_tga("cabin_gold", cabin_texture("gold"))
    save_tga("pad_big", pad_texture(True))
    save_tga("pad_small", pad_texture(False))
    save_tga("fx_blue", glow_texture(BLUE))
    save_tga("fx_orange", glow_texture(ORANGE))
    save_tga("fx_gold", glow_texture(GOLD))
    save_tga("fx_white", glow_texture(WHITE))
    save_tga("crowd_blue", crowd_texture(BLUE))
    save_tga("crowd_orange", crowd_texture(ORANGE))
    save_tga("crowd_white", crowd_texture(WHITE))
    save_tga("neon_blue", neon_strip_texture(BLUE))
    save_tga("neon_orange", neon_strip_texture(ORANGE))
    save_tga("sandstone", sandstone_texture())
    save_tga("rock", sandstone_texture(base=(150, 120, 90)))
    save_tga("panel", panel_texture())
    print("done")


if __name__ == "__main__":
    main()
