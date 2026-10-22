"""The car: an original wedge hatchback "Pulse" in the spirit of the Octane class, built around the hitbox of
assets/data/rl_tuning.json. Frame: origin = the hitbox centre, +Z forward (the nose), +Y up, X to the right.

Parts (one mesh each, the team picks only the material):
  car_body    hull, cabin with glass, spoiler, splitter, skirts, mirrors, exhaust and the boost nozzle  (atlas car_<team>)
  car_lights  two headlights and two tail lights, a separate mesh so a later pass can make them glow     (car_lights)
  car_wheel   one wheel of the rear radius 0.15 m, axis along X; the front wheels use the entity scale x 0.8333  (car_wheel)
  car_boost   the boost flame cone, from the nozzle backwards (for an additive pass of G7b)               (car_boost)
"""

import math

from .common import (BLUE, BLUE_DARK, BLUE_GLOW, GRAPHITE, ORANGE, ORANGE_DARK, ORANGE_GLOW, STEEL, WHITE, Atlas, canvas,
                     mix, shade)
from .mesh import (FULL, Mesh, box, cylinder, disc, loft, revolve, rot_z, translate, Xf, rot_y, rot_x)

# --- constants of rl_tuning.json (cross-checked in build.py against the file)
HALF = (0.4335, 0.1933, 0.60254)
FRONT_WHEEL = {"z": 0.37374, "x": 0.259, "y": -0.26755, "r": 0.125}
REAR_WHEEL = {"z": -0.47626, "x": 0.295, "y": -0.25055, "r": 0.150}
WHEEL_WIDTH_RATIO = 1.13  # width / radius

TAIL_Z, NOSE_Z = -0.68, 0.70
BELLY = -0.27


def pl(table, z):
    """Piecewise linear lookup in [(z, value)]."""
    if z <= table[0][0]:
        return table[0][1]
    for (z0, v0), (z1, v1) in zip(table, table[1:]):
        if z <= z1:
            return v0 + (v1 - v0) * (z - z0) / (z1 - z0)
    return table[-1][1]


HW = [(-0.68, 0.37), (-0.62, 0.41), (-0.50, 0.44), (-0.10, 0.45), (0.20, 0.45), (0.50, 0.43), (0.64, 0.38), (0.70, 0.28)]
YTOP = [(-0.68, 0.085), (-0.63, 0.125), (-0.45, 0.135), (-0.10, 0.125), (0.20, 0.105), (0.50, 0.045), (0.64, -0.005), (0.70, -0.06)]
YBASE = [(-0.68, -0.22), (-0.60, BELLY), (0.55, BELLY), (0.70, -0.17)]


def belly(z):
    y = pl(YBASE, z)
    for w in (FRONT_WHEEL, REAR_WHEEL):
        radius = w["r"] + 0.04
        dz = z - w["z"]
        if abs(dz) < radius:
            y = max(y, w["y"] + math.sqrt(radius * radius - dz * dz) * 1.0)
    return y


HULL_STATIONS = [-0.68, -0.655, -0.62, -0.58, -0.54, -0.50, -0.465, -0.43, -0.39, -0.35, -0.31, -0.25, -0.15, -0.05, 0.05, 0.13,
                 0.19, 0.225, 0.26, 0.30, 0.34, 0.374, 0.41, 0.45, 0.49, 0.53, 0.57, 0.60, 0.635, 0.665, 0.70]


def hull_ring(z):
    hw, yt, yb = pl(HW, z), pl(YTOP, z), belly(z)
    h = yt - yb
    left = [(-0.55 * hw, yb), (-0.92 * hw, yb + 0.03), (-hw, yb + 0.25 * h), (-hw, yt - 0.30 * h), (-0.90 * hw, yt - 0.06),
            (-0.55 * hw, yt + 0.012), (0.0, yt + 0.02)]
    ring = [(0.0, yb)] + left + [(-p[0], p[1]) for p in reversed(left[:-1])] + [(0.0, yb)]
    return [(x, y, z) for (x, y) in ring]


CABIN = [  # z, y_base, hw_base, y_top, hw_top
    (0.24, 0.100, 0.39, 0.105, 0.37),
    (0.10, 0.115, 0.41, 0.205, 0.30),
    (-0.04, 0.125, 0.42, 0.238, 0.295),
    (-0.22, 0.128, 0.43, 0.235, 0.29),
    (-0.40, 0.130, 0.43, 0.185, 0.31),
    (-0.56, 0.135, 0.43, 0.140, 0.34),
]


