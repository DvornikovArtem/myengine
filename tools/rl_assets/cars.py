"""The six cars of the garage (docs/game/rl-cars.md): original bodies built around the six hitboxes of Rocket League, with their numbers from the
car configs of RocketSim (https://github.com/ZealanL/RocketSim, MIT, Copyright (c) 2022 ZealanL; src/Sim/Car/CarConfig/CarConfig.cpp, commit
c2baacb8f4b441dd8505e63c2aeb5a1679b60b02). Every car has its own mesh, lights, texture atlas for the two teams and a profile preview; the wheels and
the flame of the boost are shared (the wheel mesh is scaled by the radius of the car). Frame of every mesh: origin = the centre of the hitbox,
+Z forward (the nose), +Y up, X to the right, as in car.py.

    key      name      class     hitbox of RL
    pulse    Pulse     Balanced  Octane      the wedge hatchback of car.py (kept as it was)
    vector   Vector    Long      Dominus     a low long coupe with a flat nose
    slate    Slate     Flat      Plank       flat and wide, a shovel nose and a big wing
    dart     Dart      Low-long  Breakout    a long narrow speedster with a tail fin
    fusion   Fusion    Hybrid    Hybrid      a coupe between Pulse and Vector
    bulwark  Bulwark   Tall      Merc        a tall boxy wagon with a roof rack
"""

import math
from dataclasses import dataclass, field

from .common import (BLUE, BLUE_DARK, BLUE_GLOW, GRAPHITE, ORANGE, ORANGE_DARK, ORANGE_GLOW, STEEL, WHITE, Atlas, mix, shade)
from .mesh import Mesh, box, cylinder, loft, translate, Xf, rot_x, rot_y, rot_z

UU = 0.01  # RocketSim units are Unreal units: 1 uu = 1 cm

# --------------------------------------------------------------------------- RocketSim hitbox classes
# size (length, width, height), the offset of the hitbox from the centre of mass (forward, 0, up), the wheels: radius, relaxed suspension length,
# the connection point (forward, right, up) from the centre of mass. All in uu. The order is that of CarConfig.cpp.
ROCKETSIM = {
    "octane": dict(size=(120.507, 86.6994, 38.6591), offset=(13.8757, 0.0, 20.755),
                   front=dict(radius=12.50, rest=38.755, conn=(51.25, 25.90, 20.755)), rear=dict(radius=15.00, rest=37.055, conn=(-33.75, 29.50, 20.755))),
    "dominus": dict(size=(130.427, 85.7799, 33.8), offset=(9.0, 0.0, 15.75),
                    front=dict(radius=12.00, rest=33.95, conn=(50.30, 31.10, 15.75)), rear=dict(radius=13.50, rest=33.85, conn=(-34.75, 33.00, 15.75))),
    "plank": dict(size=(131.32, 87.1704, 31.8944), offset=(9.00857, 0.0, 12.0942),
                  front=dict(radius=12.50, rest=31.9242, conn=(49.97, 27.80, 12.0942)), rear=dict(radius=17.00, rest=27.9242, conn=(-35.43, 20.28, 12.0942))),
    "breakout": dict(size=(133.992, 83.021, 32.8), offset=(12.5, 0.0, 11.75),
                     front=dict(radius=13.50, rest=29.7, conn=(51.50, 26.67, 11.75)), rear=dict(radius=15.00, rest=29.666, conn=(-35.75, 35.00, 11.75))),
    "hybrid": dict(size=(129.519, 84.6879, 36.6591), offset=(13.8757, 0.0, 20.755),
                   front=dict(radius=12.50, rest=38.755, conn=(51.25, 25.90, 20.755)), rear=dict(radius=15.00, rest=37.055, conn=(-34.00, 29.50, 20.755))),
    "merc": dict(size=(123.22, 79.2103, 44.1591), offset=(11.3757, 0.0, 21.505),
                 front=dict(radius=15.00, rest=39.505, conn=(51.25, 25.90, 21.505)), rear=dict(radius=15.00, rest=39.105, conn=(-33.75, 29.50, 21.505))),
}
TRAVEL = 12.0  # uu: RocketSim's rest length is the relaxed spring, ours is rest - the 12 uu of travel (rl_tuning.json: suspensionRestFront 0.26755 = 38.755 - 12)


