"""PNG previews of the pack with the software rasterizer (raster.py): the views a reviewer needs without starting the engine."""

import time
from pathlib import Path

from PIL import Image, ImageDraw

from . import raster as R
from .common import ROOT, TEX_DIR
from .scene import car as place_car, Entities, showcase

NIGHT = ((10, 14, 30), (60, 78, 120))


def scene_from(items, w, h, sky=NIGHT):
    s = R.Scene(w, h, sky=sky)
    for it in items:
        s.add(ROOT / it["mesh"], ROOT / it["material"], it["position"], it["rotation"], it["scale"])
    return s


def shot(items, cam, w, h, path, sky=NIGHT, only=None):
    t = time.time()
    its = [i for i in items if only is None or only(i["name"])]
    s = scene_from(its, w, h, sky)
    img = s.render(cam)
    img.save(path)
    print("  %-34s %dx%d  %d draws %d tris  %.1f s" % (Path(path).name, w, h, s.stats["draws"], s.stats["tris"], time.time() - t))
    return img


def label(img, text):
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 8 + 7 * len(text), 16], fill=(0, 0, 0))
    d.text((4, 2), text, fill=(255, 255, 255))
    return img


def run(ctx, tuning, out_dir):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    items = showcase(ctx.meshes, tuning)
    ents = Entities(ctx.meshes)
    # the car on its own
    car_items = Entities(ctx.meshes)
    place_car(car_items, "blue", (0.0, 0.0, 0.0), 0.0)
    place_car(car_items, "orange", (0.0, 0.0, 0.0), 0.0, boost=True)
    blue = [i for i in car_items.items if "orange" not in i["name"]]
    orange = [i for i in car_items.items if "blue" not in i["name"]]
    sheets = []
    views = [("3/4 front", (1.5, 0.75, 2.0)), ("3/4 rear", (-1.5, 0.7, -2.1)), ("side", (2.7, 0.05, 0.0)), ("top", (0.001, 2.8, -0.9))]
    for team, its in (("blue", blue), ("orange", orange)):
        row = []
        for name, eye in views:
            cam = R.Camera.look_at(eye, (0, -0.05, 0.0), 34)
            s = scene_from(its, 480, 300, ((26, 32, 52), (92, 104, 132)))
            row.append(label(s.render(cam), "%s %s" % (team, name)))
        sheets.append(row)
    sheet = Image.new("RGB", (4 * 480, 2 * 300))
    for r, row in enumerate(sheets):
        for c, im in enumerate(row):
            sheet.paste(im, (c * 480, r * 300))
    sheet.save(out / "04_cars.png")
    print("  04_cars.png")
    # the ball
    s = R.Scene(640, 420, sky=((26, 32, 52), (92, 104, 132)))
    info = ctx.meshes["ball"]
    s.add(ROOT / "assets/models/rl2/ball.obj", ROOT / "assets/materials/rl2/ball.material.json", (0, 0, 0), (0, 0, 0), (info["scale"],) * 3)
    s.render(R.Camera.look_at((0.0, 1.0, -3.1), (0, 0, 0), 34)).save(out / "05_ball.png")
    print("  05_ball.png")
    # the whole scene
    shot(items, R.Camera((0.0, 16.0, -48.0), (22.0, 0.0, 0.0), 95.0), 1280, 720, out / "01_arena_game_camera.png")
    shot(items, R.Camera.look_at((-30, 13, -32), (14, 3, 26), 75), 1280, 720, out / "02_arena_corner.png")
    shot(items, R.Camera.look_at((0, 15, 40), (-6, 5, -30), 80), 1280, 720, out / "03_arena_from_orange.png")
    return items


def extra(ctx, tuning, out_dir, items):
    """Close-ups: the crowd, a goal, the pads, the stands from the field and the textures contact sheet."""
    out = Path(out_dir)
    # the crowd: a camera low on the field looking at the stands of the +x side
    shot(items, R.Camera.look_at((30, 11.0, -14), (52, 5.0, -4), 62), 1280, 720, out / "06_crowd_closeup.png")
    # a goal from in front of it
    shot(items, R.Camera.look_at((0, 3.2, 30), (0, 2.8, 56), 66), 1280, 720, out / "07_goal_orange.png")
    # the pads and an orb
    shot(items, R.Camera.look_at((28.5, 3.2, -32.5), (35.9, 0.5, -24.8), 55), 1280, 720, out / "08_pads_and_orb.png")
    # the stands and the scoreboard from the top of the field
    shot(items, R.Camera.look_at((0, 12, -30), (0, 18, -75), 70), 1280, 720, out / "09_scoreboard_end.png")
    # a kickoff like moment: the game camera behind the blue car
    cam_target = (-6.0, 1.0, -9.0)
    shot(items, R.Camera.look_at((-10.5, 4.0, -18.0), (-4.0, 1.0, -3.0), 70), 1280, 720, out / "10_car_and_ball_game_view.png")


def texture_sheet(out_dir, names=None, cell=256):
    from . import common
    paths = sorted(common.TEX_DIR.glob("*.tga"))
    thumbs = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        im.thumbnail((cell, cell))
        t = Image.new("RGB", (cell, cell + 16), (24, 24, 28))
        t.paste(im, ((cell - im.width) // 2, 0))
        ImageDraw.Draw(t).text((4, cell + 2), "%s %dx%d" % (p.stem, *Image.open(p).size), fill=(255, 255, 255))
        thumbs.append(t)
    cols = 6
    rows = (len(thumbs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * (cell + 16)), (24, 24, 28))
    for i, t in enumerate(thumbs):
        sheet.paste(t, ((i % cols) * cell, (i // cols) * (cell + 16)))
    sheet.save(Path(out_dir) / "11_textures.png")
    print("  11_textures.png (%d textures)" % len(thumbs))
