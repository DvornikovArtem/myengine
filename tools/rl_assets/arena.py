"""The arena visual: it follows the shell of game/rl/src/RlArena.cpp (the same footprint, floor and ceiling ramps, goal
mouths) and reads its sizes from assets/data/rl_tuning.json.

Meshes (all in the world frame of the sim, y = 0 is the floor, blue defends -z, orange +z):
  arena_floor         the turf with the markings and the pad rings painted in (planar UV over the field)
  arena_wall_blue     floor ramps + the opaque lower wall (up to 7 m) of the blue half, the mouth jambs and lintel
  arena_wall_orange   the same for the orange half
  arena_frame_blue    the "glass" above the lower wall as an open frame: posts, rails, ceiling ramp ribs, ceiling beams
  arena_frame_orange  (open geometry instead of transparent glass: the scene shader has no blending, see rl-assets.md)
  arena_goal_blue     the inside of a goal behind the end wall: the net on the smooth profile (quarter pipes like the collision), posts, crossbar, rails
  arena_goal_orange
  pad_small, pad_big, pad_orb   the boost pads: a hexagon plate, a big plate with a ring, the floating orb
"""

import math

from .common import BLUE, BLUE_DARK, BLUE_GLOW, GRAPHITE, GOLD, ORANGE, ORANGE_DARK, ORANGE_GLOW, WHITE, Atlas, canvas, mix, shade
from .mesh import (FULL, Mesh, beam, box, cylinder, disc, length, lerp, loft, revolve, uv_sphere, vadd, vmul, vsub, normalize, cross)

TILE_U_M = 8.0  # the wall tile repeats every 8 m along the wall
WALL_TOP = 5.0  # the opaque lower wall ends here, above it the open frame
MOUTH_TOP = 7.0  # the header over a goal mouth (the mouth is 6.43 m high)
RAMP_FRAC = None  # set below: the share of the wall tile taken by the floor ramp
BAR = 0.24
ROOF_LIFT = 0.45   # the ceiling beams hang this far ABOVE the physical ceiling: the camera that slips through it never meets a beam

# --- boost pads: RLBot order, uu -> m (RlMatch.cpp kPadsUu); big = 3, 4, 15, 18, 29, 30
PADS_UU = [(0, -4240), (-1792, -4184), (1792, -4184), (-3072, -4096), (3072, -4096), (-940, -3308), (940, -3308), (0, -2816),
           (-3584, -2484), (3584, -2484), (-1788, -2300), (1788, -2300), (-2048, -1036), (0, -1024), (2048, -1036), (-3584, 0),
           (-1024, 0), (1024, 0), (3584, 0), (-2048, 1036), (0, 1024), (2048, 1036), (-1788, 2300), (1788, 2300), (-3584, 2484),
           (3584, 2484), (0, 2816), (-940, 3310), (940, 3308), (-3072, 4096), (3072, 4096), (-1792, 4184), (1792, 4184), (0, 4240)]
BIG_PADS = (3, 4, 15, 18, 29, 30)


def pad_positions():
    return [(x * 0.01, z * 0.01, i in BIG_PADS) for i, (x, z) in enumerate(PADS_UU)]


# --------------------------------------------------------------------------- footprint (port of RlArena.cpp)
class Footprint:
    def __init__(self, t):
        hx, hz, c = t["halfWidth"], t["halfLength"], t["cornerCut"]
        self.v = [(hx, 0, -(hz - c)), (hx, 0, hz - c), (hx - c, 0, hz), (-(hx - c), 0, hz), (-hx, 0, hz - c), (-hx, 0, -(hz - c)),
                  (-(hx - c), 0, -hz), (hx - c, 0, -hz)]
        self.n = []
        for i in range(8):
            d = normalize(vsub(self.v[(i + 1) % 8], self.v[i]))
            self.n.append((-d[2], 0.0, d[0]))
        self.m = []
        for i in range(8):
            a, b = self.n[(i + 7) % 8], self.n[i]
            k = 1.0 / (1.0 + a[0] * b[0] + a[2] * b[2])
            self.m.append(((a[0] + b[0]) * k, 0.0, (a[2] + b[2]) * k))
        self.lengths = [math.dist(self.v[i], self.v[(i + 1) % 8]) for i in range(8)]
        self.start = [sum(self.lengths[:i]) for i in range(8)]  # the perimeter distance of the edge start

    def point(self, edge, u, s, y):
        if u <= 0.0:
            p = vadd(self.v[edge], vmul(self.m[edge], s))
        elif u >= 1.0:
            p = vadd(self.v[(edge + 1) % 8], vmul(self.m[(edge + 1) % 8], s))
        else:
            p = vadd(lerp(self.v[edge], self.v[(edge + 1) % 8], u), vmul(self.n[edge], s))
        return (p[0], y, p[2])

    def dist(self, edge, u):
        return self.start[edge] + self.lengths[edge] * u


def edge_team(edge, u):
    """0 = blue (z < 0), 1 = orange. The side walls (0, 4) are split in the middle of the field."""
    if edge in (2, 3, 1):
        return 1
    if edge in (5, 6, 7):
        return 0
    return 1 if (u >= 0.5) == (edge == 0) else 0