def cabin_ring(s):
    z, yb, hwb, yt, hwt = s
    h = yt - yb
    left = [(-hwb, yb - 0.02), (-hwb * 0.985, yb + 0.30 * h), (-hwt, yt - 0.006), (-hwt * 0.5, yt + 0.01), (0.0, yt + 0.013)]
    ring = left + [(-p[0], p[1]) for p in reversed(left[:-1])]
    return [(x, y, z) for (x, y) in ring]


# --------------------------------------------------------------------------- texture
CELLS = {  # name: (x0, y0, x1, y1) in px of the 1024 atlas
    "paint": (0, 0, 720, 1024),
    "glass": (720, 0, 1024, 480),
    "trim": (720, 480, 1024, 736),
    "chrome": (720, 736, 1024, 1024),
}


def body_texture(team):
    main, dark, glow = (BLUE, BLUE_DARK, BLUE_GLOW) if team == "blue" else (ORANGE, ORANGE_DARK, ORANGE_GLOW)
    a = Atlas(1024, 1024, GRAPHITE)
    for n, r in CELLS.items():
        a.cell(n, *r)
    cv = a.cv
    x0, y0, x1, y1 = CELLS["paint"]
    w, h = x1 - x0, y1 - y0
    # paint: u runs round the hull (0.5 = the crown), v along the car (image top = nose)
    for i in range(w):
        u = i / (w - 1)
        side = abs(u - 0.5) * 2.0  # 0 on the crown, 1 at the belly centre line (under the car)
        k = 1.06 - 0.30 * side ** 1.6  # a lighter crown, darker flanks
        col = shade(main, k)
        if side > 0.86:
            col = mix(col, GRAPHITE, min(1.0, (side - 0.86) / 0.06))
        cv.rect(x0 + i, y0, x0 + i + 1, y1, fill=col)
    # racing stripes along the crown
    for off, wd in ((-0.045, 0.022), (0.045, 0.022)):
        cx = x0 + (0.5 + off) * w
        cv.rect(cx - wd * w, y0, cx + wd * w, y1, fill=WHITE)
    # the hood vent and the headlight bands on the nose, the rear bumper strip
    cv.rect(x0 + 0.42 * w, y0 + 0.20 * h, x0 + 0.58 * w, y0 + 0.30 * h, fill=GRAPHITE)
    cv.rect(x0, y0, x1, y0 + 0.055 * h, fill=GRAPHITE)  # the nose end
    cv.rect(x0, y1 - 0.045 * h, x1, y1, fill=GRAPHITE)  # the tail end
    # a team coloured accent line on both shoulders and a wide side slash in the dark team tone
    for u in (0.29, 0.71):
        cv.line([(x0 + u * w, y0 + 0.06 * h), (x0 + u * w, y1 - 0.06 * h)], glow, 5)
    for u in (0.20, 0.80):
        cv.poly([(x0 + (u - 0.035) * w, y0 + 0.26 * h), (x0 + (u + 0.035) * w, y0 + 0.30 * h),
                 (x0 + (u + 0.035) * w, y0 + 0.62 * h), (x0 + (u - 0.035) * w, y0 + 0.58 * h)], fill=dark)
        cv.rect(x0 + (u - 0.012) * w, y0 + 0.30 * h, x0 + (u + 0.012) * w, y0 + 0.58 * h, fill=glow)
    # glass: a dark blue pane with a soft sky reflection, framed (the frame is paint) on every side
    gx0, gy0, gx1, gy1 = CELLS["glass"]
    gw, gh = gx1 - gx0, gy1 - gy0
    cv.vgrad(gx0, gy0, gx1, gy1, (36, 52, 78), (10, 14, 24))
    cv.poly([(gx0 + 0.55 * gw, gy0), (gx0 + 0.8 * gw, gy0), (gx0 + 0.45 * gw, gy1), (gx0 + 0.2 * gw, gy1)], fill=(52, 72, 104))
    frame = 0.07
    cv.rect(gx0, gy0, gx1, gy0 + frame * gh, fill=shade(main, 0.8))
    cv.rect(gx0, gy1 - frame * gh, gx1, gy1, fill=shade(main, 0.8))
    cv.rect(gx0, gy0, gx0 + frame * gw, gy1, fill=shade(main, 0.8))
    cv.rect(gx1 - frame * gw, gy0, gx1, gy1, fill=shade(main, 0.8))
    # trim: graphite plastic with a soft highlight
    tx0, ty0, tx1, ty1 = CELLS["trim"]
    cv.vgrad(tx0, ty0, tx1, ty1, (60, 66, 78), (26, 29, 36))
    # chrome: a bright band structure (a baked studio reflection)
    cx0, cy0, cx1, cy1 = CELLS["chrome"]
    stops = [(235, 240, 248), (120, 130, 148), (200, 208, 222), (70, 78, 94), (180, 188, 204)]
    bh = (cy1 - cy0) / len(stops)
    for i, c in enumerate(stops):
        cv.rect(cx0, cy0 + i * bh, cx1, cy0 + (i + 1) * bh + 1, fill=c)
    return a


