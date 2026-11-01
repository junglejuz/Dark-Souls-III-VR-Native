"""Prove (or disprove) that the two eye pictures are a real stereo pair.

A pair is real only if the sideways offset GROWS with the eye separation and is larger near than far. One
viewpoint captured twice gives the same answer at every separation - that is how a brightness difference fooled
the built-in check on 2026-10-30. Each picture is normalised to zero mean and unit standard deviation first, so
a grade difference cannot drive the match.

Usage: python run_parallax.py           (the game must be in gameplay with 3D on, camera still)
"""
import glob
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVE = os.path.join(HERE, "ds_drive.py")
DATA = r"E:\Games\Steam\steamapps\common\DARK SOULS III\Game\DS3VR2"


def drive(*a):
    return subprocess.run([sys.executable, DRIVE, *a], capture_output=True, text=True).stdout.strip()


def shot_pair():
    drive("cmd", "ds capture")
    time.sleep(6)
    d = sorted(glob.glob(os.path.join(DATA, "captures", "2026*")))[-1]
    l = os.path.join(d, "eye_left.bmp")
    r = os.path.join(d, "eye_right.bmp")
    if not (os.path.exists(l) and os.path.exists(r)):
        return None
    return (np.asarray(Image.open(l).convert("L"), dtype=np.float32),
            np.asarray(Image.open(r).convert("L"), dtype=np.float32))


def best_shift(L, R, lo, hi, rng=260):
    h, w = L.shape
    band = slice(int(h * lo), int(h * hi))
    best = None
    for s in range(-rng, rng + 1, 2):
        if s >= 0:
            a, b = L[band, s:], (R[band, :w - s] if s else R[band, :])
        else:
            a, b = L[band, :w + s], R[band, -s:]
        e = float(np.mean((a - b) ** 2))
        if best is None or e < best[1]:
            best = (s, e)
    return best


def nz(a):
    return (a - a.mean()) / (a.std() + 1e-6)


def main():
    print("eye separation   sky        middle     near ground   (sideways offset, px)")
    for mult, metres in ((1.0, 0.064), (2.0, 0.13), (4.0, 0.26), (8.0, 0.51)):
        drive("cmd", "ds ipd %.2f" % mult)
        time.sleep(4)                      # let a few frames go by at the new separation
        p = shot_pair()
        if not p:
            print("x%-4.1f  no eye captures" % mult)
            continue
        L, R = nz(p[0]), nz(p[1])
        out = []
        for lo, hi in ((0.02, 0.25), (0.35, 0.55), (0.70, 0.95)):
            s, e = best_shift(L, R, lo, hi)
            out.append("%+5d (%.2f)" % (s, e))
        print("x%-4.1f (%.2f m)  %s" % (mult, metres, "  ".join(out)))
    drive("cmd", "ds ipd 1.00")
    return 0


if __name__ == "__main__":
    sys.exit(main())