# --------------------------------------------------------------------------- textures
def floor_texture(t, ppm=20):
    hx, hz = t["halfWidth"], t["halfLength"]
    w, h = int(round(2 * hx * ppm)), int(round(2 * hz * ppm))
    cv = canvas(w, h, (30, 92, 56))
    X = lambda x: (x + hx) * ppm  # metres -> px
    Z = lambda z: (hz - z) * ppm  # +z is the image top (the orange end)
    # mowing stripes across the field
    band = 6.4
    n = int(2 * hz / band) + 1
    for k in range(n):
        z0, z1 = -hz + k * band, min(hz, -hz + (k + 1) * band)
        if z1 - z0 < 0.1:
            continue
        col = (31, 96, 58) if k % 2 == 0 else (38, 108, 64)
        cv.rect(0, Z(z1), w, Z(z0), fill=col)
    line = (232, 240, 234)
    lw = 0.2

    def L(pts, col=line, width=lw):
        cv.line([(X(x), Z(z)) for x, z in pts], col, width * ppm)
    # the outline just inside the ramps, the centre line and the circle
    rf = t["floorRampRadius"]
    c = t["cornerCut"]
    ox, oz = hx - rf * 0.6, hz - rf * 0.6
    L([(-ox, -oz + c), (-ox + c, -oz), (ox - c, -oz), (ox, -oz + c), (ox, oz - c), (ox - c, oz), (-ox + c, oz), (-ox, oz - c), (-ox, -oz + c)])
    L([(-hx, 0), (hx, 0)])
    r = 8.0
    cv.ellipse(X(0), Z(0), r * ppm, r * ppm, outline=line, width=lw * ppm)
    cv.ellipse(X(0), Z(0), 0.6 * ppm, 0.6 * ppm, fill=line)
    # the goal areas: a big box (the 18 m "keeper area") and a small one in front of each goal
    for sign, col in ((-1, BLUE_GLOW), (1, ORANGE_GLOW)):
        gz = sign * hz
        L([(-14, gz - sign * 0.6 * rf), (-14, gz - sign * 17), (14, gz - sign * 17), (14, gz - sign * 0.6 * rf)])
        L([(-t["goalHalfWidth"], gz - sign * 0.6 * rf), (-t["goalHalfWidth"], gz - sign * 7.5), (t["goalHalfWidth"], gz - sign * 7.5),
           (t["goalHalfWidth"], gz - sign * 0.6 * rf)])
        # (no spot in front of the goal: the kick-off camera of the back spots stands two and a half metres from it, and a white dot seen at that
        # angle with the gloss of the floor blew up into a white blur at the corner of the screen)
        # a team coloured goal line glow
        L([(-t["goalHalfWidth"], gz - sign * 0.6 * rf), (t["goalHalfWidth"], gz - sign * 0.6 * rf)], col, 0.5)
    # the pad rings and the plates of the pads
    for x, z, big in pad_positions():
        r = 1.7 if big else 1.15
        cv.ellipse(X(x), Z(z), r * ppm, r * ppm, fill=(20, 66, 40), outline=(255, 190, 80), width=0.14 * ppm)
    # the team halves: a soft colour wash growing towards each end (drawn over the stripes and the lines)
    import numpy as np
    from PIL import Image
    img = cv.finish()
    arr = np.asarray(img.convert("RGB"), dtype=np.float64)
    z = hz - (np.arange(h) + 0.5) / ppm
    k = ((np.abs(z) / hz) ** 1.8 * 0.30)[:, None, None]
    tint = np.where((z < 0)[:, None, None], np.array(BLUE, dtype=np.float64), np.array(ORANGE, dtype=np.float64))
    arr = arr * (1 - k) + tint * k
    img = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    return img


def wall_texture(team, size=512, tuning=None):
    global RAMP_FRAC
    if tuning is not None:
        prof = lower_profile(tuning)
        RAMP_FRAC = prof[-2][2] / prof[-1][2]
    base, panel, accent, glow = (((16, 30, 66), (26, 52, 112), BLUE, BLUE_GLOW) if team == "blue"
                                 else ((64, 30, 12), (122, 58, 18), ORANGE, ORANGE_GLOW))
    s = size
    cv = canvas(s, s, base)
    ramp_h = int(s * (1 - RAMP_FRAC))  # image y of the ramp / wall seam (the wall part is above it)
    # the wall part: two big panels per tile with a lighter field and a team chevron
    cv.vgrad(0, 0, s, ramp_h, shade(base, 1.25), shade(base, 0.9))
    for k in range(2):
        x0, x1 = k * s / 2 + 10, (k + 1) * s / 2 - 10
        cv.rect(x0, 22, x1, ramp_h - 14, fill=shade(panel, 0.85), outline=shade(base, 0.6), width=3)
        cv.rect(x0 + 8, 30, x1 - 8, ramp_h - 22, fill=panel)
        cx = (x0 + x1) / 2
        cv.poly([(cx - 46, ramp_h - 40), (cx, ramp_h - 94), (cx + 46, ramp_h - 40), (cx + 46, ramp_h - 62), (cx, ramp_h - 116),
                 (cx - 46, ramp_h - 62)], fill=accent)
    # the glowing rail at the top of the wall (y = 7 m) and a dark band under it
    cv.rect(0, 0, s, 14, fill=glow)
    cv.rect(0, 14, s, 26, fill=shade(accent, 0.55))
    cv.rect(0, 0, s, 4, fill=WHITE)
    # the seam between the ramp and the wall
    cv.rect(0, ramp_h - 3, s, ramp_h + 4, fill=shade(base, 0.45))
    # the ramp: a field of hexagons fading to the floor, glowing strip at the very bottom
    cv.vgrad(0, ramp_h + 4, s, s, shade(base, 0.9), shade(base, 1.45))
    hexr = 28
    row_h = hexr * 1.5
    r = 0
    y = ramp_h + 20.0
    while y < s:
        off = (hexr * 0.866) if r % 2 else 0.0
        x = off - hexr
        while x < s + hexr:
            pts = [(x + hexr * 0.92 * math.cos(math.radians(60 * i + 30)), y + hexr * 0.92 * math.sin(math.radians(60 * i + 30))) for i in range(6)]
            cv.poly(pts, fill=mix(base, panel, 0.55 + 0.25 * ((r * 3 + int(x / 50)) % 3) / 2.0), outline=shade(base, 0.55))
            x += hexr * 1.732
        y += row_h
        r += 1
    cv.rect(0, s - 12, s, s, fill=accent)
    cv.rect(0, s - 5, s, s, fill=glow)
    return cv.finish()


