"""Draws the icon of RocketArena.exe: app/resources/RocketArena.ico (16 .. 256 px, PNG frames inside).

The motif is the one of the game: the Pulse car (blue, side view, with a boost flame) and the orange ball on the navy of the menu. It is drawn at
1024 px and scaled down, so every size has the same picture. Run from the repository root:

    python tools/gen_rl_icon.py [--preview out.png]

Needs Pillow. The .ico is committed: the build does not run this script.
"""
import argparse
import math
import os

from PIL import Image, ImageDraw, ImageFilter

SIZE = 1024
BLUE = (0x1F, 0x8F, 0xFF)
BLUE_HI = (0x6C, 0xC0, 0xFF)
BLUE_DARK = (0x0B, 0x3F, 0x94)
ORANGE = (0xFF, 0x8A, 0x1E)
ORANGE_HI = (0xFF, 0xC0, 0x6A)
NAVY_TOP = (0x13, 0x2A, 0x66)
NAVY_BOTTOM = (0x07, 0x0F, 0x2B)


def gradient(size, top, bottom):
    image = Image.new("RGB", (size, size))
    pixels = image.load()
    for y in range(size):
        t = y / (size - 1)
        color = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(size):
            pixels[x, y] = color
    return image


def rounded_mask(size, radius):
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return mask


def glow(layer, color, radius, strength):
    """A soft halo of the opaque pixels of `layer` (RGBA), added under it."""
    alpha = layer.getchannel("A").filter(ImageFilter.GaussianBlur(radius))
    alpha = alpha.point(lambda v: int(v * strength))
    halo = Image.new("RGBA", layer.size, color + (0,))
    halo.putalpha(alpha)
    return halo


def draw_icon():
    base = gradient(SIZE, NAVY_TOP, NAVY_BOTTOM).convert("RGBA")

    # speed stripes of the menu (slanted parallelograms), very faint
    stripes = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    sd = ImageDraw.Draw(stripes)
    for i, (x, w) in enumerate(((120, 90), (330, 40), (500, 130), (780, 60))):
        sd.polygon([(x + 220, 0), (x + 220 + w, 0), (x + w, SIZE), (x, SIZE)], fill=(255, 255, 255, 14 + 6 * (i % 2)))
    base = Image.alpha_composite(base, stripes)

    # the ball: orange, with a darker rim, a highlight and the hexagon seam of the game ball
    ball = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    bd = ImageDraw.Draw(ball)
    cx, cy, r = 620, 400, 270
    for step in range(40):
        t = step / 39
        rr = int(r * (1 - 0.55 * t))
        color = tuple(int(ORANGE[i] * (1 - t) + ORANGE_HI[i] * t) for i in range(3))
        ox = int(-70 * t)
        oy = int(-80 * t)
        bd.ellipse((cx - rr + ox, cy - rr + oy, cx + rr + ox, cy + rr + oy), fill=color + (255,))
    bd.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(0xB8, 0x4A, 0x00, 255), width=14)
    hexagon = [(cx + 105 * math.cos(math.radians(a)), cy + 105 * math.sin(math.radians(a))) for a in range(30, 390, 60)]
    bd.polygon(hexagon, outline=(0xB8, 0x4A, 0x00, 255), width=12)
    for (x, y) in hexagon:
        bd.line((x, y, cx + (x - cx) * 2.1, cy + (y - cy) * 2.1), fill=(0xB8, 0x4A, 0x00, 255), width=12)
    base = Image.alpha_composite(base, glow(ball, ORANGE, 38, 0.8))
    base = Image.alpha_composite(base, ball)

    # the boost flame behind the car
    flame = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    fd = ImageDraw.Draw(flame)
    fd.polygon([(190, 705), (30, 640), (95, 700), (10, 745), (110, 760), (60, 830), (200, 780)], fill=ORANGE + (255,))
    fd.polygon([(190, 715), (95, 690), (140, 725), (85, 755), (170, 760)], fill=ORANGE_HI + (255,))
    base = Image.alpha_composite(base, glow(flame, ORANGE, 26, 0.9))
    base = Image.alpha_composite(base, flame)

    # the car (the side view of Pulse): body, cabin, windows, wheels
    car = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    cd = ImageDraw.Draw(car)
    body = [(170, 760), (150, 690), (200, 640), (360, 612), (455, 520), (650, 505), (760, 600), (905, 640), (950, 700), (935, 760)]
    cd.polygon(body, fill=BLUE + (255,), outline=BLUE_DARK + (255,), width=10)
    cd.polygon([(460, 535), (640, 522), (730, 604), (385, 618)], fill=BLUE_DARK + (255,))              # the window band
    cd.polygon([(480, 545), (560, 540), (545, 600), (440, 606)], fill=BLUE_HI + (200,))
    cd.line([(200, 690), (930, 700)], fill=BLUE_HI + (255,), width=12)                                   # the highlight along the flank
    cd.polygon([(850, 650), (935, 668), (938, 700), (850, 696)], fill=(255, 255, 255, 235))              # the headlight
    for wx in (330, 765):
        cd.ellipse((wx - 112, 668, wx + 112, 892), fill=(10, 14, 28, 255))
        cd.ellipse((wx - 62, 718, wx + 62, 842), fill=BLUE_HI + (255,))
        cd.ellipse((wx - 26, 754, wx + 26, 806), fill=(10, 14, 28, 255))
    base = Image.alpha_composite(base, glow(car, BLUE, 30, 0.7))
    base = Image.alpha_composite(base, car)

    # the floor line under the car
    floor = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(floor).rectangle((60, 905, SIZE - 60, 925), fill=BLUE + (150,))
    base = Image.alpha_composite(base, floor)

    base.putalpha(rounded_mask(SIZE, 210))
    return base


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", help="also write the 256 px picture as a PNG")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = os.path.join(root, "app", "resources", "RocketArena.ico")
    os.makedirs(os.path.dirname(target), exist_ok=True)

    master = draw_icon()
    sizes = [256, 128, 64, 48, 32, 24, 16]
    frames = [master.resize((s, s), Image.LANCZOS) for s in sizes]
    frames[0].save(target, format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[1:])
    print("wrote", target, os.path.getsize(target), "bytes")
    if args.preview:
        master.resize((256, 256), Image.LANCZOS).save(args.preview)


if __name__ == "__main__":
    main()
