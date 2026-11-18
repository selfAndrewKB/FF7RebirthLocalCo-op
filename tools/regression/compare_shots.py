"""Compares two pictures of the game window taken by the regression's `# shot:` lines (tools/regression/run.py), one
per build, e.g. the last commit's build against a change's:

    python tools/regression/compare_shots.py before.png after.png [--out DIR] [--layout side-by-side|top-bottom]

The pictures are split into the two players' views (side by side by default, the mod's default layout, the one the
scripts leave the game in). For each view, and for the HUD's own areas in it (its four corner bands, where the minimap, the party and the
prompts sit), it reports the mean difference of the pixels (0 to 255) and the share of pixels that differ by more than
a threshold. It writes a side-by-side picture and a difference picture (amplified) per view to look at. The 3D scene
moves on its own between two launches (idle animations, effects), so a small difference is expected everywhere; a HUD
element missing or drawn in the other view shows as a large difference in one band of one view.
"""
import argparse
import pathlib
import sys

from PIL import Image, ImageChops, ImageOps

THRESHOLD = 40  # a pixel "differs" when a channel differs by more than this


def views(image: Image.Image, layout: str) -> list[tuple[str, Image.Image]]:
    w, h = image.size
    if layout == "side-by-side":
        return [("player 1 (left)", image.crop((0, 0, w // 2, h))), ("player 2 (right)", image.crop((w // 2, 0, w, h)))]
    return [("player 1 (top)", image.crop((0, 0, w, h // 2))), ("player 2 (bottom)", image.crop((0, h // 2, w, h)))]


def bands(view: Image.Image) -> list[tuple[str, tuple[int, int, int, int]]]:
    """The HUD's areas of a view: a quarter of the width by a third of the height in each corner."""
    w, h = view.size
    bw, bh = w // 4, h // 3
    return [("top left", (0, 0, bw, bh)), ("top right", (w - bw, 0, w, bh)),
            ("bottom left", (0, h - bh, bw, h)), ("bottom right", (w - bw, h - bh, w, h))]


def measure(a: Image.Image, b: Image.Image) -> tuple[float, float]:
    diff = ImageChops.difference(a, b).convert("L")
    pixels = list(diff.getdata())
    mean = sum(pixels) / len(pixels)
    share = sum(1 for p in pixels if p > THRESHOLD) / len(pixels)
    return mean, share


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("before", type=pathlib.Path)
    parser.add_argument("after", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, default=None, help="where the pictures go (beside `after`)")
    parser.add_argument("--layout", choices=("top-bottom", "side-by-side"), default="side-by-side")
    args = parser.parse_args()
    before = Image.open(args.before).convert("RGB")
    after = Image.open(args.after).convert("RGB")
    if before.size != after.size:
        print(f"the pictures differ in size: {before.size} and {after.size}")
        return 2
    out = args.out or args.after.parent
    out.mkdir(parents=True, exist_ok=True)
    stem = args.after.stem
    for (name, a), (_, b) in zip(views(before, args.layout), views(after, args.layout)):
        mean, share = measure(a, b)
        print(f"{name}: whole view mean {mean:.1f}, {share:.1%} of pixels differ")
        for band, box in bands(a):
            bmean, bshare = measure(a.crop(box), b.crop(box))
            print(f"    {band}: mean {bmean:.1f}, {bshare:.1%} differ")
        tag = name.split(" ")[1]
        pair = Image.new("RGB", (a.width * 2, a.height))
        pair.paste(a, (0, 0))
        pair.paste(b, (a.width, 0))
        pair.save(out / f"{stem}-view{tag}-before-after.png")
        diff = ImageOps.autocontrast(ImageChops.difference(a, b).convert("L"))
        diff.save(out / f"{stem}-view{tag}-difference.png")
    print(f"pictures: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())