def frame_texture(team):
    accent = BLUE_GLOW if team == "blue" else ORANGE_GLOW
    a = Atlas(128, 64, GRAPHITE)
    a.cell("metal", 0, 0, 64, 64)
    a.cell("accent", 64, 0, 128, 64)
    cv = a.cv
    cv.vgrad(0, 0, 64, 64, (72, 78, 90), (28, 31, 38))
    cv.rect(0, 0, 64, 4, fill=(120, 128, 142))
    cv.vgrad(64, 0, 128, 64, accent, shade(accent, 0.7))
    return a


def goal_texture(team):
    """512x256: the net tile (left) and the metal / accent cells of the frame (right)."""
    col, glow = (BLUE, BLUE_GLOW) if team == "blue" else (ORANGE, ORANGE_GLOW)
    a = Atlas(512, 256, GRAPHITE)
    a.cell("net", 0, 0, 256, 256)
    a.cell("metal", 256, 0, 384, 256)
    a.cell("accent", 384, 0, 512, 256)
    cv = a.cv
    cv.rect(0, 0, 256, 256, fill=shade(col, 0.16))
    # a diamond net: the lines are 12 px wide on 2 x 2 diamonds
    for k in range(-2, 5):
        o = k * 128
        cv.line([(o, 0), (o + 256, 256)], glow, 9)
        cv.line([(o + 256, 0), (o, 256)], glow, 9)
    for k in range(-2, 5):
        o = k * 128
        cv.line([(o, 0), (o + 256, 256)], shade(col, 1.0), 4)
    cv.vgrad(256, 0, 384, 256, (232, 236, 244), (120, 128, 142))
    cv.vgrad(384, 0, 512, 256, glow, col)
    return a


def goal_image(atlas, team):
    """The goal atlas as RGBA: the net tile is see-through between its lines (the material alpha-tests it), the frame cells opaque."""
    import numpy as np
    from PIL import Image
    col, glow = (BLUE, BLUE_GLOW) if team == "blue" else (ORANGE, ORANGE_GLOW)
    img = atlas.image().convert("RGBA")
    arr = np.asarray(img).copy()
    region = arr[:, :256, :3].astype(np.int32)
    background = np.array(shade(col, 0.16), dtype=np.int32)
    diff = np.abs(region - background).sum(axis=-1)
    alpha = np.clip(diff * 3, 0, 255).astype(np.uint8)
    arr[:, :256, 3] = alpha
    # the see-through texels carry the line colour, so a filtered edge does not get a dark fringe
    clear = alpha == 0
    for c in range(3):
        channel = arr[:, :256, c]
        channel[clear] = glow[c]
    return Image.fromarray(arr, "RGBA")


def glass_texture(team):
    """128x128 RGBA tile of the glass: a faint team tint, a few brighter streaks, the edges of the pane."""
    from PIL import Image, ImageDraw
    tint = {"blue": (70, 130, 255), "orange": (255, 150, 60), "roof": (150, 190, 255)}[team]
    img = Image.new("RGBA", (128, 128), tint + (34,))
    d = ImageDraw.Draw(img)
    for x, w in ((18, 5), (44, 2), (70, 9), (112, 3)):
        d.polygon([(x, 0), (x + w, 0), (x + w - 40, 128), (x - 40, 128)], fill=tint + (78,))
    d.rectangle([0, 0, 127, 127], outline=tint + (150,), width=2)
    return img


def build_glass(t, foot, team):
    """Translucent panels in the open frame: the wall above the lower wall (from the mouth header over a goal) and the ceiling ramp."""
    from .mesh import Mesh
    H, rc = t["height"], t["ceilingRampRadius"]
    cuts = mouth_cuts(t)
    m = Mesh("arena_glass_" + ("blue" if team == 0 else "orange"))
    offset = 0.03
    segs = 4
    centre = (0.0, 6.0, 0.0)
    for edge in range(8):
        spans = [(0.0, 1.0, WALL_TOP)] if edge not in (0, 4) else [(0.0, 0.5, WALL_TOP), (0.5, 1.0, WALL_TOP)]
        if edge in (2, 6):
            spans = [(0.0, cuts[0], WALL_TOP), (cuts[0], cuts[1], MOUTH_TOP), (cuts[1], 1.0, WALL_TOP)]
        for u0, u1, y_sill in spans:
            if edge_team(edge, (u0 + u1) * 0.5) != team:
                continue
            rib = [(offset, y_sill), (offset, H - rc)]
            for k in range(1, segs + 1):
                ang = 0.5 * math.pi * k / segs
                rib.append((offset + rc - rc * math.cos(ang), min(H - rc + rc * math.sin(ang), H - 0.02)))
            arc = [0.0]
            for i in range(1, len(rib)):
                arc.append(arc[-1] + math.dist(rib[i], rib[i - 1]))
            ua, ub = foot.dist(edge, u0) / TILE_U_M, foot.dist(edge, u1) / TILE_U_M
            for i in range(len(rib) - 1):
                (s0, y0), (s1, y1) = rib[i], rib[i + 1]
                q = [foot.point(edge, u0, s0, y0), foot.point(edge, u1, s0, y0), foot.point(edge, u1, s1, y1), foot.point(edge, u0, s1, y1)]
                v0, v1 = arc[i] / TILE_U_M, arc[i + 1] / TILE_U_M
                m.quad(*q, uvs=[(ua, v0), (ub, v0), (ub, v1), (ua, v1)], outside=centre)
    return m


