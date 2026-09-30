"""How often each pixel of a still view changes over a recording, for comparing runs.

The camera of the region scanned must hold still. Each frame is compared with the one before it on
fine detail (smooth exposure changes removed); a pixel counts as changed when that difference exceeds
--threshold (above the anti-aliasing's sub-pixel jitter). The map of counts shows what keeps changing:
wind and characters in every run, and anything else only where something acts on it.

    python tools/change_map.py OUT.png RUN.mp4 [RUN2.mp4 ...] [--rows y0 y1] [--threshold 40]

Writes one heat map per run, stacked in the order given, labelled with the run's total changes.
"""
import argparse
from pathlib import Path

import cv2
import numpy as np


def counts(video, y0, y1, threshold):
    capture = cv2.VideoCapture(video)
    previous, total, frames = None, None, 0
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        gray = cv2.cvtColor(frame[y0:y1], cv2.COLOR_BGR2GRAY).astype(np.float32)
        gray = gray - cv2.GaussianBlur(gray, (0, 0), 6)
        if previous is not None:
            changed = (np.abs(gray - previous) > threshold).astype(np.uint16)
            total = changed if total is None else total + changed
        previous = gray
        frames += 1
        if frames == 2:
            first = frame[y0:y1].copy()
    return total, frames, first


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out')
    parser.add_argument('videos', nargs='+')
    parser.add_argument('--rows', type=int, nargs=2, default=[740, 1440])
    parser.add_argument('--threshold', type=float, default=40.0)
    args = parser.parse_args()
    sheets = []
    for video in args.videos:
        total, frames, first = counts(video, args.rows[0], args.rows[1], args.threshold)
        heat = cv2.applyColorMap(np.minimum(total * 8, 255).astype(np.uint8), cv2.COLORMAP_INFERNO)
        sheet = cv2.addWeighted(first // 3, 1.0, heat, 1.0, 0)
        label = f'{Path(video).stem}: {int(total.sum())} changes over {frames} frames'
        cv2.putText(sheet, label, (12, 40), cv2.FONT_HERSHEY_SIMPLEX, 1.2, (0, 255, 255), 3)
        sheets.append(cv2.resize(sheet, None, fx=0.5, fy=0.5, interpolation=cv2.INTER_AREA))
        print(label)
    cv2.imwrite(args.out, np.vstack(sheets))


if __name__ == '__main__':
    main()