def hitbox_numbers(hitbox_class):
    """The numbers of the car section of rl_tuning.json for a hitbox class, in the axes and units of the game (m; X right, Y up, Z forward)."""
    c = ROCKETSIM[hitbox_class]
    length, width, height = c["size"]
    ox, _, oz = c["offset"]
    out = {
        "halfExtentX": width / 2 * UU, "halfExtentY": height / 2 * UU, "halfExtentZ": length / 2 * UU,
        "comOffsetY": -oz * UU, "comOffsetZ": -ox * UU,
        "wheelMountXFront": c["front"]["conn"][1] * UU, "wheelMountXRear": c["rear"]["conn"][1] * UU,
        "wheelFrontZ": (c["front"]["conn"][0] - ox) * UU, "wheelRearZ": (c["rear"]["conn"][0] - ox) * UU,
        "wheelMountYOffset": (c["front"]["conn"][2] - oz) * UU,
        "wheelRadiusFront": c["front"]["radius"] * UU, "wheelRadiusRear": c["rear"]["radius"] * UU,
        "suspensionRestFront": (c["front"]["rest"] - TRAVEL) * UU, "suspensionRestRear": (c["rear"]["rest"] - TRAVEL) * UU,
    }
    return {k: round(v, 5) for k, v in out.items()}


def wheel_specs(hitbox_class):
    n = hitbox_numbers(hitbox_class)
    return (dict(z=n["wheelFrontZ"], x=n["wheelMountXFront"], y=-n["suspensionRestFront"], r=n["wheelRadiusFront"]),
            dict(z=n["wheelRearZ"], x=n["wheelMountXRear"], y=-n["suspensionRestRear"], r=n["wheelRadiusRear"]))


def pl(table, z):
    """Piecewise linear lookup in [(z, value)]."""
    if z <= table[0][0]:
        return table[0][1]
    for (z0, v0), (z1, v1) in zip(table, table[1:]):
        if z <= z1:
            return v0 + (v1 - v0) * (z - z0) / (z1 - z0)
    return table[-1][1]


CELLS = {  # name: (x0, y0, x1, y1) in px of the 1024 atlas: the same layout for every car
    "paint": (0, 0, 720, 1024),
    "glass": (720, 0, 1024, 480),
    "trim": (720, 480, 1024, 736),
    "chrome": (720, 736, 1024, 1024),
}


@dataclass
class Design:
    key: str
    name: str
    klass: str                # the class shown in the UI
    hitbox: str               # a key of ROCKETSIM
    hitbox_label: str         # "Octane": the note "Hitbox ≈ Octane"
    description: str
    hw: list                  # (z, half width of the hull)
    ytop: list                # (z, y of the top of the hull)
    ybase: list               # (z, y of the underside) before the wheel arches
    cabin: list               # (z, y_base, half width base, y_top, half width top), six sections from the nose to the tail
    livery: str
    details: object = None    # function(body, uv, design, rects) adding the spoiler, the splitter, the mirrors ...
    nose: float = 0.70
    tail: float = -0.68
    nozzle_y: float = -0.06
    head: tuple = (0.265, -0.052, 0.652, 0.17, 0.04, 0.07)      # lights: x, y, z, width, height, depth of one headlight
    tails: tuple = (0.30, 0.075, -0.667, 0.20, 0.036, 0.04)     # x, y, z, width, height, depth of one tail light
    tail_bar: tuple = (0.07, 0.20)                              # y and width of the centre tail light bar; None = no bar
    arch_gap: float = 0.04
    stations: list = field(default_factory=list)

    def wheels(self):
        return wheel_specs(self.hitbox)

    def belly(self, z):
        y = pl(self.ybase, z)
        for w in self.wheels():
            radius = w["r"] + self.arch_gap
            dz = z - w["z"]
            if abs(dz) < radius:
                y = max(y, w["y"] + math.sqrt(radius * radius - dz * dz))
        return y

    def hull_ring(self, z):
        hw, yt, yb = pl(self.hw, z), pl(self.ytop, z), self.belly(z)
        h = yt - yb
        left = [(-0.55 * hw, yb), (-0.92 * hw, yb + 0.03), (-hw, yb + 0.25 * h), (-hw, yt - 0.30 * h), (-0.90 * hw, yt - 0.06),
                (-0.55 * hw, yt + 0.012), (0.0, yt + 0.02)]
        ring = [(0.0, yb)] + left + [(-p[0], p[1]) for p in reversed(left[:-1])] + [(0.0, yb)]
        return [(x, y, z) for (x, y) in ring]

    def hull_stations(self):
        if self.stations:
            return self.stations
        n = 31
        zs = [self.tail + (self.nose - self.tail) * i / (n - 1) for i in range(n)]
        # denser at the wheels (the arches) and at both ends
        extra = []
        for w in self.wheels():
            extra += [w["z"] + d for d in (-0.16, -0.10, -0.05, 0.0, 0.05, 0.10, 0.16)]
        zs = sorted(set(round(z, 4) for z in zs + [e for e in extra if self.tail < e < self.nose]))
        return zs


def cabin_ring(s):
    z, yb, hwb, yt, hwt = s
    h = yt - yb
    left = [(-hwb, yb - 0.02), (-hwb * 0.985, yb + 0.30 * h), (-hwt, yt - 0.006), (-hwt * 0.5, yt + 0.01), (0.0, yt + 0.013)]
    ring = left + [(-p[0], p[1]) for p in reversed(left[:-1])]
    return [(x, y, z) for (x, y) in ring]