def build_glass_roof(t, foot):
    from .mesh import Mesh
    H, rc = t["height"], t["ceilingRampRadius"]
    cuts = mouth_cuts(t)
    m = Mesh("arena_glass_roof")
    loop = floor_loop(foot, t, cuts, rc, H - 0.02)
    c = (0.0, H - 0.02, 0.0)
    uv = lambda p: (p[0] / TILE_U_M, p[2] / TILE_U_M)
    for i in range(len(loop)):
        a, b = loop[i], loop[(i + 1) % len(loop)]
        m.tri(c, a, b, uv(c), uv(a), uv(b), normals=((0, -1, 0),) * 3)
    return m


def pads_texture():
    a = Atlas(768, 256, (20, 22, 28))
    a.cell("small", 0, 0, 256, 256)
    a.cell("big", 256, 0, 512, 256)
    a.cell("orb", 512, 0, 640, 256)
    a.cell("trim", 640, 0, 768, 256)
    cv = a.cv
    amber, hot = (255, 170, 40), (255, 236, 150)
    for x0, rr, inner in ((0, 124, 0.72), (256, 124, 0.62)):
        cx, cy = x0 + 128, 128
        hexp = lambda r: [(cx + r * math.cos(math.radians(60 * i + 30)), cy - r * math.sin(math.radians(60 * i + 30))) for i in range(6)]
        cv.poly(hexp(rr), fill=(24, 26, 32))
        cv.poly(hexp(rr * 0.93), fill=amber)
        cv.poly(hexp(rr * 0.82), fill=(40, 30, 14))
        cv.poly(hexp(rr * inner), fill=shade(amber, 0.8))
        cv.poly(hexp(rr * (inner - 0.12)), fill=hot)
        cv.poly(hexp(rr * (inner - 0.30)), fill=(60, 40, 14))
        # a bolt in the middle (the boost sign)
        bolt = [(cx + 6, cy - 54), (cx - 24, cy + 6), (cx - 2, cy + 6), (cx - 10, cy + 54), (cx + 26, cy - 8), (cx + 2, cy - 8)]
        cv.poly(bolt, fill=hot if x0 == 0 else WHITE)
    cv.vgrad(512, 0, 640, 256, (255, 214, 96), (255, 120, 16))
    cv.rect(512, 112, 640, 144, fill=(255, 238, 170))
    cv.vgrad(640, 0, 768, 256, (70, 74, 86), (26, 28, 34))
    return a


# --------------------------------------------------------------------------- geometry
def floor_loop(foot, t, cuts, s, y):
    loop = []
    for edge in range(8):
        loop.append(foot.point(edge, 0.0, s, y))
        if edge in (2, 6):
            loop.append(foot.point(edge, cuts[0], s, y))
            loop.append(foot.point(edge, cuts[1], s, y))
    return loop


def build_floor(t, foot):
    hx, hz = t["halfWidth"], t["halfLength"]
    rf = t["floorRampRadius"]
    cuts = mouth_cuts(t)

    def uv(p):
        return ((p[0] + hx) / (2 * hx), (p[2] + hz) / (2 * hz))

    m = Mesh("arena_floor")
    loop = floor_loop(foot, t, cuts, rf, 0.0)
    c = (0.0, 0.0, 0.0)
    for i in range(len(loop)):
        a, b = loop[i], loop[(i + 1) % len(loop)]
        m.tri(c, a, b, uv(c), uv(a), uv(b), normals=((0, 1, 0),) * 3)
    for edge in (2, 6):  # the floor runs on to the wall plane through the goal mouth
        q = [foot.point(edge, cuts[0], rf, 0.0), foot.point(edge, cuts[1], rf, 0.0), foot.point(edge, cuts[1], 0.0, 0.0),
             foot.point(edge, cuts[0], 0.0, 0.0)]
        m.quad(*q, uvs=[uv(p) for p in q], normals=((0, 1, 0),) * 4)
    return m


def mouth_cuts(t):
    end_half = t["halfWidth"] - t["cornerCut"]
    return 0.5 * (1.0 - t["goalHalfWidth"] / end_half), 0.5 * (1.0 + t["goalHalfWidth"] / end_half)


def lower_profile(t):
    """(s, y, arc) of the lower wall: the floor ramp of RlArena (same radius and segments), then the straight wall up to WALL_TOP.
    arc is the length along the profile from the floor; v = arc / the length of the profile."""
    rf, segs = t["floorRampRadius"], max(t["rampSegments"], 1)
    pts = []
    for k in range(segs + 1):
        a = 0.5 * math.pi * k / segs
        pts.append((rf - rf * math.sin(a), rf - rf * math.cos(a)))
    pts.append((0.0, WALL_TOP))
    out, arc = [], 0.0
    for i, p in enumerate(pts):
        if i:
            arc += math.dist(p, pts[i - 1])
        out.append((p[0], p[1], arc))
    return out


def tile_height(t):
    return lower_profile(t)[-1][2]


