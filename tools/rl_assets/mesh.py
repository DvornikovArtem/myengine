"""Triangle meshes of the RL assets: a small builder, primitives, and the OBJ writer of the engine's convention.

Everything is described in the metres of the game world (x right, y up, z forward, +z is the orange end of the field).
A triangle is front facing when cross(p1 - p0, p2 - p0) points along its normal (the right handed formula in the
engine's left handed world, which is what D3D12's clockwise front face turns into); the builders orient triangles that
way from a normal or an inside point. UV is the OBJ one: v = 1 is the TOP of the texture image.

The OBJ writer applies what ResourceManager's loader undoes: it writes z mirrored and the winding reversed, and it
normalises the mesh into the unit space of the renderer. The renderer culls an entity by a sphere of
0.87 * max(scale) around its origin, so a mesh has to fit into a radius of 0.87 around its own origin: the writer
picks the scale S (metres per mesh unit) that makes the radius 0.85 and the entity is placed with scale (S, S, S).
"""

import math
from pathlib import Path

UNIT_RADIUS = 0.85  # the mesh fits this sphere around its origin; the renderer's culling sphere is 0.87


# --------------------------------------------------------------------------- vectors
def vadd(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def vsub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def vmul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def length(a):
    return math.sqrt(dot(a, a))


def normalize(a):
    n = length(a)
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-12 else (0.0, 1.0, 0.0)


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


# --------------------------------------------------------------------------- transforms
class Xf:
    """A 3x3 matrix and a translation: p' = R p + t. Row-major 3x3 as three rows."""

    def __init__(self, rows=((1, 0, 0), (0, 1, 0), (0, 0, 1)), t=(0, 0, 0)):
        self.r = tuple(tuple(float(v) for v in row) for row in rows)
        self.t = tuple(float(v) for v in t)
        self._normal = None

    def point(self, p):
        r = self.r
        return (r[0][0] * p[0] + r[0][1] * p[1] + r[0][2] * p[2] + self.t[0],
                r[1][0] * p[0] + r[1][1] * p[1] + r[1][2] * p[2] + self.t[1],
                r[2][0] * p[0] + r[2][1] * p[1] + r[2][2] * p[2] + self.t[2])

    def direction(self, d):
        """For normals: the inverse transpose, so a non uniform scale keeps them perpendicular."""
        if self._normal is None:
            r = self.r
            c0 = cross(r[1], r[2])
            c1 = cross(r[2], r[0])
            c2 = cross(r[0], r[1])
            self._normal = (c0, c1, c2)  # the cofactor matrix = det * inverse transpose
        n = self._normal
        return normalize((n[0][0] * d[0] + n[0][1] * d[1] + n[0][2] * d[2],
                          n[1][0] * d[0] + n[1][1] * d[1] + n[1][2] * d[2],
                          n[2][0] * d[0] + n[2][1] * d[1] + n[2][2] * d[2]))

    def det(self):
        r = self.r
        return dot(r[0], cross(r[1], r[2]))

    def __mul__(self, other):
        """(self * other)(p) = self(other(p))."""
        a, b = self.r, other.r
        rows = tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))
        return Xf(rows, self.point(other.t))


def translate(x, y=None, z=None):
    if y is None:
        x, y, z = x
    return Xf(t=(x, y, z))


def scale(sx, sy=None, sz=None):
    if sy is None:
        sy = sz = sx
    return Xf(((sx, 0, 0), (0, sy, 0), (0, 0, sz)))


def rot_y(deg):
    """Yaw: turns +z towards +x (the game's convention)."""
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return Xf(((c, 0, s), (0, 1, 0), (-s, 0, c)))


def rot_x(deg):
    """Pitch: turns +z towards -y (the nose down for a positive angle)."""
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return Xf(((1, 0, 0), (0, c, -s), (0, s, c)))


