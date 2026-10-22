#!/usr/bin/env python3
"""Cuts the static styles of Exo 2 for the UI kit of the Rocket League game (docs/game/rl-ui.md, .team/design/rl-ui-spec.md section 4.3).

Exo 2 (Natanael Gama, SIL Open Font License 1.1) is published in github.com/google/fonts, folder ofl/exo2, as two variable fonts
(Exo2[wght].ttf and Exo2-Italic[wght].ttf, the axis wght 100..900). ImGui bakes one style of a variable font, so the styles of the spec are cut
once with fontTools (varLib.instancer) and written to assets/fonts/exo2/:

    Exo2-BlackItalic.ttf       900 italic: the digits, the headlines, the banners
    Exo2-ExtraBoldItalic.ttf   800 italic: the buttons, the tabs
    Exo2-Bold.ttf              700: the plates, the feed
    Exo2-SemiBold.ttf          600: the text of the rows
    OFL.txt                    the license (Exo 2 has no Reserved Font Name, the instances may keep the name)

Run from anywhere:  python tools/make_exo2.py [--source <folder with the two variable fonts and OFL.txt>]
Without --source the files are downloaded from raw.githubusercontent.com into a temporary folder that is removed afterwards.
Needs fontTools:  pip install fonttools
"""

import argparse
import shutil
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "fonts" / "exo2"
BASE_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/exo2/"
SOURCES = ["Exo2[wght].ttf", "Exo2-Italic[wght].ttf", "OFL.txt"]

# (output file, source variable font, weight)
STYLES = [
    ("Exo2-BlackItalic.ttf", "Exo2-Italic[wght].ttf", 900),
    ("Exo2-ExtraBoldItalic.ttf", "Exo2-Italic[wght].ttf", 800),
    ("Exo2-Bold.ttf", "Exo2[wght].ttf", 700),
    ("Exo2-SemiBold.ttf", "Exo2[wght].ttf", 600),
]


def download(folder: Path) -> None:
    for name in SOURCES:
        url = BASE_URL + urllib.parse.quote(name)
        print("  downloading", url)
        request = urllib.request.Request(url, headers={"User-Agent": "myengine-make-exo2"})
        with urllib.request.urlopen(request, timeout=60) as response:
            (folder / name).write_bytes(response.read())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, help="a folder that already has the variable fonts and OFL.txt")
    args = parser.parse_args()

    temporary = None
    source = args.source
    if source is None:
        temporary = Path(tempfile.mkdtemp(prefix="exo2_"))
        source = temporary
        download(source)
    try:
        for name in SOURCES:
            if not (source / name).is_file():
                print("missing", source / name)
                return 1
        OUT.mkdir(parents=True, exist_ok=True)
        total = 0
        for output, variable, weight in STYLES:
            font = TTFont(source / variable)
            instance = instancer.instantiateVariableFont(font, {"wght": weight})
            path = OUT / output
            instance.save(path)
            total += path.stat().st_size
            print("  %-26s wght %d  %6.1f KB" % (output, weight, path.stat().st_size / 1024.0))
        shutil.copyfile(source / "OFL.txt", OUT / "OFL.txt")
        print("total %.2f MB in %d fonts" % (total / 1048576.0, len(STYLES)))
    finally:
        if temporary is not None:
            shutil.rmtree(temporary, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
