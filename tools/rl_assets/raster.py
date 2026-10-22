"""A small software preview of the engine's textured_lit scene program (numpy + Pillow, previews only).

It loads an OBJ exactly as ResourceManager does (z negated, v flipped, winding reversed), culls like D3D12 with the
engine's front face, shades texture * tint * (ambient 0.25 + Lambert) with the engine light, and cuts the entity by the
renderer's culling sphere (0.87 * max scale around the origin) so that a mesh that would pop out in the game shows up
cut here. The generator never needs it; it only draws the PNG previews and checks the meshes.
"""

import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

LIGHT = np.array([0.35, -1.0, 0.25], dtype=np.float64)
LIGHT /= np.linalg.norm(LIGHT)
AMBIENT = 0.25
CULL_RADIUS = 0.87

_obj_cache = {}
_tex_cache = {}


def load_obj(path):
    """-> (pos[T,3,3], uv[T,3,2], nrm[T,3,3]) in mesh space, as the engine holds them after loading."""
    path = str(path)
    if path in _obj_cache:
        return _obj_cache[path]
    v, vt, vn = [], [], []
    tp, tu, tn = [], [], []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if line.startswith("v "):
            x, y, z = map(float, line.split()[1:4])
            v.append((x, y, -z))
        elif line.startswith("vt "):
            a = line.split()
            vt.append((float(a[1]), 1.0 - float(a[2])))
        elif line.startswith("vn "):
            x, y, z = map(float, line.split()[1:4])
            vn.append((x, y, -z))
        elif line.startswith("f "):
            refs = []
            for tok in line.split()[1:]:
                parts = tok.split("/")
                refs.append((int(parts[0]) - 1, int(parts[1]) - 1 if len(parts) > 1 and parts[1] else -1,
                             int(parts[2]) - 1 if len(parts) > 2 and parts[2] else -1))
            refs.reverse()  # the loader reverses the winding
            for k in range(1, len(refs) - 1):
                for r in (refs[0], refs[k], refs[k + 1]):
                    tp.append(v[r[0]])
                    tu.append(vt[r[1]] if r[1] >= 0 else (0.0, 0.0))
                    tn.append(vn[r[2]] if r[2] >= 0 else (0.0, 1.0, 0.0))
    out = (np.array(tp, dtype=np.float64).reshape(-1, 3, 3), np.array(tu, dtype=np.float64).reshape(-1, 3, 2),
           np.array(tn, dtype=np.float64).reshape(-1, 3, 3))
    _obj_cache[path] = out
    return out


def winding_agreement(path):
    """Fraction of the loaded triangles that are front facing by their own normals (1.0 for a clean OBJ)."""
    p, _, n = load_obj(path)
    c = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    return float(np.mean(np.einsum("ij,ij->i", c, n.sum(axis=1)) > 0.0))


def load_texture(path):
    path = str(path)
    if path not in _tex_cache:
        _tex_cache[path] = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64) / 255.0
    return _tex_cache[path]


def load_material(path):
    """-> (texture array or None, tint rgb)."""
    path = Path(path)
    m = json.loads(path.read_text(encoding="utf-8"))
    tex = (path.parent / m["texture"]).resolve() if m.get("texture") else None
    tint = np.array(m.get("tint", [1, 1, 1, 1])[:3], dtype=np.float64)
    return (load_texture(tex) if tex and tex.exists() else None), tint


def euler_matrix(rot_deg):
    """The rotation of an entity from rotationDeg = (pitch, yaw, roll): roll about z, then pitch about x, then yaw about y."""
    px, py, pz = (math.radians(a) for a in rot_deg)
    cx, sx, cy, sy, cz, sz = math.cos(px), math.sin(px), math.cos(py), math.sin(py), math.cos(pz), math.sin(pz)
    rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])  # positive pitch looks down
    ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return ry @ rx @ rz  # applied right to left to a column vector: roll, pitch, yaw