# --------------------------------------------------------------------------- the details of every car
def _exhausts(body, rects, xs, y, z0, z1, r0=0.034, r1=0.042):
    for sx in xs:
        body.extend(cylinder((sx, y, z0), (sx, y, z1), r0, r1, 8, rect=rects["chrome"]))


def _nozzle(body, rects, d):
    z = d.tail + 0.04
    body.extend(cylinder((0.0, d.nozzle_y, z), (0.0, d.nozzle_y, z - 0.10), 0.052, 0.074, 10, rect=rects["trim"]))


def _mirrors(body, rects, x, y, z, size=(0.075, 0.034, 0.05)):
    for sx in (-x, x):
        body.extend(box((sx, y, z), size, rect=rects["roof"]))


def details_pulse(body, uv, d, rects):
    trim, chrome, paint_roof = rects["trim"], rects["chrome"], rects["roof"]
    body.extend(box((0.0, 0.222, -0.60), (0.80, 0.014, 0.17), rect=paint_roof))
    for sx in (-0.24, 0.24):
        body.extend(box((sx, 0.17, -0.60), (0.03, 0.095, 0.05), rect=trim))
    for sx in (-0.405, 0.405):
        body.extend(box((sx, 0.228, -0.60), (0.012, 0.05, 0.19), rect=paint_roof))
    body.extend(box((0.0, -0.235, 0.655), (0.70, 0.012, 0.09), rect=trim))
    for sx in (-0.447, 0.447):
        body.extend(box((sx, -0.225, -0.075), (0.03, 0.05, 0.36), rect=trim))
    _mirrors(body, rects, 0.45, 0.158, 0.13)
    _exhausts(body, rects, (-0.15, 0.15), -0.115, -0.64, -0.72)
    body.extend(cylinder((0.0, -0.06, -0.64), (0.0, -0.06, -0.74), 0.052, 0.074, 10, rect=trim))


def details_vector(body, uv, d, rects):
    trim, paint_roof = rects["trim"], rects["roof"]
    # a ducktail lip on the deck, a wide flat splitter, a bonnet scoop, skirts, two side-pipe exhausts
    body.extend(box((0.0, 0.118, -0.675), (0.74, 0.020, 0.10), rect=paint_roof))
    body.extend(box((0.0, -0.200, 0.730), (0.82, 0.012, 0.11), rect=trim))
    body.extend(box((0.0, 0.087, 0.36), (0.20, 0.034, 0.30), rect=trim))
    for sx in (-0.448, 0.448):
        body.extend(box((sx, -0.190, -0.06), (0.028, 0.045, 0.52), rect=trim))
    for sx in (-0.30, 0.30):
        body.extend(box((sx, 0.015, 0.640), (0.20, 0.018, 0.05), rect=trim))  # a grille bar on each side of the nose
    _mirrors(body, rects, 0.445, 0.138, 0.05, (0.08, 0.030, 0.05))
    _exhausts(body, rects, (-0.20, 0.20), -0.12, -0.70, -0.78, 0.030, 0.040)
    _nozzle(body, rects, d)


def details_slate(body, uv, d, rects):
    trim, chrome, paint_roof = rects["trim"], rects["chrome"], rects["roof"]
    # a very wide low wing on short pylons, the shovel: a flat blade out of the nose, fender flares over the arches
    body.extend(box((0.0, 0.185, -0.675), (0.96, 0.014, 0.19), rect=paint_roof))
    for sx in (-0.30, 0.30):
        body.extend(box((sx, 0.135, -0.66), (0.030, 0.100, 0.06), rect=trim))
    for sx in (-0.475, 0.475):
        body.extend(box((sx, 0.188, -0.675), (0.012, 0.05, 0.20), rect=paint_roof))
    body.extend(box((0.0, -0.150, 0.785), (0.90, 0.014, 0.17), rect=trim))   # the blade
    for sx in (-1, 1):
        for w in d.wheels():
            body.extend(box((sx * (0.455), w["y"] + w["r"] + 0.05, w["z"]), (0.045, 0.020, 2 * w["r"] + 0.14), rect=paint_roof))
    for sx in (-0.462, 0.462):
        body.extend(box((sx, -0.145, -0.04), (0.026, 0.040, 0.46), rect=trim))
    _mirrors(body, rects, 0.45, 0.115, 0.08, (0.07, 0.028, 0.045))
    _exhausts(body, rects, (-0.12, 0.12), -0.085, -0.72, -0.80, 0.030, 0.038)
    _nozzle(body, rects, d)