def build_walls(t, foot, team):
    prof = lower_profile(t)
    total = prof[-1][2]
    cuts = mouth_cuts(t)
    gh = t["goalHeight"]
    m = Mesh("arena_wall_" + ("blue" if team == 0 else "orange"))
    inside = (0.0, t["height"] * 0.5, 0.0)
    ramp_v = 1.0 - (total - prof[-2][2]) / total  # v of the top of the floor ramp (the seam of the tile)
    for edge in range(8):
        spans = [(0.0, 1.0, "wall")]
        if edge in (2, 6):
            spans = [(0.0, cuts[0], "wall"), (cuts[0], cuts[1], "header"), (cuts[1], 1.0, "wall")]
        if edge in (0, 4):  # the side walls change hands in the middle
            spans = [(0.0, 0.5, "wall"), (0.5, 1.0, "wall")]
        for u0, u1, kind in spans:
            if edge_team(edge, (u0 + u1) * 0.5) != team:
                continue
            ua, ub = foot.dist(edge, u0) / TILE_U_M, foot.dist(edge, u1) / TILE_U_M
            if kind == "header":  # over the goal mouth: from the mouth height to MOUTH_TOP, the wall part of the tile
                q = [foot.point(edge, u0, 0, gh), foot.point(edge, u1, 0, gh), foot.point(edge, u1, 0, MOUTH_TOP), foot.point(edge, u0, 0, MOUTH_TOP)]
                m.quad(*q, uvs=[(ua, ramp_v), (ub, ramp_v), (ub, 1.0), (ua, 1.0)], outside=inside)
                continue
            for k in range(0, len(prof) - 1):
                (s0, y0, a0), (s1, y1, a1) = prof[k], prof[k + 1]
                q = [foot.point(edge, u0, s0, y0), foot.point(edge, u1, s0, y0), foot.point(edge, u1, s1, y1), foot.point(edge, u0, s1, y1)]
                m.quad(*q, uvs=[(ua, a0 / total), (ub, a0 / total), (ub, a1 / total), (ua, a1 / total)], outside=inside)
    # the jambs: the cut ends of the ramps beside the goal mouth, in the planes x = +-goalHalfWidth
    for edge in (2, 6):
        if edge_team(edge, 0.5) != team:
            continue
        for u in cuts:
            pivot = foot.point(edge, u, 0.0, 0.0)
            uj = foot.dist(edge, u) / TILE_U_M
            for k in range(0, len(prof) - 2):  # the ramp only
                (s0, y0, a0), (s1, y1, a1) = prof[k], prof[k + 1]
                p0, p1 = foot.point(edge, u, s0, y0), foot.point(edge, u, s1, y1)
                uvs = [(uj, 0.0), (uj + (p0[2] - pivot[2]) * 0.02, p0[1] / 8.0), (uj + (p1[2] - pivot[2]) * 0.02, p1[1] / 8.0)]
                m.tri(pivot, p0, p1, *uvs, outside=(0.0, 3.0, pivot[2]))
    m.smooth(45.0)
    return m


def build_frame(t, foot, team, atlas):
    """The open frame: rails and posts of the upper wall, the ribs of the ceiling ramp, the ceiling beams (above the physical ceiling)."""
    metal, accent = atlas.uv("metal"), atlas.uv("accent")
    H = t["height"]
    rc = t["ceilingRampRadius"]
    segs = 4
    m = Mesh("arena_frame_" + ("blue" if team == 0 else "orange"))
    up = (0, 1, 0)
    inset = BAR * 0.5
    y_beam = H + ROOF_LIFT + BAR * 0.5   # the axis of the ceiling beams
    rib = [(inset, WALL_TOP), (inset, H - rc)]  # the first point is replaced per span (the sill)
    for k in range(1, segs + 1):
        ang = 0.5 * math.pi * k / segs
        rib.append((inset + rc - rc * math.cos(ang), min(H - rc + rc * math.sin(ang), H - BAR * 0.5)))
    rib.append((inset + rc, y_beam))   # a short stub carries the rib above the ceiling to the beams
    cuts = mouth_cuts(t)
    mid_rails = (9.2, 13.3, H - rc)
    for edge in range(8):
        spans = [(0.0, 1.0, WALL_TOP)] if edge not in (0, 4) else [(0.0, 0.5, WALL_TOP), (0.5, 1.0, WALL_TOP)]
        if edge in (2, 6):  # over a goal mouth the glass starts at the header
            spans = [(0.0, cuts[0], WALL_TOP), (cuts[0], cuts[1], MOUTH_TOP), (cuts[1], 1.0, WALL_TOP)]
        for u0, u1, y_sill in spans:
            if edge_team(edge, (u0 + u1) * 0.5) != team:
                continue
            n_in = (foot.n[edge][0], 0.0, foot.n[edge][2])
            tangent = (-n_in[2], 0.0, n_in[0])
            # rails: the sill, two in the middle and the head of the glass band (the ones below the sill are skipped)
            for y in (y_sill + BAR * 0.5,) + tuple(v for v in mid_rails if v > y_sill + 1.0):
                m.extend(beam(foot.point(edge, u0, inset, y), foot.point(edge, u1, inset, y), BAR, BAR, up, metal, n_in, accent))
            # the rail at the end of the ceiling ramp
            m.extend(beam(foot.point(edge, u0, rc + inset, y_beam), foot.point(edge, u1, rc + inset, y_beam), BAR, BAR, up,
                          metal, (0, -1, 0), accent))
            # posts every ~4 m, each continuing in the ribs of the ceiling ramp
            n_posts = max(2, int(round(foot.lengths[edge] * (u1 - u0) / 4.0)) + 1)
            local_rib = [(inset, y_sill)] + rib[1:]
            for i in range(n_posts):
                if edge in (0, 4) and u1 == 0.5 and i == n_posts - 1:
                    continue  # the post in the middle belongs to the other half
                u = u0 + (u1 - u0) * i / (n_posts - 1)
                prev = None
                for (s_in, y) in local_rib:
                    p = foot.point(edge, u, s_in, y)
                    if prev is not None:
                        m.extend(beam(prev, p, BAR * 0.8, BAR * 0.8, tangent, metal, n_in, accent))
                    prev = p
    # the ceiling: beams across the roof opening (the opening is the footprint inset by the ramp radius)
    poly = [foot.point(e, 0.0, rc, H) for e in range(8)]
    step = 6.0
    z_max = t["halfLength"] - rc
    k = 0
    while k * step <= z_max + 1e-6:
        for z in ((0.0,) if k == 0 else (k * step, -k * step)):
            if (1 if z > 0 else 0) != team:
                continue
            lo, hi = poly_span(poly, "z", z)
            if hi - lo > 0.5:
                m.extend(beam((lo, y_beam, z), (hi, y_beam, z), BAR, BAR, up, metal, (0, -1, 0), accent))
        k += 1
    x_max = t["halfWidth"] - rc
    k = 0
    while k * step <= x_max + 1e-6:
        for x in ((0.0,) if k == 0 else (k * step, -k * step)):
            lo, hi = poly_span(poly, "x", x)
            if hi - lo < 0.5:
                continue
            a, b = (lo, min(hi, 0.0)) if team == 0 else (max(lo, 0.0), hi)
            if b - a > 0.5:
                m.extend(beam((x, y_beam, a), (x, y_beam, b), BAR, BAR, up, metal, (0, -1, 0), accent))
        k += 1
    return m