def rot_z(deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return Xf(((c, -s, 0), (s, c, 0), (0, 0, 1)))


def basis(forward, up=(0, 1, 0)):
    """A rotation that takes +z to `forward` (and keeps +y near `up`)."""
    f = normalize(forward)
    r = normalize(cross(up, f))
    if length(cross(up, f)) < 1e-9:
        r = (1.0, 0.0, 0.0)
    u = cross(f, r)
    return Xf(((r[0], u[0], f[0]), (r[1], u[1], f[1]), (r[2], u[2], f[2])))


# --------------------------------------------------------------------------- the mesh
class Mesh:
    """Corners in triples: triangle t is corners 3t, 3t + 1, 3t + 2 (position, uv, normal each)."""

    def __init__(self, name="mesh"):
        self.name = name
        self.pos = []
        self.uv = []
        self.nrm = []

    # -- adding
    def tri(self, p0, p1, p2, uv0=(0.0, 0.0), uv1=(1.0, 0.0), uv2=(0.0, 1.0), normals=None, inside=None, outside=None):
        """One triangle, oriented from `normals` (a corner normal each), or away from `inside`, or towards `outside`.
        Without a hint the order is kept. Without `normals` the face normal is used (flat)."""
        n = cross(vsub(p1, p0), vsub(p2, p0))
        if dot(n, n) < 1e-14:
            return False
        flip = False
        if normals is not None:
            flip = dot(n, vadd(vadd(normals[0], normals[1]), normals[2])) < 0.0
        elif inside is not None:
            flip = dot(n, vsub(vmul(vadd(vadd(p0, p1), p2), 1.0 / 3.0), inside)) < 0.0
        elif outside is not None:
            flip = dot(n, vsub(outside, vmul(vadd(vadd(p0, p1), p2), 1.0 / 3.0))) < 0.0
        if flip:
            p1, p2 = p2, p1
            uv1, uv2 = uv2, uv1
            if normals is not None:
                normals = (normals[0], normals[2], normals[1])
            n = (-n[0], -n[1], -n[2])
        if normals is None:
            fn = normalize(n)
            normals = (fn, fn, fn)
        self.pos.extend((p0, p1, p2))
        self.uv.extend((uv0, uv1, uv2))
        self.nrm.extend(normals)
        return True

    def quad(self, p0, p1, p2, p3, uvs=None, normals=None, inside=None, outside=None):
        """A quad p0 p1 p2 p3 (in a ring order). uvs: four (u, v) or one rect (u0, v0, u1, v1) laid on p0 p1 p2 p3 as
        (u0, v0) (u1, v0) (u1, v1) (u0, v1)."""
        if uvs is None:
            uvs = ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0))
        elif len(uvs) == 4 and not isinstance(uvs[0], (tuple, list)):
            u0, v0, u1, v1 = uvs
            uvs = ((u0, v0), (u1, v0), (u1, v1), (u0, v1))
        if normals is None:
            nq = (None, None, None, None)
        else:
            nq = normals
        a = None if normals is None else (nq[0], nq[1], nq[2])
        b = None if normals is None else (nq[0], nq[2], nq[3])
        self.tri(p0, p1, p2, uvs[0], uvs[1], uvs[2], a, inside, outside)
        self.tri(p0, p2, p3, uvs[0], uvs[2], uvs[3], b, inside, outside)

    def extend(self, other, xf=None):
        if xf is None:
            self.pos.extend(other.pos)
            self.uv.extend(other.uv)
            self.nrm.extend(other.nrm)
            return
        flip = xf.det() < 0.0
        for t in range(len(other.pos) // 3):
            i = 3 * t
            p = [xf.point(other.pos[i + k]) for k in range(3)]
            n = [xf.direction(other.nrm[i + k]) for k in range(3)]
            uv = [other.uv[i + k] for k in range(3)]
            if flip:  # a mirror reverses the winding
                p[1], p[2] = p[2], p[1]
                n[1], n[2] = n[2], n[1]
                uv[1], uv[2] = uv[2], uv[1]
            self.pos.extend(p)
            self.nrm.extend(n)
            self.uv.extend(uv)

    def transformed(self, xf):
        out = Mesh(self.name)
        out.extend(self, xf)
        return out

    def remap_uv(self, rect):
        """Maps the UV of a 0..1 mesh into the rect (u0, v0, u1, v1) of an atlas."""
        u0, v0, u1, v1 = rect
        self.uv = [(u0 + (u1 - u0) * u, v0 + (v1 - v0) * v) for (u, v) in self.uv]

    # -- queries
    @property
    def triangle_count(self):
        return len(self.pos) // 3

    def bounds(self):
        xs = [p[0] for p in self.pos]
        ys = [p[1] for p in self.pos]
        zs = [p[2] for p in self.pos]
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))

    def radius_around(self, origin):
        return max(length(vsub(p, origin)) for p in self.pos)

    # -- normals
    def smooth(self, crease_deg=50.0, only_flat=False):
        """Recomputes the corner normals: a corner takes the area weighted normal of the faces that meet at its position
        and bend less than `crease_deg` from its own face. Sharp edges stay sharp, round surfaces get smooth."""
        cos_limit = math.cos(math.radians(crease_deg))
        faces = []
        groups = {}
        for t in range(self.triangle_count):
            i = 3 * t
            a, b, c = self.pos[i], self.pos[i + 1], self.pos[i + 2]
            n = cross(vsub(b, a), vsub(c, a))
            area = length(n)
            faces.append((normalize(n), area))
            for k in range(3):
                p = self.pos[i + k]
                key = (round(p[0] * 2000), round(p[1] * 2000), round(p[2] * 2000))
                groups.setdefault(key, []).append(t)
        new = list(self.nrm)
        for t in range(self.triangle_count):
            fn, _ = faces[t]
            for k in range(3):
                p = self.pos[3 * t + k]
                key = (round(p[0] * 2000), round(p[1] * 2000), round(p[2] * 2000))
                s = (0.0, 0.0, 0.0)
                for o in groups[key]:
                    on, oa = faces[o]
                    if dot(on, fn) >= cos_limit:
                        s = vadd(s, vmul(on, oa))
                new[3 * t + k] = normalize(s) if dot(s, s) > 1e-18 else fn
        self.nrm = new

    def orientation_report(self):
        """Fraction of the triangles whose winding agrees with their corner normals (1.0 for a clean mesh)."""
        good = 0
        for t in range(self.triangle_count):
            i = 3 * t
            n = cross(vsub(self.pos[i + 1], self.pos[i]), vsub(self.pos[i + 2], self.pos[i]))
            s = vadd(vadd(self.nrm[i], self.nrm[i + 1]), self.nrm[i + 2])
            if dot(n, s) > 0.0:
                good += 1
        return good / max(1, self.triangle_count)