def details_dart(body, uv, d, rects):
    trim, chrome, paint_roof = rects["trim"], rects["chrome"], rects["roof"]
    # a tail fin down the middle, canards on the nose, a long flat splitter, twin exhausts
    body.extend(box((0.0, 0.185, -0.58), (0.016, 0.115, 0.34), rect=paint_roof))
    body.extend(box((0.0, 0.255, -0.62), (0.020, 0.025, 0.26), rect=trim))
    for sx in (-0.17, 0.17):
        body.extend(box((sx, -0.060, 0.70), (0.13, 0.010, 0.07), rect=trim))   # canards
    body.extend(box((0.0, -0.205, 0.735), (0.60, 0.012, 0.11), rect=trim))
    for sx in (-0.405, 0.405):
        body.extend(box((sx, -0.205, -0.05), (0.026, 0.040, 0.50), rect=trim))
    _mirrors(body, rects, 0.395, 0.150, -0.02, (0.07, 0.028, 0.045))
    _exhausts(body, rects, (-0.12, 0.12), -0.13, -0.72, -0.80, 0.030, 0.040)
    _nozzle(body, rects, d)


def details_fusion(body, uv, d, rects):
    trim, chrome, paint_roof = rects["trim"], rects["chrome"], rects["roof"]
    # a lip spoiler on the boot, vents on the bonnet, skirts, mirrors, one wide exhaust
    body.extend(box((0.0, 0.158, -0.65), (0.68, 0.016, 0.10), rect=paint_roof))
    for sx in (-0.15, 0.15):
        body.extend(box((sx, 0.123, -0.66), (0.025, 0.060, 0.04), rect=trim))
    for sx in (-0.13, 0.13):
        body.extend(box((sx, 0.105, 0.38), (0.12, 0.012, 0.22), rect=trim))   # bonnet vents
    body.extend(box((0.0, -0.245, 0.715), (0.72, 0.012, 0.09), rect=trim))
    for sx in (-0.447, 0.447):
        body.extend(box((sx, -0.235, -0.05), (0.030, 0.050, 0.42), rect=trim))
    _mirrors(body, rects, 0.448, 0.172, 0.12)
    _exhausts(body, rects, (-0.17, 0.17), -0.125, -0.67, -0.75)
    _nozzle(body, rects, d)


def details_bulwark(body, uv, d, rects):
    trim, chrome, paint_roof = rects["trim"], rects["chrome"], rects["roof"]
    # a roof rack, a bull bar and a bumper step, heavy arches, big mirrors
    for sx in (-0.30, 0.30):
        body.extend(box((sx, 0.330, -0.10), (0.025, 0.020, 0.66), rect=trim))
    for z in (-0.38, -0.12, 0.14):
        body.extend(box((0.0, 0.335, z), (0.62, 0.016, 0.03), rect=trim))
    for sx in (-0.30, 0.30):
        body.extend(box((sx, 0.318, -0.38), (0.025, 0.030, 0.025), rect=trim))
        body.extend(box((sx, 0.318, 0.14), (0.025, 0.030, 0.025), rect=trim))
    body.extend(box((0.0, -0.190, 0.735), (0.74, 0.030, 0.06), rect=trim))      # the bull bar
    for sx in (-0.30, 0.30):
        body.extend(box((sx, -0.100, 0.728), (0.026, 0.14, 0.03), rect=trim))
    body.extend(box((0.0, -0.205, -0.70), (0.76, 0.040, 0.08), rect=trim))      # the rear bumper step
    for sx in (-1, 1):
        for w in d.wheels():
            body.extend(box((sx * 0.405, w["y"] + w["r"] + 0.06, w["z"]), (0.040, 0.026, 2 * w["r"] + 0.16), rect=trim))
    for sx in (-0.415, 0.415):
        body.extend(box((sx, -0.235, -0.03), (0.03, 0.05, 0.58), rect=trim))
    _mirrors(body, rects, 0.43, 0.215, 0.24, (0.085, 0.060, 0.055))
    _exhausts(body, rects, (-0.22, 0.22), -0.20, -0.72, -0.79, 0.034, 0.044)
    _nozzle(body, rects, d)


# --------------------------------------------------------------------------- the six designs
PULSE_HW = [(-0.68, 0.37), (-0.62, 0.41), (-0.50, 0.44), (-0.10, 0.45), (0.20, 0.45), (0.50, 0.43), (0.64, 0.38), (0.70, 0.28)]
PULSE_YTOP = [(-0.68, 0.085), (-0.63, 0.125), (-0.45, 0.135), (-0.10, 0.125), (0.20, 0.105), (0.50, 0.045), (0.64, -0.005), (0.70, -0.06)]
PULSE_BELLY = -0.27
PULSE_YBASE = [(-0.68, -0.22), (-0.60, PULSE_BELLY), (0.55, PULSE_BELLY), (0.70, -0.17)]
PULSE_STATIONS = [-0.68, -0.655, -0.62, -0.58, -0.54, -0.50, -0.465, -0.43, -0.39, -0.35, -0.31, -0.25, -0.15, -0.05, 0.05, 0.13,
                  0.19, 0.225, 0.26, 0.30, 0.34, 0.374, 0.41, 0.45, 0.49, 0.53, 0.57, 0.60, 0.635, 0.665, 0.70]