def poly_span(poly, axis, value):
    """The span of the other coordinate where the line axis = value crosses the convex polygon of (x, y, z) points."""
    ia, ib = (2, 0) if axis == "z" else (0, 2)
    hits = []
    for i in range(len(poly)):
        p, q = poly[i], poly[(i + 1) % len(poly)]
        a0, a1 = p[ia], q[ia]
        if (a0 - value) * (a1 - value) <= 0 and abs(a1 - a0) > 1e-9:
            f = (value - a0) / (a1 - a0)
            hits.append(p[ib] + (q[ib] - p[ib]) * f)
    return (min(hits), max(hits)) if hits else (0.0, 0.0)


def goal_profile(t):
    """The sections of the inside of a goal, from the mouth to the back wall: the same list as RlArena::GoalProfile (game/rl/src/RlArena.cpp), which is
    also the collision. A section is (along, inner_x, inner_y, radius): a rounded rectangle around (0, goalHeight / 2) with straight parts up to
    +-inner and quarter circle corners of `radius`. The radius grows from 0 at the mouth over goalMouthBlend metres (smoothstep), is goalCornerRadius in
    the straight part and shrinks to 0 along a quarter circle at the back (spheres in the back corners)."""
    segments = max(int(t["rampSegments"]), 1)
    gh, gw, depth = t["goalHeight"], t["goalHalfWidth"], max(t["goalDepth"], 0.1)
    radius = max(0.0, min(t.get("goalCornerRadius", 0.0), 0.5 * gh - 0.01, gw - 0.01, depth - 0.01))
    if radius <= 0.0:
        return [(0.0, gw, 0.5 * gh, 0.0), (depth, gw, 0.5 * gh, 0.0)]
    blend = min(max(t.get("goalMouthBlend", 5.0), 0.01), depth - radius)
    inner_x, inner_y = gw - radius, 0.5 * gh - radius
    sections = []
    steps = max(segments, 4)
    for k in range(steps + 1):
        f = k / steps
        rho = radius * f * f * (3.0 - 2.0 * f)
        sections.append((blend * f, gw - rho, 0.5 * gh - rho, rho))
    back_start = depth - radius
    if back_start > blend + 1e-4:
        sections.append((back_start, inner_x, inner_y, radius))
    for k in range(1, segments + 1):
        a = 0.5 * math.pi * k / segments
        sections.append((back_start + radius * math.sin(a), inner_x, inner_y, radius * math.cos(a)))
    return sections


def goal_section_points(t, sec):
    """The (x, y) points of a section: four corners (+x +y, -x +y, -x -y, +x -y), rampSegments + 1 points each (RlArena::GoalSectionPoint)."""
    segments = max(int(t["rampSegments"]), 1)
    _, ix, iy, rho = sec
    sx, sy = (1.0, -1.0, -1.0, 1.0), (1.0, 1.0, -1.0, -1.0)
    pts = []
    for corner in range(4):
        for step in range(segments + 1):
            a = 0.5 * math.pi * (corner + step / segments)
            pts.append((sx[corner] * ix + rho * math.cos(a), 0.5 * t["goalHeight"] + sy[corner] * iy + rho * math.sin(a)))
    return pts