def _clip_near(cp, uv, sh, near):
    """Clips a camera space triangle against the near plane: yields 0, 1 or 2 triangles (cp, uv, shade)."""
    inside = cp[:, 2] >= near
    if inside.all():
        yield cp, uv, sh
        return
    poly = []
    for i in range(3):
        j = (i + 1) % 3
        if inside[i]:
            poly.append((cp[i], uv[i], sh[i]))
        if inside[i] != inside[j]:
            t = (near - cp[i, 2]) / (cp[j, 2] - cp[i, 2])
            poly.append((cp[i] + (cp[j] - cp[i]) * t, uv[i] + (uv[j] - uv[i]) * t, sh[i] + (sh[j] - sh[i]) * t))
    for k in range(1, len(poly) - 1):
        yield (np.array([poly[0][0], poly[k][0], poly[k + 1][0]]), np.array([poly[0][1], poly[k][1], poly[k + 1][1]]),
               np.array([poly[0][2], poly[k][2], poly[k + 1][2]]))


class Camera:
    def __init__(self, position, rotation_deg, fov_y_deg=60.0, near=0.1, far=300.0):
        self.pos = np.array(position, dtype=np.float64)
        self.r = euler_matrix(rotation_deg)  # camera -> world
        self.fov = math.radians(fov_y_deg)
        self.near, self.far = near, far

    @staticmethod
    def look_at(eye, target, fov_y_deg=60.0, up=(0, 1, 0), near=0.1, far=300.0):
        eye = np.array(eye, dtype=np.float64)
        f = np.array(target, dtype=np.float64) - eye
        f /= np.linalg.norm(f)
        yaw = math.degrees(math.atan2(f[0], f[2]))
        pitch = math.degrees(-math.asin(max(-1.0, min(1.0, f[1]))))
        return Camera(eye, (pitch, yaw, 0.0), fov_y_deg, near, far)