# --------------------------------------------------------------------------- primitives
FULL = (0.0, 0.0, 1.0, 1.0)


def box(center=(0, 0, 0), size=(1, 1, 1), rects=None, rect=FULL, skip=()):
    """An axis aligned box, flat normals. rects: face -> (u0, v0, u1, v1) for '+x' '-x' '+y' '-y' '+z' '-z'; every face
    shows the same image upright when looked at from outside (u to the right, v up; on +y the image top is +z)."""
    m = Mesh("box")
    hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    cx, cy, cz = center
    faces = {
        "+x": ((cx + hx, cy - hy, cz - hz), (0, 0, 2 * hz), (0, 2 * hy, 0)),
        "-x": ((cx - hx, cy - hy, cz + hz), (0, 0, -2 * hz), (0, 2 * hy, 0)),
        "+y": ((cx - hx, cy + hy, cz - hz), (2 * hx, 0, 0), (0, 0, 2 * hz)),
        "-y": ((cx - hx, cy - hy, cz + hz), (2 * hx, 0, 0), (0, 0, -2 * hz)),
        "+z": ((cx + hx, cy - hy, cz + hz), (-2 * hx, 0, 0), (0, 2 * hy, 0)),
        "-z": ((cx - hx, cy - hy, cz - hz), (2 * hx, 0, 0), (0, 2 * hy, 0)),
    }
    for name, (o, u, v) in faces.items():
        if name in skip:
            continue
        r = (rects or {}).get(name, rect)
        p0 = o
        p1 = vadd(o, u)
        p2 = vadd(vadd(o, u), v)
        p3 = vadd(o, v)
        outward = {"+x": (1, 0, 0), "-x": (-1, 0, 0), "+y": (0, 1, 0), "-y": (0, -1, 0), "+z": (0, 0, 1), "-z": (0, 0, -1)}[name]
        n = (outward,) * 4
        m.quad(p0, p1, p2, p3, r, n)
    return m


