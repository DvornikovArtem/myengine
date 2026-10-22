"""Command line of the RL asset pack:  python tools/gen_rl_assets.py pack [--only car,ball,arena,stadium] [--preview DIR] [--no-scene]"""

import argparse
import json
import sys

from . import build, common
from .common import ROOT


def budgets(ctx, items):
    """Checks the numbers of the task card; returns (rows, ok)."""
    m = ctx.meshes
    tris = lambda *names: sum(m[n]["triangles"] for n in names if n in m)
    rows = []

    def row(name, value, limit, unit=""):
        rows.append((name, value, limit, value <= limit, unit))
    if "car_body" in m:
        row("car triangles (body + lights + 4 wheels)", tris("car_body", "car_lights") + 4 * tris("car_wheel"), 4000)
    if "ball" in m:
        row("ball triangles", tris("ball"), 2000)
    if "arena_floor" in m:
        row("arena visual triangles (without pads)", tris("arena_floor", "arena_wall_blue", "arena_wall_orange", "arena_frame_blue",
                                                         "arena_frame_orange", "arena_goal_blue", "arena_goal_orange"), 60000)
    for n in ("stand_blue", "stand_orange", "stand_mixed"):
        if n in m:
            row("%s triangles" % n, tris(n), 20000)
    if items:
        row("entities of the showcase scene (draw calls without instancing)", len(items), 150)
        row("draw calls with instancing (mesh + material groups)", len({(i["mesh"], i["material"]) for i in items}), 60)
    tex_max = max((max(t["size"]) for t in ctx.textures.values()), default=0)
    row("largest texture side", tex_max, 2048, "px")
    total = sum(t["bytes"] for t in ctx.textures.values()) + sum(i["bytes"] for i in m.values())
    row("new assets total", round(total / 1048576.0, 2), 60, "MB")
    row("mesh radius in the unit space (limit 0.87 of the culling sphere)", max((i["radius"] for i in m.values()), default=0), 0.8501)
    bad = [n for n, i in m.items() if i["orientation"] < 0.999]
    row("meshes with a winding that disagrees with normals", len(bad), 0)
    return rows, all(r[3] for r in rows)


def main(argv):
    ap = argparse.ArgumentParser(prog="gen_rl_assets.py pack", description=__doc__)
    ap.add_argument("--only", help="comma separated parts: car, ball, arena, stadium, podium (default: all)")
    ap.add_argument("--preview", metavar="DIR", help="also render the PNG previews (software rasterizer, needs numpy) into DIR")
    ap.add_argument("--no-scene", action="store_true", help="do not write assets/scenes/rl_showcase.json and assets/scenes/rocket_league.json")
    args = ap.parse_args(argv)

    print("RL asset pack -> assets/{models,textures,materials}/%s" % common.PACK)
    parts = args.only.split(",") if args.only else None
    ctx = build.run(parts)
    tuning = json.loads((ROOT / "assets" / "data" / "rl_tuning.json").read_text(encoding="utf-8"))
    items = None
    if parts is None:
        from . import scene
        items = scene.showcase(ctx.meshes, tuning)
        if not args.no_scene:
            n = scene.write_scene(items)
            print("  scene assets/scenes/rl_showcase.json: %d entities" % n)
            from . import game_scene
            n = game_scene.write_game_scene(game_scene.game_items(ctx.meshes, tuning))
            print("  scene assets/scenes/rocket_league.json: %d entities" % n)
    rows, ok = budgets(ctx, items)
    print("budgets")
    for name, value, limit, good, unit in rows:
        print("  %-62s %8s / %-6s %s %s" % (name, value, limit, unit, "ok" if good else "OVER"))
    path = common.MODEL_DIR / "manifest.json"
    manifest = json.loads(path.read_text(encoding="utf-8")) if parts and path.exists() else {"meshes": {}, "textures": {}}
    manifest["pack"] = common.PACK
    manifest["meshes"].update(ctx.meshes)
    manifest["textures"].update(ctx.textures)
    manifest["materials"] = sorted(p.name[:-len(".material.json")] for p in common.MAT_DIR.glob("*.material.json"))
    if items:
        manifest["showcase_draw_calls"] = len(items)
    path.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    if args.preview:
        from . import preview
        print("previews -> %s" % args.preview)
        its = preview.run(ctx, tuning, args.preview)
        preview.extra(ctx, tuning, args.preview, its)
        preview.texture_sheet(args.preview)
    return 0 if ok else 1