class Scene:
    def __init__(self, width=960, height=540, sky=((92, 140, 210), (200, 222, 245))):
        self.w, self.h = width, height
        self.items = []
        self.sky = sky

    def add(self, mesh_path, material_path, position=(0, 0, 0), rotation_deg=(0, 0, 0), scale=(1, 1, 1), cull_sphere=True):
        self.items.append((mesh_path, material_path, np.array(position, dtype=np.float64), tuple(rotation_deg),
                           np.array(scale, dtype=np.float64) if not np.isscalar(scale) else np.array([scale] * 3, dtype=np.float64),
                           cull_sphere))

    def render(self, cam, fog=None):
        w, h = self.w, self.h
        # sky gradient
        t = np.linspace(0, 1, h)[:, None, None]
        top, bot = np.array(self.sky[0], dtype=np.float64), np.array(self.sky[1], dtype=np.float64)
        img = (top * (1 - t) + bot * t) * np.ones((1, w, 1)) / 255.0
        zbuf = np.full((h, w), np.inf)
        focal = (h / 2.0) / math.tan(cam.fov / 2.0)
        inv_r = cam.r.T
        stats = {"draws": 0, "tris": 0, "culled_entities": 0}
        for mesh_path, mat_path, pos, rot, scl, cull in self.items:
            p, uv, n = load_obj(mesh_path)
            tex, tint = load_material(mat_path) if mat_path else (None, np.ones(3))
            rmat = euler_matrix(rot)
            # the renderer's culling sphere
            if cull:
                radius = CULL_RADIUS * float(np.max(np.abs(scl)))
                d = cam.pos - pos
                if np.linalg.norm(d) - radius > cam.far:
                    stats["culled_entities"] += 1
                    continue
            world = (p * scl) @ rmat.T + pos
            wn = n @ rmat.T
            cp = (world - cam.pos) @ inv_r.T  # camera space: x right, y up, z forward
            if np.all(cp[..., 2] < cam.near):  # entirely behind the camera
                stats["culled_entities"] += 1
                continue
            stats["draws"] += 1
            stats["tris"] += len(p)
            # front facing test in world space (right handed formula, see mesh.py)
            c = np.cross(world[:, 1] - world[:, 0], world[:, 2] - world[:, 0])
            to_cam = cam.pos - world[:, 0]
            front = np.einsum("ij,ij->i", c, to_cam) > 0.0
            # a mirrored entity scale flips the winding on the GPU; the engine scales positively, so nothing to do here
            lam = np.clip(-(wn @ LIGHT), 0.0, 1.0)
            shade = AMBIENT + (1.0 - AMBIENT) * lam  # per corner
            for t_i in np.nonzero(front)[0]:
                zc = cp[t_i, :, 2]
                if np.all(zc < cam.near) or np.all(zc > cam.far * 1.2):
                    continue
                for cpt, uvt, sht in _clip_near(cp[t_i], uv[t_i], shade[t_i], cam.near):
                    self._raster(img, zbuf, cpt, uvt, sht, tex, tint, focal, fog)
        self.stats = stats
        return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))

    def _raster(self, img, zbuf, cpt, uvt, sht, tex, tint, focal, fog):
        w, h = self.w, self.h
        if True:
            if True:
                z = cpt[:, 2]
                sx = w / 2.0 + focal * cpt[:, 0] / z
                sy = h / 2.0 - focal * cpt[:, 1] / z
                x0, x1 = int(max(0, math.floor(sx.min()))), int(min(w - 1, math.ceil(sx.max())))
                y0, y1 = int(max(0, math.floor(sy.min()))), int(min(h - 1, math.ceil(sy.max())))
                if x0 > x1 or y0 > y1:
                    return
                den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
                if abs(den) < 1e-12:
                    return
                xs = np.arange(x0, x1 + 1) + 0.5
                ys = np.arange(y0, y1 + 1) + 0.5
                gx, gy = np.meshgrid(xs, ys)
                l0 = ((sy[1] - sy[2]) * (gx - sx[2]) + (sx[2] - sx[1]) * (gy - sy[2])) / den
                l1 = ((sy[2] - sy[0]) * (gx - sx[2]) + (sx[0] - sx[2]) * (gy - sy[2])) / den
                l2 = 1.0 - l0 - l1
                inside = (l0 >= -1e-9) & (l1 >= -1e-9) & (l2 >= -1e-9)
                if not inside.any():
                    return
                inv_z = l0 / z[0] + l1 / z[1] + l2 / z[2]
                depth = 1.0 / np.where(inv_z > 0, inv_z, 1e-9)
                zt = zbuf[y0:y1 + 1, x0:x1 + 1]
                vis = inside & (depth < zt)
                if not vis.any():
                    return
                # perspective correct barycentrics
                b0 = l0 * depth / z[0]
                b1 = l1 * depth / z[1]
                b2 = l2 * depth / z[2]
                col = np.empty(vis.shape + (3,))
                if tex is not None:
                    u = b0 * uvt[0, 0] + b1 * uvt[1, 0] + b2 * uvt[2, 0]
                    v = b0 * uvt[0, 1] + b1 * uvt[1, 1] + b2 * uvt[2, 1]
                    th, tw = tex.shape[:2]
                    ix = np.floor((u % 1.0) * tw).astype(int) % tw
                    iy = np.floor((v % 1.0) * th).astype(int) % th
                    col = tex[iy, ix] * tint
                else:
                    col[:] = tint
                s = b0 * sht[0] + b1 * sht[1] + b2 * sht[2]
                col = col * s[..., None]
                if fog is not None:
                    f = np.clip((depth - fog[0]) / (fog[1] - fog[0]), 0.0, 1.0)[..., None] * fog[2]
                    col = col * (1 - f) + np.array(fog[3]) / 255.0 * f
                region = img[y0:y1 + 1, x0:x1 + 1]
                region[vis] = np.clip(col[vis], 0.0, 1.0)
                zt[vis] = depth[vis]