def revolve(profile, segments=24, axis_point=(0, 0, 0), rect=FULL, start_deg=0.0, sweep_deg=360.0, closed_u=True,
            v_from_length=True):
    """A surface of revolution about the Y axis through `axis_point`. profile: [(radius, y)] bottom to top; the normals
    come from the profile (smooth). u runs round (0..1 over the sweep), v along the profile (bottom 0 .. top 1)."""
    m = Mesh("revolve")
    n = len(profile)
    arc = [0.0]
    for i in range(1, n):
        arc.append(arc[-1] + math.hypot(profile[i][0] - profile[i - 1][0], profile[i][1] - profile[i - 1][1]))
    total = max(arc[-1], 1e-9)
    # profile normals (pointing out, as the profile goes up with the surface on its left looking from outside)
    pn = []
    for i in range(n):
        a = profile[max(i - 1, 0)]
        b = profile[min(i + 1, n - 1)]
        dr, dy = b[0] - a[0], b[1] - a[1]
        ln = math.hypot(dr, dy) or 1.0
        pn.append((dy / ln, -dr / ln))  # (radial, vertical) of the normal
    u0, v0, u1, v1 = rect
    for s in range(segments):
        a0 = math.radians(start_deg + sweep_deg * s / segments)
        a1 = math.radians(start_deg + sweep_deg * (s + 1) / segments)
        for i in range(n - 1):
            def corner(a, ang, j):
                r, y = profile[j]
                x = axis_point[0] + r * math.sin(ang)
                z = axis_point[2] + r * math.cos(ang)
                nr, ny = pn[j]
                return (x, axis_point[1] + y, z), (nr * math.sin(ang), ny, nr * math.cos(ang))
            pa, na = corner(None, a0, i)
            pb, nb = corner(None, a1, i)
            pc, nc = corner(None, a1, i + 1)
            pd, nd = corner(None, a0, i + 1)
            va = arc[i] / total if v_from_length else i / (n - 1)
            vb = arc[i + 1] / total if v_from_length else (i + 1) / (n - 1)
            ua, ub = s / segments, (s + 1) / segments
            uvs = ((u0 + (u1 - u0) * ua, v0 + (v1 - v0) * va), (u0 + (u1 - u0) * ub, v0 + (v1 - v0) * va),
                   (u0 + (u1 - u0) * ub, v0 + (v1 - v0) * vb), (u0 + (u1 - u0) * ua, v0 + (v1 - v0) * vb))
            m.quad(pa, pb, pc, pd, uvs, (na, nb, nc, nd))
    return m


