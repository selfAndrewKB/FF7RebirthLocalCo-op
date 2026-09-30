"""Capture a burst of frames from the game window and measure how much each part keeps changing.

With both cameras held still, the world should settle: a tile that keeps changing from frame to frame
is detail loading and unloading (or an animation: water, foliage, characters). Comparing the score of
each half between setups (player 2 on their own camera or on player 1's, a console setting on or off)
separates the causes.

    python tools/flicker.py --out DIR [--frames 90] [--layout top-bottom|side-by-side|solo] [--skip 20]

Writes DIR/first.png, DIR/last.png, DIR/heat.png (the per-tile change count, brighter = more changes,
over the first frame) and DIR/result.json; prints each half's score. --skip drops the first frames
(the camera settling after a spin).
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import time

import cv2
import numpy as np
from PIL import ImageGrab

GAME_TITLE = 'FINAL FANTASY VII REBIRTH'
TILE = 16            # pixels at the analysis width
WIDTH = 960          # analysis width
CHANGE = 12.0        # mean absolute difference (0..255) of a tile between two frames that counts as a change


def client_box():
    u = C.WinDLL('user32', use_last_error=True)
    hwnd = u.FindWindowW(None, GAME_TITLE)
    if not hwnd:
        raise SystemExit(f"no window titled '{GAME_TITLE}'")
    rect = W.RECT()
    u.GetClientRect(hwnd, C.byref(rect))
    origin = W.POINT(0, 0)
    u.ClientToScreen(hwnd, C.byref(origin))
    return (origin.x, origin.y, origin.x + rect.right, origin.y + rect.bottom)


def grab(box):
    image = np.asarray(ImageGrab.grab(bbox=box, all_screens=True).convert('L'), dtype=np.float32)
    height = int(image.shape[0] * WIDTH / image.shape[1])
    return cv2.resize(image, (WIDTH, height), interpolation=cv2.INTER_AREA)


def tiles(frame):
    h, w = (frame.shape[0] // TILE) * TILE, (frame.shape[1] // TILE) * TILE
    return frame[:h, :w].reshape(h // TILE, TILE, w // TILE, TILE).mean(axis=(1, 3))


def halves(shape, layout):
    rows, cols = shape
    if layout == 'side-by-side':
        return {'player 1 (left)': (slice(None), slice(0, cols // 2)), 'player 2 (right)': (slice(None), slice(cols // 2, None))}
    if layout == 'top-bottom':
        return {'player 1 (top)': (slice(0, rows // 2), slice(None)), 'player 2 (bottom)': (slice(rows // 2, None), slice(None))}
    return {'whole screen': (slice(None), slice(None))}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True)
    parser.add_argument('--frames', type=int, default=90)
    parser.add_argument('--skip', type=int, default=0)
    parser.add_argument('--layout', default='top-bottom', choices=['top-bottom', 'side-by-side', 'solo'])
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    box = client_box()
    for _ in range(args.skip):
        grab(box)
    frames, times = [], []
    start = time.perf_counter()
    for _ in range(args.frames):
        frames.append(grab(box))
        times.append(time.perf_counter() - start)
    stack = np.stack([tiles(f) for f in frames])                 # (n, rows, cols) tile means
    diffs = np.abs(np.diff(stack, axis=0))                      # (n-1, rows, cols)
    # A tile's change between frames, measured on its pixels (not only its mean).
    pixel_diffs = [np.abs(frames[i + 1] - frames[i]) for i in range(len(frames) - 1)]
    tile_changes = np.stack([tiles(d) for d in pixel_diffs]) > CHANGE
    counts = tile_changes.sum(axis=0)                           # per tile: frames in which it changed

    result = {'frames': len(frames), 'seconds': round(times[-1], 2), 'fps': round(len(frames) / times[-1], 1),
              'layout': args.layout, 'tile': TILE, 'threshold': CHANGE, 'halves': {}}
    for name, (rs, cs) in halves(counts.shape, args.layout).items():
        part = counts[rs, cs]
        result['halves'][name] = {
            'changing tiles %': round(100.0 * float((part > 0).mean()), 2),
            'flickering tiles % (changed in >=3 frames)': round(100.0 * float((part >= 3).mean()), 2),
            'changes per tile per frame': round(float(part.mean()) / max(1, len(frames) - 1), 4),
            'max tile mean drift': round(float(diffs[:, rs, cs].max()), 1),
        }
    first = frames[0].astype(np.uint8)
    cv2.imwrite(str(out / 'first.png'), first)
    cv2.imwrite(str(out / 'last.png'), frames[-1].astype(np.uint8))
    heat = cv2.resize((np.minimum(counts, 10) * 25).astype(np.uint8), (first.shape[1], first.shape[0]),
                      interpolation=cv2.INTER_NEAREST)
    overlay = cv2.cvtColor(first // 2, cv2.COLOR_GRAY2BGR)
    overlay[..., 2] = np.maximum(overlay[..., 2], heat)
    cv2.imwrite(str(out / 'heat.png'), overlay)
    (out / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