PULSE_CABIN = [(0.24, 0.100, 0.39, 0.105, 0.37), (0.10, 0.115, 0.41, 0.205, 0.30), (-0.04, 0.125, 0.42, 0.238, 0.295),
               (-0.22, 0.128, 0.43, 0.235, 0.29), (-0.40, 0.130, 0.43, 0.185, 0.31), (-0.56, 0.135, 0.43, 0.140, 0.34)]

DESIGNS = [
    Design("pulse", "Pulse", "Balanced", "octane", "Octane", "The all-rounder. Easy to fly, easy to read.",
           PULSE_HW, PULSE_YTOP, PULSE_YBASE, PULSE_CABIN, "pulse", details_pulse, stations=PULSE_STATIONS),
    Design("vector", "Vector", "Long", "dominus", "Dominus", "Long and low. Hits hard off the flat nose.",
           hw=[(-0.72, 0.34), (-0.66, 0.39), (-0.52, 0.435), (-0.15, 0.445), (0.25, 0.445), (0.55, 0.42), (0.70, 0.36), (0.745, 0.26)],
           ytop=[(-0.72, 0.055), (-0.66, 0.085), (-0.45, 0.10), (-0.10, 0.095), (0.25, 0.08), (0.55, 0.035), (0.70, -0.01), (0.745, -0.055)],
           ybase=[(-0.72, -0.17), (-0.64, -0.215), (0.60, -0.215), (0.745, -0.14)],
           cabin=[(0.16, 0.085, 0.385, 0.092, 0.365), (0.04, 0.095, 0.405, 0.172, 0.285), (-0.12, 0.100, 0.415, 0.195, 0.275),
                  (-0.30, 0.100, 0.420, 0.190, 0.280), (-0.46, 0.100, 0.420, 0.150, 0.300), (-0.58, 0.100, 0.420, 0.100, 0.330)],
           livery="vector", details=details_vector, nose=0.745, tail=-0.72, nozzle_y=-0.05,
           head=(0.285, -0.035, 0.700, 0.20, 0.034, 0.07), tails=(0.31, 0.050, -0.712, 0.22, 0.030, 0.04), tail_bar=(0.050, 0.22)),
    Design("slate", "Slate", "Flat", "plank", "Plank", "Flat and wide. A shovel for dribbles and pinches.",
           hw=[(-0.735, 0.38), (-0.68, 0.42), (-0.55, 0.455), (-0.10, 0.462), (0.30, 0.462), (0.60, 0.45), (0.72, 0.40), (0.755, 0.30)],
           ytop=[(-0.735, 0.06), (-0.68, 0.10), (-0.45, 0.115), (-0.10, 0.082), (0.30, 0.062), (0.60, 0.02), (0.72, -0.02), (0.755, -0.05)],
           ybase=[(-0.735, -0.15), (-0.66, -0.20), (0.62, -0.20), (0.755, -0.13)],
           cabin=[(0.10, 0.080, 0.360, 0.088, 0.340), (0.00, 0.085, 0.380, 0.158, 0.270), (-0.10, 0.088, 0.385, 0.172, 0.260),
                  (-0.22, 0.090, 0.385, 0.170, 0.260), (-0.34, 0.090, 0.380, 0.140, 0.290), (-0.42, 0.090, 0.370, 0.095, 0.320)],
           livery="slate", details=details_slate, nose=0.755, tail=-0.735, nozzle_y=-0.04,
           head=(0.300, -0.030, 0.738, 0.22, 0.030, 0.07), tails=(0.33, 0.045, -0.727, 0.24, 0.030, 0.04), tail_bar=(0.045, 0.24)),
    Design("dart", "Dart", "Low-long", "breakout", "Breakout", "Long and quick. Great for flicks and dribbles.",
           hw=[(-0.745, 0.30), (-0.69, 0.36), (-0.55, 0.40), (-0.20, 0.405), (0.20, 0.40), (0.50, 0.36), (0.68, 0.26), (0.775, 0.12)],
           ytop=[(-0.745, 0.07), (-0.68, 0.10), (-0.50, 0.11), (-0.15, 0.10), (0.20, 0.08), (0.55, 0.03), (0.70, -0.02), (0.775, -0.06)],
           ybase=[(-0.745, -0.18), (-0.67, -0.215), (0.62, -0.215), (0.775, -0.15)],
           cabin=[(-0.02, 0.090, 0.370, 0.095, 0.350), (-0.12, 0.100, 0.385, 0.190, 0.270), (-0.26, 0.105, 0.390, 0.210, 0.260),
                  (-0.40, 0.105, 0.390, 0.200, 0.265), (-0.52, 0.105, 0.385, 0.150, 0.280), (-0.62, 0.105, 0.370, 0.105, 0.300)],
           livery="dart", details=details_dart, nose=0.775, tail=-0.745, nozzle_y=-0.07,
           head=(0.185, -0.040, 0.742, 0.15, 0.030, 0.07), tails=(0.27, 0.060, -0.737, 0.20, 0.030, 0.04), tail_bar=None),
    Design("fusion", "Fusion", "Hybrid", "hybrid", "Hybrid", "Between Pulse and Vector. Good at everything a bit.",
           hw=[(-0.725, 0.35), (-0.67, 0.40), (-0.54, 0.438), (-0.12, 0.445), (0.22, 0.445), (0.52, 0.42), (0.68, 0.36), (0.745, 0.26)],
           ytop=[(-0.725, 0.08), (-0.67, 0.115), (-0.48, 0.13), (-0.10, 0.12), (0.20, 0.10), (0.52, 0.04), (0.67, -0.01), (0.745, -0.06)],
           ybase=[(-0.725, -0.20), (-0.64, -0.25), (0.58, -0.25), (0.745, -0.16)],
           cabin=[(0.22, 0.100, 0.385, 0.100, 0.365), (0.08, 0.115, 0.405, 0.215, 0.290), (-0.08, 0.120, 0.415, 0.235, 0.285),
                  (-0.26, 0.125, 0.420, 0.230, 0.285), (-0.44, 0.130, 0.420, 0.180, 0.305), (-0.58, 0.130, 0.420, 0.135, 0.335)],
           livery="fusion", details=details_fusion, nose=0.745, tail=-0.725, nozzle_y=-0.07,
           head=(0.275, -0.050, 0.700, 0.18, 0.040, 0.07), tails=(0.31, 0.080, -0.712, 0.21, 0.034, 0.04), tail_bar=(0.075, 0.20)),
    Design("bulwark", "Bulwark", "Tall", "merc", "Merc", "Heavy and tall. Wins the 50s, turns like a truck.",
           hw=[(-0.69, 0.37), (-0.66, 0.39), (-0.55, 0.415), (-0.10, 0.42), (0.30, 0.42), (0.62, 0.405), (0.70, 0.37), (0.715, 0.32)],
           ytop=[(-0.69, 0.14), (-0.66, 0.17), (-0.50, 0.18), (-0.10, 0.175), (0.25, 0.165), (0.55, 0.12), (0.68, 0.07), (0.715, 0.02)],
           ybase=[(-0.69, -0.26), (-0.62, -0.30), (0.58, -0.30), (0.715, -0.22)],
           cabin=[(0.28, 0.170, 0.370, 0.180, 0.360), (0.20, 0.175, 0.385, 0.300, 0.340), (0.00, 0.180, 0.395, 0.318, 0.335),
                  (-0.25, 0.180, 0.395, 0.318, 0.335), (-0.46, 0.180, 0.390, 0.300, 0.335), (-0.54, 0.180, 0.370, 0.220, 0.340)],
           livery="bulwark", details=details_bulwark, nose=0.715, tail=-0.69, nozzle_y=-0.12, arch_gap=0.05,
           head=(0.265, -0.060, 0.690, 0.17, 0.060, 0.07), tails=(0.30, 0.100, -0.680, 0.16, 0.100, 0.04), tail_bar=None),
]
BY_KEY = {d.key: d for d in DESIGNS}