def disc(center, radius, normal=(0, 1, 0), segments=24, rect=FULL, inner=0.0, rotation_deg=0.0):
    """A flat ring or disc facing `normal`; the UV is the disc inscribed in the rect (a planar projection)."""
    m = Mesh("disc")
    xf = basis(normal)
    xf = Xf(xf.r, center)
    u0, v0, u1, v1 = rect
    cu, cv = (u0 + u1) / 2, (v0 + v1) / 2
    ru, rv = (u1 - u0) / 2, (v1 - v0) / 2
    n = normalize(normal)
    for s in range(segments):
        a0 = math.radians(rotation_deg + 360.0 * s / segments)
        a1 = math.radians(rotation_deg + 360.0 * (s + 1) / segments)
        def pt(a, r):
            return xf.point((r * math.cos(a), r * math.sin(a), 0.0)), (cu + ru * (r / radius) * math.cos(a), cv + rv * (r / radius) * math.sin(a))
        if inner <= 0.0:
            p0, t0 = (center, (cu, cv))
            p1, t1 = pt(a0, radius)
            p2, t2 = pt(a1, radius)
            m.tri(p0, p1, p2, t0, t1, t2, (n, n, n), None)
        else:
            p0, t0 = pt(a0, inner)
            p1, t1 = pt(a0, radius)
            p2, t2 = pt(a1, radius)
            p3, t3 = pt(a1, inner)
            m.quad(p0, p1, p2, p3, (t0, t1, t2, t3), (n, n, n, n))
    return m


def cylinder(p_from, p_to, radius, radius_to=None, segments=16, caps=(True, True), rect=FULL, rect_caps=None):
    """A (truncated) cone along p_from -> p_to: smooth sides, flat caps. Side UV: u round, v along."""
    radius_to = radius if radius_to is None else radius_to
    axis = vsub(p_to, p_from)
    h = length(axis)
    xf = Xf(basis(axis).r, p_from)
    m = Mesh("cylinder")
    slope = (radius - radius_to) / h if h > 1e-9 else 0.0  # the slant of the side normals of a cone
    u0, v0, u1, v1 = rect
    for s in range(segments):
        a0 = 2 * math.pi * s / segments
        a1 = 2 * math.pi * (s + 1) / segments
        def rp(a, r, z):
            return (r * math.cos(a), r * math.sin(a), z)
        def rn(a):
            nl = math.hypot(1.0, slope)
            return (math.cos(a) / nl, math.sin(a) / nl, slope / nl)
        pa, pb, pc, pd = rp(a0, radius, 0.0), rp(a1, radius, 0.0), rp(a1, radius_to, h), rp(a0, radius_to, h)
        uu0 = u0 + (u1 - u0) * s / segments
        uu1 = u0 + (u1 - u0) * (s + 1) / segments
        sub = Mesh()
        sub.quad(pa, pb, pc, pd, ((uu0, v0), (uu1, v0), (uu1, v1), (uu0, v1)), (rn(a0), rn(a1), rn(a1), rn(a0)))
        m.extend(sub, xf)
    cap_rect = rect_caps or rect
    if caps[0] and radius > 0:
        m.extend(disc((0, 0, 0), radius, (0, 0, -1), segments, cap_rect), xf)
    if caps[1] and radius_to > 0:
        m.extend(disc((0, 0, h), radius_to, (0, 0, 1), segments, cap_rect), xf)
    return m


def uv_sphere(center=(0, 0, 0), radius=1.0, segments=24, rings=12, rect=FULL):
    m = revolve([(radius * math.sin(math.pi * r / rings), -radius * math.cos(math.pi * r / rings)) for r in range(rings + 1)],
                segments, axis_point=center, rect=rect, v_from_length=False)
    m.name = "sphere"
    return m


def grid_surface(fn, nu, nv, rect=FULL, inside=None, outside=None, smooth_normals=True):
    """A surface p = fn(u, v) over a grid (u, v in 0..1); the UV fills the rect. Normals from the neighbours."""
    m = Mesh("grid")
    u0, v0, u1, v1 = rect
    pts = [[fn(i / nu, j / nv) for j in range(nv + 1)] for i in range(nu + 1)]
    def nrm(i, j):
        a = pts[min(i + 1, nu)][j]
        b = pts[max(i - 1, 0)][j]
        c = pts[i][min(j + 1, nv)]
        d = pts[i][max(j - 1, 0)]
        n = cross(vsub(a, b), vsub(c, d))
        n = normalize(n)
        if inside is not None and dot(n, vsub(pts[i][j], inside)) < 0:
            n = vmul(n, -1)
        if outside is not None and dot(n, vsub(outside, pts[i][j])) < 0:
            n = vmul(n, -1)
        return n
    for i in range(nu):
        for j in range(nv):
            c = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
            ps = [pts[a][b] for a, b in c]
            ns = [nrm(a, b) for a, b in c]
            uvs = [(u0 + (u1 - u0) * a / nu, v0 + (v1 - v0) * b / nv) for a, b in c]
            m.quad(*ps, uvs=uvs, normals=tuple(ns))
    return m