def lights_texture():
    """64x32 px: the headlight (left half) and the tail light (right half)."""
    cv = canvas(128, 64, (0, 0, 0))
    cv.vgrad(0, 0, 64, 64, (255, 252, 236), (255, 222, 150))
    cv.rect(0, 0, 64, 6, fill=(255, 255, 255))
    cv.vgrad(64, 0, 128, 64, (255, 70, 56), (150, 14, 14))
    cv.rect(64, 26, 128, 36, fill=(255, 150, 130))
    return cv.finish()


def wheel_texture():
    """512x256: the tyre (left square: u around the wheel, v across the tread) and the rim face (right square: the disc)."""
    cv = canvas(512, 256, (20, 20, 22))
    cv.vgrad(0, 0, 256, 256, (30, 30, 34), (30, 30, 34))
    # tread blocks and a centre rib
    for k in range(32):
        x = k * 8
        cv.rect(x, 0, x + 4, 256, fill=(44, 44, 50))
    cv.rect(0, 120, 256, 136, fill=(18, 18, 20))
    for y in (50, 205):
        cv.rect(0, y, 256, y + 4, fill=(18, 18, 20))
    # the rim face: a 5 spoke alloy in front of a dark well
    cx, cy, r = 384, 128, 124
    cv.ellipse(cx, cy, r, r, fill=(24, 25, 29))
    cv.ellipse(cx, cy, r * 0.97, r * 0.97, fill=(150, 156, 168))
    cv.ellipse(cx, cy, r * 0.90, r * 0.90, fill=(22, 24, 28))
    for k in range(5):
        a = math.radians(90 + 72 * k)
        pts = []
        for da, rad in ((-0.19, r * 0.88), (-0.07, r * 0.20), (0.07, r * 0.20), (0.19, r * 0.88)):
            pts.append((cx + math.cos(a + da) * rad, cy + math.sin(a + da) * rad))
        cv.poly(pts, fill=(176, 183, 196))
    cv.ellipse(cx, cy, r * 0.24, r * 0.24, fill=(205, 211, 222))
    cv.ellipse(cx, cy, r * 0.10, r * 0.10, fill=(70, 76, 88))
    return cv.finish()


def boost_texture():
    """The flame: v runs from the nozzle (image bottom, white hot) to the tip (image top, red)."""
    cv = canvas(32, 128, (255, 120, 20))
    stops = [(255, 252, 214), (255, 214, 96), (255, 140, 24), (214, 60, 10)]
    n = len(stops) - 1
    for i in range(128):
        t = i / 127.0  # 0 = top = tip
        j = min(n - 1, int((1 - t) * n))
        c = mix(stops[j], stops[j + 1], (1 - t) * n - j)
        cv.rect(0, i, 32, i + 1, fill=c)
    return cv.finish()