def build_goal(t, team, atlas):
    sign = -1.0 if team == 0 else 1.0
    gw, gh, depth = t["goalHalfWidth"], t["goalHeight"], t["goalDepth"]
    z0 = sign * t["halfLength"]
    z1 = sign * (t["halfLength"] + depth)
    cavity = (0.0, gh * 0.5, 0.5 * (z0 + z1))
    net, metal, accent = atlas.uv("net"), atlas.uv("metal"), atlas.uv("accent")
    nu0, nv0, nu1, nv1 = net
    m = Mesh("arena_goal_" + ("blue" if team == 0 else "orange"))
    cell = 2.0  # metres of the net tile (2 x 2 diamonds)

    def face(p0, p1, p2, p3, a, b):
        # subdivide in 'a' and 'b' into whole tiles so that no UV leaves the net cell
        pts = [p0, p1, p2, p3]
        amin, amax = min(p[a] for p in pts), max(p[a] for p in pts)
        bmin, bmax = min(p[b] for p in pts), max(p[b] for p in pts)
        ia = int(math.floor(amin / cell + 1e-6))
        ib = int(math.floor(bmin / cell + 1e-6))
        ja = int(math.ceil(amax / cell - 1e-6))
        jb = int(math.ceil(bmax / cell - 1e-6))
        axis_n = 3 - a - b
        for i in range(ia, ja):
            for j in range(ib, jb):
                lo_a, hi_a = max(i * cell, amin), min((i + 1) * cell, amax)
                lo_b, hi_b = max(j * cell, bmin), min((j + 1) * cell, bmax)
                if hi_a - lo_a < 1e-6 or hi_b - lo_b < 1e-6:
                    continue

                def pt(va, vb):
                    p = [0.0, 0.0, 0.0]
                    p[a], p[b], p[axis_n] = va, vb, p0[axis_n]
                    return tuple(p)
                q = [pt(lo_a, lo_b), pt(hi_a, lo_b), pt(hi_a, hi_b), pt(lo_a, hi_b)]
                uvs = [(nu0 + (nu1 - nu0) * (qq[a] / cell - i), nv0 + (nv1 - nv0) * (qq[b] / cell - j)) for qq in q]
                m.quad(*q, uvs=uvs, outside=cavity)

    # the net: the sweep of the sections of the profile (the same triangles as the collision). The UV follow the surface: u is the length round the
    # section from the middle of the floor (so the mirror halves of the net are alike), v is the length of the surface along the goal (not the depth z: where
    # the radius grows the surface also moves inwards, and a net laid by z is stretched there). A long quad is cut into pieces of a tile, the UV of a piece
    # do not leave the tile of its middle.
    sections = goal_profile(t)
    pts = [goal_section_points(t, s) for s in sections]
    steps = max(int(t["rampSegments"]), 1) + 1
    count = len(pts[0])
    perimeter = []   # perimeter[i][j]: the length of the section i from the middle of the floor to the point j (j = count: back to the first point)
    for ring in pts:
        total, acc = [0.0], 0.0
        for k in range(1, count + 1):
            acc += math.hypot(ring[k % count][0] - ring[k - 1][0], ring[k % count][1] - ring[k - 1][1])
            total.append(acc)
        # the middle of the floor: between the last point of corner 2 and the first of corner 3
        mid_floor = 0.5 * (total[3 * steps - 1] + total[3 * steps])
        perimeter.append([s - mid_floor for s in total])
    depth_v = [[0.0] * (count + 1) for _ in sections]   # the length of the surface along the goal, per point
    for i in range(1, len(sections)):
        for j in range(count + 1):
            a3 = (pts[i - 1][j % count][0], pts[i - 1][j % count][1], sections[i - 1][0])
            b3 = (pts[i][j % count][0], pts[i][j % count][1], sections[i][0])
            depth_v[i][j] = depth_v[i - 1][j] + math.dist(a3, b3)

    def uv_of(s, v, tile):
        return (nu0 + (nu1 - nu0) * min(max(s / cell - tile[0], 0.0), 1.0), nv0 + (nv1 - nv0) * min(max(v / cell - tile[1], 0.0), 1.0))

    for i in range(len(sections) - 1):
        for j in range(count):
            jn = (j + 1) % count
            ring_a, ring_b = pts[i], pts[i + 1]
            # a vertex: (x, y, along, u, v)
            quad = [(ring_a[j], sections[i][0], perimeter[i][j], depth_v[i][j]), (ring_a[jn], sections[i][0], perimeter[i][j + 1], depth_v[i][j + 1]),
                    (ring_b[jn], sections[i + 1][0], perimeter[i + 1][j + 1], depth_v[i + 1][j + 1]), (ring_b[j], sections[i + 1][0], perimeter[i + 1][j], depth_v[i + 1][j])]
            span_s = max(abs(quad[1][2] - quad[0][2]), abs(quad[2][2] - quad[3][2]))
            span_v = max(abs(quad[3][3] - quad[0][3]), abs(quad[2][3] - quad[1][3]))
            if span_s < 1e-6 or span_v < 1e-6:
                continue
            na = max(1, int(math.ceil(span_s / cell - 1e-6)))
            nb = max(1, int(math.ceil(span_v / cell - 1e-6)))
            for ka in range(na):
                for kb in range(nb):
                    corners = []
                    for fa, fb in ((ka, kb), (ka + 1, kb), (ka + 1, kb + 1), (ka, kb + 1)):
                        u, v = fa / na, fb / nb

                        def mix2(f0, f1, f2, f3):
                            return f0 * (1 - u) * (1 - v) + f1 * u * (1 - v) + f2 * u * v + f3 * (1 - u) * v
                        x = mix2(quad[0][0][0], quad[1][0][0], quad[2][0][0], quad[3][0][0])
                        y = mix2(quad[0][0][1], quad[1][0][1], quad[2][0][1], quad[3][0][1])
                        along = mix2(quad[0][1], quad[1][1], quad[2][1], quad[3][1])
                        s = mix2(quad[0][2], quad[1][2], quad[2][2], quad[3][2])
                        w = mix2(quad[0][3], quad[1][3], quad[2][3], quad[3][3])
                        corners.append(((x, y, z0 + sign * along), s, w))
                    mid_s = sum(c[1] for c in corners) / 4.0
                    mid_v = sum(c[2] for c in corners) / 4.0
                    tile = (math.floor(mid_s / cell), math.floor(mid_v / cell))
                    q = [c[0] for c in corners]
                    area2 = (length(cross(vsub(q[1], q[0]), vsub(q[2], q[0]))) + length(cross(vsub(q[2], q[0]), vsub(q[3], q[0]))))
                    # a quad that has collapsed to a line (the section shrinks to nothing at the back) has no net
                    if area2 < 1e-8:
                        continue
                    m.quad(*q, uvs=[uv_of(c[1], c[2], tile) for c in corners], outside=cavity)
    # the flat part of the back wall: the inner rectangle of the last section
    last = sections[-1]
    cy = 0.5 * gh
    face((-last[1], cy - last[2], z1), (last[1], cy - last[2], z1), (last[1], cy + last[2], z1), (-last[1], cy + last[2], z1), 0, 1)
    # the frame: posts and crossbar on the goal line, a rail along each upper corner of the net, stays on the back wall
    r = 0.16
    for x in (-gw, gw):
        m.extend(cylinder((x, 0, z0), (x, gh, z0), r, r, 10, caps=(False, True), rect=metal))
    mid_step = max(int(t["rampSegments"]), 1) // 2
    segments = max(int(t["rampSegments"]), 1)
    for corner in (0, 1):
        rail = [(p[0], p[1], z0 + sign * s[0]) for s, p in ((s, goal_section_points(t, s)[corner * (segments + 1) + mid_step]) for s in sections)]
        for a, b in zip(rail, rail[1:]):
            if math.dist(a, b) > 1e-4:
                m.extend(cylinder(a, b, r * 0.8, r * 0.8, 10, caps=(True, True), rect=metal))
    m.extend(cylinder((-gw, gh, z0), (gw, gh, z0), r, r, 10, caps=(True, True), rect=accent))
    for x in (-last[1], last[1]):
        m.extend(cylinder((x, cy - last[2], z1 - sign * 0.2), (x, cy + last[2], z1 - sign * 0.2), r * 0.8, r * 0.8, 10, caps=(False, True), rect=metal))
    m.extend(cylinder((-last[1], cy + last[2], z1 - sign * 0.2), (last[1], cy + last[2], z1 - sign * 0.2), r * 0.8, r * 0.8, 10, caps=(True, True), rect=metal))
    m.smooth(45.0)
    return m


