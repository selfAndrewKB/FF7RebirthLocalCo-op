"""Find detail pops in a recording of a turning camera.

During a turn, distant scenery only slides sideways. For each pair of frames this estimates that slide
in a band of one half (phase correlation), aligns the earlier frame to the later one and measures what
is left: content that changed by itself (a level-of-detail switch, detail streaming in or out) rather
than by the camera's motion. Nearby objects move by parallax and leave some residual too, so choose a
band with distant scenery.

    python tools/pop_scan.py VIDEO --out DIR [--band y0 y1] [--x x0 x1] [--top 8]

Coordinates are in the video's pixels. Writes DIR/scores.csv (frame, shift, residual) and, for the
`--top` frames with the largest residual, DIR/pop-NNNN.png: the aligned previous frame, the frame, and
the residual (amplified), stacked.
"""
import argparse
from pathlib import Path

import cv2
import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('video')
    parser.add_argument('--out', required=True)
    parser.add_argument('--band', type=int, nargs=2, default=[760, 1100], help='rows y0 y1')
    parser.add_argument('--x', type=int, nargs=2, default=[0, 2560], help='columns x0 x1')
    parser.add_argument('--top', type=int, default=8)
    parser.add_argument('--threshold', type=float, default=24.0, help='per-pixel residual that counts')
    args = parser.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    capture = cv2.VideoCapture(args.video)
    y0, y1 = args.band
    x0, x1 = args.x
    previous, previous_color = None, None
    rows, kept = [], []
    index = 0
    window = None
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        color = frame[y0:y1, x0:x1]
        gray = cv2.cvtColor(color, cv2.COLOR_BGR2GRAY).astype(np.float32)
        # Fine detail only: exposure and sky changes are smooth, detail switches are not.
        gray = gray - cv2.GaussianBlur(gray, (0, 0), 6)
        if window is None:
            window = cv2.createHanningWindow((gray.shape[1], gray.shape[0]), cv2.CV_32F)
        if previous is not None:
            (dx, dy), response = cv2.phaseCorrelate(previous, gray, window)
            shift = np.float32([[1, 0, dx], [0, 1, dy]])
            aligned = cv2.warpAffine(previous, shift, (gray.shape[1], gray.shape[0]), borderMode=cv2.BORDER_REPLICATE)
            residual = np.abs(gray - aligned)
            margin = int(abs(dx)) + 4  # the edge the slide brought in has nothing to compare with
            core = residual[:, margin:-margin] if margin * 2 < residual.shape[1] else residual
            share = float((core > args.threshold).mean())
            rows.append((index, round(dx, 2), round(dy, 2), round(float(response), 3), round(share * 100, 3)))
            kept.append((share, index, previous_color, shift, color, residual))
            kept.sort(key=lambda item: -item[0])
            del kept[args.top:]
        previous, previous_color = gray, color
        index += 1

    with open(out / 'scores.csv', 'w') as f:
        f.write('frame,dx,dy,response,residual_pct\n')
        for row in rows:
            f.write(','.join(str(v) for v in row) + '\n')
    for share, i, prev_color, shift, color, residual in kept:
        aligned = cv2.warpAffine(prev_color, shift, (color.shape[1], color.shape[0]), borderMode=cv2.BORDER_REPLICATE)
        heat = cv2.applyColorMap(np.minimum(residual * 3, 255).astype(np.uint8), cv2.COLORMAP_INFERNO)
        sheet = np.vstack([aligned, color, heat])
        cv2.imwrite(str(out / f'pop-{i:04d}.png'), cv2.resize(sheet, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA))
    residuals = np.array([r[4] for r in rows])
    print(f'{len(rows)} frame pairs; residual % mean {residuals.mean():.3f}, p95 {np.percentile(residuals, 95):.3f}, '
          f'max {residuals.max():.3f}; top frames: {[k[1] for k in kept]}')


if __name__ == '__main__':
    main()