# --------------------------------------------------------------------------- meshes
def build_body(atlas):
    uv = atlas.uv
    body = Mesh("car_body")
    # hull
    rings = [hull_ring(z) for z in HULL_STATIONS]
    hull = loft(rings, rect=uv("paint"), closed=False, caps=(True, True), smooth_deg=50.0)
    body.extend(hull)
    # cabin
    cabin_rings = [cabin_ring(s) for s in CABIN]
    glass, paint = uv("glass"), uv("paint")
    pw, ph = paint[2] - paint[0], paint[3] - paint[1]
    paint_roof = (paint[0] + pw * 0.480, paint[1] + ph * 0.40, paint[0] + pw * 0.520, paint[1] + ph * 0.60)  # solid team colour
    last = len(CABIN) - 2
    seg_top = (2, 3, 4, 5)  # the segments over the roof line (ring points 2..6)

    def cell(i, j):
        pts = cabin_rings[i]
        if i in (0, last) and j in seg_top:
            ax, bx = pts[j][0], pts[j + 1][0]
            lo, hi = pts[2][0], pts[6][0]
            f0, f1 = (ax - lo) / (hi - lo), (bx - lo) / (hi - lo)
            return (glass[0] + (glass[2] - glass[0]) * f0, glass[1], glass[0] + (glass[2] - glass[0]) * f1, glass[3])
        if 1 <= i < last and j in (1, 6):  # a side pane: the quad's u runs up the window, v along the car (rear wards)
            gh = glass[3] - glass[1]
            f0, f1 = (i - 1) / (last - 1), i / (last - 1)
            return (glass[0], glass[1] + gh * f0, glass[2], glass[1] + gh * f1)
        return paint_roof
    cabin = loft(cabin_rings, rect=paint, closed=False, cell_fn=cell, smooth_deg=40.0)
    body.extend(cabin)
    # spoiler: a wing on two struts
    trim, chrome = uv("trim"), uv("chrome")
    body.extend(box((0.0, 0.222, -0.60), (0.80, 0.014, 0.17), rect=paint_roof).transformed(translate(0, 0, 0)))
    for sx in (-0.24, 0.24):
        body.extend(box((sx, 0.17, -0.60), (0.03, 0.095, 0.05), rect=trim))
    for sx in (-0.405, 0.405):  # end plates
        body.extend(box((sx, 0.228, -0.60), (0.012, 0.05, 0.19), rect=paint_roof))
    # front splitter and side skirts
    body.extend(box((0.0, -0.235, 0.655), (0.70, 0.012, 0.09), rect=trim))
    for sx in (-0.447, 0.447):
        body.extend(box((sx, -0.225, -0.075), (0.03, 0.05, 0.36), rect=trim))
    # mirrors
    for sx in (-0.45, 0.45):
        body.extend(box((sx, 0.158, 0.13), (0.075, 0.034, 0.05), rect=paint_roof))
    # exhausts and the boost nozzle
    for sx in (-0.15, 0.15):
        body.extend(cylinder((sx, -0.115, -0.64), (sx, -0.115, -0.72), 0.034, 0.042, 8, rect=chrome))
    nozzle = cylinder((0.0, -0.06, -0.64), (0.0, -0.06, -0.74), 0.052, 0.074, 10, rect=trim)
    body.extend(nozzle)
    return body


def build_lights(light_uv):
    head = (light_uv[0], light_uv[1], (light_uv[0] + light_uv[2]) / 2, light_uv[3])
    tail = ((light_uv[0] + light_uv[2]) / 2, light_uv[1], light_uv[2], light_uv[3])
    m = Mesh("car_lights")
    for sx in (-1, 1):
        m.extend(box((sx * 0.265, -0.052, 0.652), (0.17, 0.04, 0.07), rect=head))
        m.extend(box((sx * 0.30, 0.075, -0.667), (0.20, 0.036, 0.04), rect=tail))
    m.extend(box((0.0, 0.07, -0.667), (0.20, 0.014, 0.04), rect=tail))
    return m


def build_wheel(seg=20):
    """A wheel of the rear radius 0.15 m with the rim face dished into the tyre; axis along X."""
    wt = 0.5 * WHEEL_WIDTH_RATIO
    tyre_rect = (0.0, 0.0, 0.5, 1.0)
    rim_rect = (0.5, 0.0, 1.0, 1.0)
    profile = [(0.62, -wt), (0.80, -wt - 0.0), (0.93, -wt * 0.92), (1.0, -wt * 0.55), (1.0, wt * 0.55), (0.93, wt * 0.92),
               (0.80, wt), (0.62, wt)]
    m = revolve(profile, seg, rect=tyre_rect, v_from_length=False)
    for sy in (-1, 1):
        m.extend(disc((0, sy * wt * 0.90, 0), 0.62, (0, sy, 0), seg, rim_rect))
    # the axis is Y here: lay it along X (+X outward for the right wheel; both sides show the same disc)
    return m.transformed(Xf(rot_z(-90).r) * Xf(((0.15, 0, 0), (0, 0.15, 0), (0, 0, 0.15))))  # metres: the rear wheel radius