# --------------------------------------------------------------------------- OBJ
def _q(v, digits):
    return round(v, digits) + 0.0


def write_obj(path, mesh, origin=(0.0, 0.0, 0.0), unit_scale=None, comment="", digits=5):
    """Writes the mesh as an OBJ of the engine's convention in the unit space (see the module header).
    Returns {'name', 'triangles', 'vertices', 'bytes', 'origin', 'scale', 'radius', 'bounds'}."""
    path = Path(path)
    radius_m = mesh.radius_around(origin)
    if unit_scale is None:
        unit_scale = math.ceil(radius_m / UNIT_RADIUS * 1000.0) / 1000.0
    s = float(unit_scale)
    inv = 1.0 / s
    positions, uvs, normals = {}, {}, {}
    pos_lines, uv_lines, nrm_lines, faces = [], [], [], []

    def pool(table, lines, key, text):
        idx = table.get(key)
        if idx is None:
            idx = len(table) + 1
            table[key] = idx
            lines.append(text)
        return idx

    unique_corners = set()
    kept = 0
    for t in range(mesh.triangle_count):
        i = 3 * t
        corner_refs = []
        degenerate = False
        for k in range(3):
            p = mesh.pos[i + k]
            q = ((p[0] - origin[0]) * inv, (p[1] - origin[1]) * inv, -(p[2] - origin[2]) * inv)  # z mirrored
            qk = (_q(q[0], digits), _q(q[1], digits), _q(q[2], digits))
            u, v = mesh.uv[i + k]
            uk = (_q(u, 5), _q(v, 5))
            n = mesh.nrm[i + k]
            nk = (_q(n[0], 3), _q(n[1], 3), _q(-n[2], 3))
            ip = pool(positions, pos_lines, qk, "v %s %s %s" % tuple(("%.*f" % (digits, c)).rstrip("0").rstrip(".") or "0" for c in qk))
            iu = pool(uvs, uv_lines, uk, "vt %s %s" % tuple(("%.5f" % c).rstrip("0").rstrip(".") or "0" for c in uk))
            inn = pool(normals, nrm_lines, nk, "vn %s %s %s" % tuple(("%.3f" % c).rstrip("0").rstrip(".") or "0" for c in nk))
            corner_refs.append((ip, iu, inn))
        if len({r[0] for r in corner_refs}) < 3:
            continue
        a, b, c = corner_refs
        # the loader reverses the order it reads: write (a, c, b) so that it ends up with the canonical (a, b, c)
        faces.append("f %d/%d/%d %d/%d/%d %d/%d/%d" % (a + c + b))
        unique_corners.update((a, c, b))
        kept += 1
    lines = ["# %s (tools/rl_assets). %s" % (path.name, comment), "# unit space: world metres = vertex * %.3f + origin %.4f %.4f %.4f; "
             "z is mirrored as the ResourceManager loader expects" % ((s,) + tuple(origin)), "o %s" % path.stem]
    lines += pos_lines + uv_lines + nrm_lines + faces
    text = "\n".join(lines) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    lo, hi = mesh.bounds()
    return {"name": path.stem, "triangles": kept, "vertices": len(unique_corners), "bytes": len(text.encode("utf-8")),
            "origin": [round(v, 4) for v in origin], "scale": s, "radius": round(radius_m / s, 4),
            "bounds": [[round(v, 3) for v in lo], [round(v, 3) for v in hi]]}


# --------------------------------------------------------------------------- lofts
def ring_centroid(ring):
    n = len(ring)
    return (sum(p[0] for p in ring) / n, sum(p[1] for p in ring) / n, sum(p[2] for p in ring) / n)