# --------------------------------------------------------------------------- the texture: one atlas layout, six liveries
def _palette(team):
    return (BLUE, BLUE_DARK, BLUE_GLOW) if team == "blue" else (ORANGE, ORANGE_DARK, ORANGE_GLOW)


def body_texture(team, d):
    main, dark, glow = _palette(team)
    a = Atlas(1024, 1024, GRAPHITE)
    for n, r in CELLS.items():
        a.cell(n, *r)
    cv = a.cv
    x0, y0, x1, y1 = CELLS["paint"]
    w, h = x1 - x0, y1 - y0
    flank = {"pulse": 0.30, "vector": 0.34, "slate": 0.26, "dart": 0.36, "fusion": 0.30, "bulwark": 0.24}[d.livery]
    for i in range(w):
        u = i / (w - 1)
        side = abs(u - 0.5) * 2.0
        k = 1.06 - flank * side ** 1.6
        col = shade(main, k)
        if side > 0.86:
            col = mix(col, GRAPHITE, min(1.0, (side - 0.86) / 0.06))
        cv.rect(x0 + i, y0, x0 + i + 1, y1, fill=col)

    def vstripe(off, wd, fill, v0=0.0, v1=1.0):
        cx = x0 + (0.5 + off) * w
        cv.rect(cx - wd * w, y0 + v0 * h, cx + wd * w, y0 + v1 * h, fill=fill)

    def hband(v0, v1, fill, u0=0.0, u1=1.0):
        cv.rect(x0 + u0 * w, y0 + v0 * h, x0 + u1 * w, y0 + v1 * h, fill=fill)

    if d.livery == "pulse":
        for off in (-0.045, 0.045):
            vstripe(off, 0.022, WHITE)
        cv.rect(x0 + 0.42 * w, y0 + 0.20 * h, x0 + 0.58 * w, y0 + 0.30 * h, fill=GRAPHITE)
        for u in (0.29, 0.71):
            cv.line([(x0 + u * w, y0 + 0.06 * h), (x0 + u * w, y1 - 0.06 * h)], glow, 5)
        for u in (0.20, 0.80):
            cv.poly([(x0 + (u - 0.035) * w, y0 + 0.26 * h), (x0 + (u + 0.035) * w, y0 + 0.30 * h),
                     (x0 + (u + 0.035) * w, y0 + 0.62 * h), (x0 + (u - 0.035) * w, y0 + 0.58 * h)], fill=dark)
            cv.rect(x0 + (u - 0.012) * w, y0 + 0.30 * h, x0 + (u + 0.012) * w, y0 + 0.58 * h, fill=glow)
    elif d.livery == "vector":
        # a wide white stripe with a thin team line on both sides, the scoop in graphite, a side flash
        vstripe(0.0, 0.055, WHITE, 0.0, 1.0)
        for off in (-0.085, 0.085):
            vstripe(off, 0.008, glow)
        hband(0.30, 0.42, GRAPHITE, 0.43, 0.57)
        for u in (0.18, 0.82):
            cv.poly([(x0 + (u - 0.05) * w, y0 + 0.30 * h), (x0 + (u + 0.05) * w, y0 + 0.34 * h), (x0 + (u + 0.05) * w, y0 + 0.74 * h),
                     (x0 + (u - 0.05) * w, y0 + 0.70 * h)], fill=dark)
            cv.line([(x0 + u * w, y0 + 0.33 * h), (x0 + u * w, y0 + 0.72 * h)], glow, 6)
    elif d.livery == "slate":
        # a hazard band across the nose and the tail, two thin racing stripes, the roof in the dark tone
        for k in range(12):
            u0 = k / 12.0
            hband(0.045, 0.115, dark if k % 2 == 0 else glow, u0, u0 + 1 / 12.0)
            hband(0.885, 0.955, dark if k % 2 == 0 else glow, u0, u0 + 1 / 12.0)
        for off in (-0.10, 0.10):
            vstripe(off, 0.012, WHITE, 0.12, 0.88)
        for u in (0.16, 0.84):
            cv.rect(x0 + (u - 0.03) * w, y0 + 0.36 * h, x0 + (u + 0.03) * w, y0 + 0.64 * h, fill=dark)
            cv.line([(x0 + u * w, y0 + 0.36 * h), (x0 + u * w, y0 + 0.64 * h)], glow, 4)
    elif d.livery == "dart":
        # two thin stripes from the nose to the tail fin, a bright line along each shoulder, a chevron on the nose
        for off in (-0.13, 0.13):
            vstripe(off, 0.010, WHITE)
        for off in (-0.16, 0.16):
            vstripe(off, 0.004, glow)
        for u in (0.27, 0.73):
            cv.line([(x0 + u * w, y0 + 0.06 * h), (x0 + u * w, y1 - 0.06 * h)], glow, 6)
        for k in range(3):
            v = 0.09 + 0.04 * k
            cv.poly([(x0 + 0.38 * w, y0 + (v + 0.03) * h), (x0 + 0.5 * w, y0 + v * h), (x0 + 0.62 * w, y0 + (v + 0.03) * h),
                     (x0 + 0.62 * w, y0 + (v + 0.045) * h), (x0 + 0.5 * w, y0 + (v + 0.015) * h), (x0 + 0.38 * w, y0 + (v + 0.045) * h)], fill=dark)
    elif d.livery == "fusion":
        # two-tone: the front half in the dark tone behind a slanted split, a white stripe off the centre line, vents in graphite
        for i in range(w):
            u = i / (w - 1)
            side = abs(u - 0.5) * 2.0
            split = 0.50 + (u - 0.5) * 0.18
            if side < 0.84:
                cv.rect(x0 + i, y0 + 0.06 * h, x0 + i + 1, y0 + split * h, fill=shade(dark, 1.05 - 0.2 * side ** 1.6))
        vstripe(0.11, 0.020, WHITE)
        vstripe(0.16, 0.006, glow)
        hband(0.20, 0.30, GRAPHITE, 0.36, 0.64)
        for u in (0.22, 0.78):
            cv.line([(x0 + u * w, y0 + 0.50 * h), (x0 + u * w, y1 - 0.06 * h)], glow, 5)
    elif d.livery == "bulwark":
        # lower cladding in graphite, a team line above it, a dark roof panel and a hazard bar on the bumpers
        for i in range(w):
            u = i / (w - 1)
            side = abs(u - 0.5) * 2.0
            if 0.58 < side < 0.90:
                cv.rect(x0 + i, y0, x0 + i + 1, y1, fill=shade(GRAPHITE, 1.2 - 0.3 * side))
        for u in (0.5 - 0.30, 0.5 + 0.30):
            cv.line([(x0 + u * w, y0 + 0.05 * h), (x0 + u * w, y1 - 0.05 * h)], glow, 6)
        vstripe(0.0, 0.03, dark, 0.12, 0.88)
        for k in range(8):
            u0 = 0.2 + 0.6 * k / 8.0
            hband(0.045, 0.085, dark if k % 2 == 0 else glow, u0, u0 + 0.6 / 8.0)
            hband(0.915, 0.955, dark if k % 2 == 0 else glow, u0, u0 + 0.6 / 8.0)
    # the nose and tail ends are graphite plastic for every car
    cv.rect(x0, y0, x1, y0 + 0.055 * h, fill=GRAPHITE)
    cv.rect(x0, y1 - 0.045 * h, x1, y1, fill=GRAPHITE)
    # glass: a dark pane with a soft sky reflection, framed in the paint of the car
    gx0, gy0, gx1, gy1 = CELLS["glass"]
    gw, gh = gx1 - gx0, gy1 - gy0
    cv.vgrad(gx0, gy0, gx1, gy1, (36, 52, 78), (10, 14, 24))
    cv.poly([(gx0 + 0.55 * gw, gy0), (gx0 + 0.8 * gw, gy0), (gx0 + 0.45 * gw, gy1), (gx0 + 0.2 * gw, gy1)], fill=(52, 72, 104))
    frame = 0.07
    for r in ((gx0, gy0, gx1, gy0 + frame * gh), (gx0, gy1 - frame * gh, gx1, gy1), (gx0, gy0, gx0 + frame * gw, gy1), (gx1 - frame * gw, gy0, gx1, gy1)):
        cv.rect(*r, fill=shade(main, 0.8))
    tx0, ty0, tx1, ty1 = CELLS["trim"]
    cv.vgrad(tx0, ty0, tx1, ty1, (60, 66, 78), (26, 29, 36))
    cx0, cy0, cx1, cy1 = CELLS["chrome"]
    stops = [(235, 240, 248), (120, 130, 148), (200, 208, 222), (70, 78, 94), (180, 188, 204)]
    bh = (cy1 - cy0) / len(stops)
    for i, c in enumerate(stops):
        cv.rect(cx0, cy0 + i * bh, cx1, cy0 + (i + 1) * bh + 1, fill=c)
    return a