# --------------------------------------------------------------------------- pads
def build_pad(big, atlas):
    top = atlas.uv("big" if big else "small", 1.0)
    trim = atlas.uv("trim")
    r = 1.55 if big else 0.95
    m = Mesh("pad_big" if big else "pad_small")
    h = 0.07 if big else 0.05
    # the plate: a six sided dish with a chamfer, its top is the hexagon of the texture
    side = revolve([(r, 0.0), (r, h * 0.55), (r * 0.94, h)], 6, rect=trim, v_from_length=False)
    m.extend(side)
    m.extend(disc((0, h, 0), r * 0.94, (0, 1, 0), 6, top, rotation_deg=-90))
    if big:  # a raised ring around the top and a short pedestal for the orb
        m.extend(revolve([(0.34, h), (0.40, h + 0.20), (0.22, h + 0.30), (0.0, h + 0.30)], 12, rect=trim, v_from_length=False))
    return m


def build_orb(atlas):
    uv = atlas.uv("orb")
    m = uv_sphere((0, 0, 0), 0.62, 12, 8, rect=uv)
    m.name = "pad_orb"
    return m


def build(ctx):
    from .common import ROOT
    import json
    tuning = json.loads((ROOT / "assets" / "data" / "rl_tuning.json").read_text(encoding="utf-8"))["arena"]
    foot = Footprint(tuning)
    H = tuning["height"]
    centre = (0.0, H * 0.5, 0.0)

    ctx.texture("arena_floor", floor_texture(tuning))
    ctx.mesh("arena_floor", build_floor(tuning, foot), origin=centre, note="turf + markings, planar UV over 2 x halfWidth by 2 x halfLength")
    for team, name in ((0, "blue"), (1, "orange")):
        ctx.texture("arena_wall_" + name, wall_texture(name, tuning=tuning))
        ctx.mesh("arena_wall_" + name, build_walls(tuning, foot, team), origin=centre, part_of="arena",
                 note="floor ramps + opaque lower wall; u = perimeter / 8 m, v = profile arc length")
        fa = frame_texture(name)
        ctx.texture("arena_frame_" + name, fa.image())
        ctx.mesh("arena_frame_" + name, build_frame(tuning, foot, team, fa), origin=centre, part_of="arena",
                 note="open frame instead of transparent glass: posts, rails, ceiling ribs and beams")
        ga = goal_texture(name)
        ctx.texture("arena_goal_" + name, goal_image(ga, name))
        ctx.texture("arena_glass_" + name, glass_texture(name))
        ctx.mesh("arena_glass_" + name, build_glass(tuning, foot, team), origin=centre, part_of="arena",
                 note="translucent glass panels over the lower wall and the ceiling ramp (blend alpha)")
        gz = (-1.0 if team == 0 else 1.0) * (tuning["halfLength"] + tuning["goalDepth"] * 0.5)
        ctx.mesh("arena_goal_" + name, build_goal(tuning, team, ga), origin=(0.0, tuning["goalHeight"] * 0.5, gz), part_of="arena",
                 note="goal behind the end wall: net on the smooth profile of RlArena::GoalProfile, posts, crossbar, rails")
    ctx.texture("arena_glass_roof", glass_texture("roof"))
    ctx.mesh("arena_glass_roof", build_glass_roof(tuning, foot), origin=centre, part_of="arena", note="the glass of the ceiling, y = height")
    pa = pads_texture()
    ctx.texture("pads", pa.image())
    ctx.mesh("pad_small", build_pad(False, pa), origin=(0.0, 0.0, 0.0), part_of="pads", note="hexagon plate, origin on the floor")
    ctx.mesh("pad_big", build_pad(True, pa), origin=(0.0, 0.0, 0.0), part_of="pads", note="big plate with a ring, origin on the floor")
    ctx.material("pad_orb", "pads")  # the orb glows additively: its own material over the same texture
    ctx.mesh("pad_orb", build_orb(pa), origin=(0.0, 0.0, 0.0), part_of="pads", note="orb of a big pad, origin = its centre (float it ~1.6 m up)")
