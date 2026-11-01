"""M3c: teach the second view the lighting the game view gets, then measure whether it worked.

Sekiro's RTCLASS method (SKVR2 L1): watch every 8-byte field of the two views' settings objects for a few
hundred frames WHILE THE CAMERA MOVES, and class each one -
  P either side looks like a pointer            -> never copy
  C differs and the engine rewrites ours too    -> per-view (camera, matrices, timers) -> never copy
  A differs, neither side changes               -> an area setting our view never got   -> copy
  B differs, only the game view's changes       -> engine state ours never gets         -> copy
Moving the camera is what makes the test valid: it puts every camera-derived field into class C.

The measurement afterwards is the honest one: put BOTH views on the same camera and compare their own lit
pictures. Identical geometry (G-buffer difference 0.00) and a lit difference of 69.8 is where we started.

Usage: python run_rtclass.py            (the game must already be in gameplay)
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVE = os.path.join(HERE, "ds_drive.py")


def _mod():
    from importlib import util
    spec = util.spec_from_file_location("ds_drive", DRIVE)
    m = util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def drive(*a):
    return subprocess.run([sys.executable, DRIVE, *a], capture_output=True, text=True).stdout.strip()


def wait_line(m, rx, since, timeout=90.0):
    pat = re.compile(rx)
    t0 = time.time()
    while time.time() - t0 < timeout:
        lines = m.log_lines()
        for l in lines[since:]:
            if pat.search(l):
                return l
        time.sleep(0.4)
    return None


def measure(m, label, path):
    """Capture both views' own picture at `path` and report how far apart they are."""
    drive("cmd", "ds picpath %s" % path)
    time.sleep(2)
    drive("cmd", "ds capture")
    time.sleep(5)
    import glob
    import numpy as np
    from PIL import Image
    b = os.path.join(m.DATA, "captures")
    d = sorted(glob.glob(os.path.join(b, "2026*")))[-1]
    try:
        a = np.asarray(Image.open(os.path.join(d, "view_main.bmp")).convert("L"), dtype=np.float32)
        c = np.asarray(Image.open(os.path.join(d, "view_second.bmp")).convert("L"), dtype=np.float32)
    except Exception as e:
        print("   %s: could not read the captures (%s)" % (label, e))
        return None
    h = a.shape[0]
    diff = float(np.mean(np.abs(a - c)))
    ground = float(np.mean(np.abs(a[2 * h // 3:] - c[2 * h // 3:])))
    print("   %-34s game view %.1f | our view %.1f | difference %.2f (ground %.2f)"
          % (label, a.mean(), c.mean(), diff, ground))
    return diff


def main():
    m = _mod()
    # One command per write: the pump takes a single line at a time, and a two-command line was silently
    # dropping the first (measured 2026-10-30).
    drive("cmd", "ds 3d 1")
    time.sleep(8)
    drive("cmd", "ds eyesep 0")                # SAME camera for both views: any difference left is ours
    time.sleep(3)
    print("before:")
    measure(m, "geometry (G-buffer)", "rt+318+40+18")
    before = measure(m, "after lighting (HDR)", "rt+498+40+18")

    n = len(m.log_lines())
    drive("cmd", "ds rtclass 600")
    print("learning for 600 frames - walking the camera about while it runs")
    t0 = time.time()
    while time.time() - t0 < 40:
        for k in ("w", "a", "s", "d"):
            drive("hold", k, "900")
        if any("rtclass: classified" in l or "rtclass: done" in l for l in m.log_lines()[n:]):
            break
    line = wait_line(m, r"rtclass:", n, timeout=60)
    for l in m.log_lines()[n:]:
        if "rtclass" in l:
            print("   " + l.split("]", 1)[-1].strip())

    drive("cmd", "ds rtclass apply")
    time.sleep(6)
    print("after the learned lighting is copied:")
    measure(m, "geometry (G-buffer)", "rt+318+40+18")
    after = measure(m, "after lighting (HDR)", "rt+498+40+18")
    if before and after:
        print("lit difference %.2f -> %.2f" % (before, after))
    return 0


if __name__ == "__main__":
    sys.exit(main())