# --------------------------------------------------------------------------- meshes
def build_body(atlas, d):
    uv = atlas.uv
    body = Mesh("car_body")
    rings = [d.hull_ring(z) for z in d.hull_stations()]
    body.extend(loft(rings, rect=uv("paint"), closed=False, caps=(True, True), smooth_deg=50.0))
    cabin_rings = [cabin_ring(s) for s in d.cabin]
    glass, paint = uv("glass"), uv("paint")
    pw, ph = paint[2] - paint[0], paint[3] - paint[1]
    paint_roof = (paint[0] + pw * 0.480, paint[1] + ph * 0.40, paint[0] + pw * 0.520, paint[1] + ph * 0.60)
    last = len(d.cabin) - 2
    seg_top = (2, 3, 4, 5)

    def cell(i, j):
        pts = cabin_rings[i]
        if i in (0, last) and j in seg_top:
            ax, bx = pts[j][0], pts[j + 1][0]
            lo, hi = pts[2][0], pts[6][0]
            f0, f1 = (ax - lo) / (hi - lo), (bx - lo) / (hi - lo)
            return (glass[0] + (glass[2] - glass[0]) * f0, glass[1], glass[0] + (glass[2] - glass[0]) * f1, glass[3])
        if 1 <= i < last and j in (1, 6):
            gh = glass[3] - glass[1]
            f0, f1 = (i - 1) / (last - 1), i / (last - 1)
            return (glass[0], glass[1] + gh * f0, glass[2], glass[1] + gh * f1)
        return paint_roof
    body.extend(loft(cabin_rings, rect=paint, closed=False, cell_fn=cell, smooth_deg=40.0))
    rects = {"trim": uv("trim"), "chrome": uv("chrome"), "roof": paint_roof, "paint": paint}
    d.details(body, uv, d, rects)
    return body


def build_lights(d):
    head = (0.0, 0.0, 0.5, 1.0)
    tail = (0.5, 0.0, 1.0, 1.0)
    m = Mesh("car_lights")
    hx, hy, hz, hw, hh, hd = d.head
    tx, ty, tz, tw, th, td = d.tails
    for sx in (-1, 1):
        m.extend(box((sx * hx, hy, hz), (hw, hh, hd), rect=head))
        m.extend(box((sx * tx, ty, tz), (tw, th, td), rect=tail))
    if d.tail_bar is not None:
        m.extend(box((0.0, d.tail_bar[0], tz), (d.tail_bar[1], 0.014, td), rect=tail))
    return m


def nozzle_position(d):
    return (0.0, d.nozzle_y, round(d.tail + 0.04 - 0.10, 4))