def build_boost(seg=8):
    m = Mesh("car_boost")
    # a cone along -Z from the nozzle (z = 0) to the tip at z = -0.9; v = 1 at the nozzle
    for s in range(seg):
        a0 = 2 * math.pi * s / seg
        a1 = 2 * math.pi * (s + 1) / seg
        rings = [(0.0, 0.07), (-0.30, 0.058), (-0.60, 0.032), (-0.90, 0.0)]
        for (z0, r0), (z1, r1) in zip(rings, rings[1:]):
            v0 = 1.0 - (-z0) / 0.9
            v1 = 1.0 - (-z1) / 0.9
            p = [(r0 * math.cos(a0), r0 * math.sin(a0), z0), (r0 * math.cos(a1), r0 * math.sin(a1), z0),
                 (r1 * math.cos(a1), r1 * math.sin(a1), z1), (r1 * math.cos(a0), r1 * math.sin(a0), z1)]
            u0, u1 = s / seg, (s + 1) / seg
            m.quad(p[0], p[1], p[2], p[3], ((u0, v0), (u1, v0), (u1, v1), (u0, v1)), inside=(0.0, 0.0, z0 * 0.5))
    return m


def check_tuning():
    """The car is modelled around the numbers above: they must still be those of assets/data/rl_tuning.json."""
    import json
    from .common import ROOT
    car = json.loads((ROOT / "assets" / "data" / "rl_tuning.json").read_text(encoding="utf-8"))["car"]
    want = {"halfExtentX": HALF[0], "halfExtentY": HALF[1], "halfExtentZ": HALF[2], "wheelFrontZ": FRONT_WHEEL["z"],
            "wheelRearZ": REAR_WHEEL["z"], "wheelMountXFront": FRONT_WHEEL["x"], "wheelMountXRear": REAR_WHEEL["x"],
            "wheelRadiusFront": FRONT_WHEEL["r"], "wheelRadiusRear": REAR_WHEEL["r"], "wheelMountYOffset": 0.0,
            "suspensionRestFront": -FRONT_WHEEL["y"], "suspensionRestRear": -REAR_WHEEL["y"]}
    bad = ["%s: file %s, generator %s" % (k, car.get(k), v) for k, v in want.items() if abs(car.get(k, 1e9) - v) > 1e-4]
    if bad:
        raise SystemExit("rl_tuning.json car section changed, update tools/rl_assets/car.py:\n  " + "\n  ".join(bad))


def build(ctx):
    """Pulse keeps its names (car_body, car_lights, car_blue, car_orange), the other five cars of cars.py are car_<key>_body ... car_<key>_<team>."""
    check_tuning()
    from . import cars
    for d in cars.DESIGNS:
        prefix = "car" if d.key == "pulse" else "car_" + d.key
        body = None
        for team in ("blue", "orange"):
            atlas = cars.body_texture(team, d)
            ctx.texture("%s_%s" % (prefix, team), atlas.image())
            if team == "blue":
                body = cars.build_body(atlas, d)  # the UV rects are the same for both teams
        info = ctx.mesh(prefix + "_body", body, origin=(0, 0, 0), part_of="car", note="hull + cabin + spoiler + nozzle, one atlas" if d.key == "pulse" else "%s: hull + cabin + details, one atlas" % d.name)
        lights = ctx.mesh(prefix + "_lights", cars.build_lights(d), origin=(0, 0, 0), part_of="car", note="head and tail lights" if d.key == "pulse" else "%s: head and tail lights" % d.name)
        del info, lights
    ctx.texture("car_lights", lights_texture())
    ctx.texture("car_wheel", wheel_texture())
    ctx.mesh("car_wheel", build_wheel(), origin=(0, 0, 0), unit_scale=None, part_of="car",
             note="the rear wheel (r 0.15 m); the front wheels (r 0.125 m) use the entity scale x 0.8333")
    ctx.texture("car_boost", boost_texture())
    ctx.mesh("car_boost", build_boost(), origin=(0, 0, 0), part_of="car", note="flame cone behind the nozzle (additive in G7b)")
    from . import cardata
    n = cardata.write_data(ctx.meshes)
    print("  cars     %d data files assets/data/cars/*.json" % n)
    try:
        n = cardata.write_previews(ctx.meshes)
        print("  cars     %d profile previews assets/ui/cars/*.png" % n)
    except ImportError as error:
        print("  cars     previews skipped (%s)" % error)
