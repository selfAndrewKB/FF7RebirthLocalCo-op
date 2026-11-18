"""Turn a player's camera away and back, then measure how the view differs from before the turn.

Detail the renderer dropped while the camera looked away streams back in after the return: the
difference from the reference frame then starts high and decays. With identical inputs the camera
comes back to (almost) the same heading, so a scene that kept its detail matches the reference at once.

    python tools/spin_return.py --out DIR --player 2 [--ticks 40] [--rate 1] [--frames 40] [--layout side-by-side]

Needs the mod's console (tools/remote.py), per-pad sticks, and player 2 in. Writes
DIR/reference.png, DIR/after-NN.png (a few), DIR/diff-first.png (where the first frame after the return
differs, brighter = more) and DIR/result.json with each half's mean difference per frame.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

import cv2
import numpy as np

import flicker

TOOLS = Path(__file__).resolve().parent


def run_control(*commands):
    args = [sys.executable, str(TOOLS / 'remote.py'), '--timeout', '60']
    for command in commands:
        args += ['-c', command]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(result.stdout + result.stderr)


def half_means(diff, layout):
    rows, cols = diff.shape
    out = {}
    for name, (rs, cs) in flicker.halves((rows, cols), layout).items():
        out[name] = round(float(diff[rs, cs].mean()), 2)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True)
    parser.add_argument('--player', type=int, default=2)
    parser.add_argument('--ticks', type=int, default=40)
    parser.add_argument('--rate', type=float, default=1.0)
    parser.add_argument('--frames', type=int, default=40)
    parser.add_argument('--settle', type=int, default=90, help='ticks to wait after the return')
    parser.add_argument('--layout', default='side-by-side', choices=['top-bottom', 'side-by-side', 'solo'])
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    box = flicker.client_box()

    # The reference: the mean of a few frames, so the scene's own motion is averaged in.
    reference = np.mean([flicker.grab(box) for _ in range(5)], axis=0)
    p, r, t = args.player, args.rate, args.ticks
    run_control(f'hold {p} stick 0 0 {r} 0 {t}', f'wait {t + 2}', f'hold {p} stick 0 0 {-r} 0 {t}',
                f'wait {t + args.settle}')

    timeline, frames = [], []
    start = time.perf_counter()
    for _ in range(args.frames):
        frame = flicker.grab(box)
        frames.append(frame)
        timeline.append({'s': round(time.perf_counter() - start, 2),
                         **half_means(np.abs(frame - reference), args.layout)})
    cv2.imwrite(str(out / 'reference.png'), reference.astype(np.uint8))
    for i in (0, len(frames) // 2, len(frames) - 1):
        cv2.imwrite(str(out / f'after-{i:02d}.png'), frames[i].astype(np.uint8))
    first = np.abs(frames[0] - reference)
    heat = cv2.cvtColor((reference // 2).astype(np.uint8), cv2.COLOR_GRAY2BGR)
    heat[..., 2] = np.maximum(heat[..., 2], np.minimum(first * 4, 255).astype(np.uint8))
    cv2.imwrite(str(out / 'diff-first.png'), heat)
    result = {'player': p, 'ticks': t, 'rate': r, 'settle_ticks': args.settle, 'timeline': timeline}
    (out / 'result.json').write_text(json.dumps(result, indent=1))
    for row in timeline[:: max(1, len(timeline) // 10)]:
        print(row)


if __name__ == '__main__':
    main()
