"""Draws the icon of FlyDoom.exe: app/resources/FlyDoom.ico (16 .. 256 px, PNG frames inside).

The motif: a fly (the mascot of the game: the brain of a fruit fly plays DOOM) in pixel art on the red of a DOOM wall. It is drawn on a 32 x 32 grid and
enlarged without smoothing, so every size keeps the same square pixels. Run from the repository root:

    python tools/gen_flydoom_icon.py [--preview out.png]

Needs Pillow. The .ico is committed: the build does not run this script.
"""
import argparse
import os

from PIL import Image, ImageDraw

GRID = 32

WALL_TOP = (0x5A, 0x12, 0x0E)
WALL_BOTTOM = (0x1E, 0x04, 0x04)
BRICK = (0x7C, 0x1C, 0x14)
BODY = (0x20, 0x20, 0x26)
BODY_HI = (0x4A, 0x4A, 0x55)
EYE = (0xE0, 0x24, 0x1C)
EYE_HI = (0xFF, 0x8A, 0x70)
EYE_DARK = (0x80, 0x0C, 0x0C)
WING = (0xC8, 0xD8, 0xE8)
WING_EDGE = (0x7C, 0x90, 0xA8)
GOLD = (0xF2, 0xC0, 0x30)


def draw_grid():
    image = Image.new("RGB", (GRID, GRID))
    px = image.load()
    for y in range(GRID):
        t = y / (GRID - 1)
        color = tuple(int(WALL_TOP[i] + (WALL_BOTTOM[i] - WALL_TOP[i]) * t) for i in range(3))
        for x in range(GRID):
            px[x, y] = color
    d = ImageDraw.Draw(image)

    # bricks of the wall
    for row in range(0, GRID, 4):
        d.line([(0, row), (GRID - 1, row)], fill=BRICK)
        offset = 0 if (row // 4) % 2 == 0 else 5
        for x in range(offset, GRID, 10):
            d.line([(x, row), (x, row + 3)], fill=BRICK)

    # wings
    d.polygon([(4, 14), (9, 5), (14, 12), (14, 18)], fill=WING, outline=WING_EDGE)
    d.polygon([(28, 14), (23, 5), (18, 12), (18, 18)], fill=WING, outline=WING_EDGE)

    # body
    d.ellipse([10, 16, 21, 29], fill=BODY, outline=BODY_HI)
    d.line([(12, 21), (19, 21)], fill=BODY_HI)
    d.line([(12, 24), (19, 24)], fill=BODY_HI)

    # head with the big compound eyes
    d.ellipse([8, 7, 23, 20], fill=BODY, outline=BODY_HI)
    d.ellipse([7, 8, 15, 18], fill=EYE, outline=EYE_DARK)
    d.ellipse([16, 8, 24, 18], fill=EYE, outline=EYE_DARK)
    for (cx, cy) in ((10, 11), (19, 11)):
        d.rectangle([cx, cy, cx + 1, cy + 1], fill=EYE_HI)
    for (cx, cy) in ((11, 15), (20, 15), (13, 13), (18, 13)):
        d.point((cx, cy), fill=EYE_DARK)

    # a muzzle flash: the fly shoots
    d.rectangle([14, 20, 17, 21], fill=GOLD)
    d.point((15, 22), fill=GOLD)
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", help="also write a 256 px PNG")
    args = parser.parse_args()

    big = draw_grid().resize((256, 256), Image.NEAREST)
    out = os.path.join("app", "resources", "FlyDoom.ico")
    big.save(out, format="ICO", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("wrote", out)
    if args.preview:
        big.save(args.preview)


if __name__ == "__main__":
    main()