def loft(rings, rect=FULL, closed=True, caps=(False, False), cell_fn=None, flat=False, smooth_deg=60.0):
    """A skin over rings of the same point count, ordered along the body. Faces face away from the rings' centroids.
    u runs round the ring (arc length, 0..1), v along the body (0..1 by the distance between the ring centroids).
    cell_fn(i, j) -> rect gives a quad its own UV rect over the whole quad (for atlas cells), i the ring pair, j the point.
    Normals are smoothed over the creases below `smooth_deg` unless flat."""
    m = Mesh("loft")
    n = len(rings[0])
    u0, v0, u1, v1 = rect
    cents = [ring_centroid(r) for r in rings]
    vs = [0.0]
    for i in range(1, len(rings)):
        vs.append(vs[-1] + length(vsub(cents[i], cents[i - 1])))
    vt = max(vs[-1], 1e-9)
    us = []
    for ring in rings:
        acc = [0.0]
        count = n if closed else n - 1
        for j in range(1, count + 1):
            acc.append(acc[-1] + length(vsub(ring[j % n], ring[j - 1])))
        tot = max(acc[-1], 1e-9)
        us.append([a / tot for a in acc])
    segs = n if closed else n - 1
    cap_uv = ((u0 + u1) / 2, (v0 + v1) / 2)
    for i in range(len(rings) - 1):
        inside = lerp(cents[i], cents[i + 1], 0.5)
        for j in range(segs):
            j2 = (j + 1) % n
            p = (rings[i][j], rings[i][j2], rings[i + 1][j2], rings[i + 1][j])
            if cell_fn is not None:
                cu0, cv0, cu1, cv1 = cell_fn(i, j)
                uvs = ((cu0, cv0), (cu1, cv0), (cu1, cv1), (cu0, cv1))
            else:
                ua, ub = us[i][j], us[i][j + 1]
                uc, ud = us[i + 1][j + 1], us[i + 1][j]
                va, vb = vs[i] / vt, vs[i + 1] / vt
                f = lambda a, b: (u0 + (u1 - u0) * a, v0 + (v1 - v0) * b)
                uvs = (f(ua, va), f(ub, va), f(uc, vb), f(ud, vb))
            m.quad(p[0], p[1], p[2], p[3], uvs, inside=inside)
    for k, end in enumerate((0, len(rings) - 1)):
        if caps[k]:
            c = cents[end]
            ring = rings[end]
            out = vadd(c, vsub(c, cents[1 if end == 0 else -2]))
            for j in range(n):
                m.tri(c, ring[j], ring[(j + 1) % n], cap_uv, cap_uv, cap_uv, outside=out)
    if not flat:
        m.smooth(smooth_deg)
    return m


# --------------------------------------------------------------------------- beams
def beam(p0, p1, width, height, up=(0, 1, 0), rect=FULL, accent_dir=None, accent_rect=None, caps=True):
    """A box from p0 to p1 with the cross-section width x height (width along cross(up, p1 - p0), height along up).
    accent_dir picks the face turned that way (a world direction) and gives it accent_rect instead of rect."""
    f = vsub(p1, p0)
    ln = length(f)
    if ln < 1e-6:
        return Mesh("beam")
    xf = basis(f, up)
    xf = Xf(xf.r, lerp(p0, p1, 0.5))
    rects = None
    if accent_dir is not None and accent_rect is not None:
        r_axis = (xf.r[0][0], xf.r[1][0], xf.r[2][0])
        u_axis = (xf.r[0][1], xf.r[1][1], xf.r[2][1])
        best = max(((abs(dot(accent_dir, a)), name, 1 if dot(accent_dir, a) > 0 else -1)
                    for a, name in ((r_axis, "x"), (u_axis, "y"))))
        face = ("+" if best[2] > 0 else "-") + best[1]
        rects = {face: accent_rect}
    m = box((0, 0, 0), (width, height, ln), rects=rects, rect=rect)
    return m.transformed(xf)
