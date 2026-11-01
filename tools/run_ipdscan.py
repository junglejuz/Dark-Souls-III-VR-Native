"""M3b bisection: the one run that put TWO pictures in the shared target had the eye separation at 1.6 m
(ipdscale 25). Everything else about it was ordinary. So: step the separation with 3D on and read the
self-check at each step - at which separation does the left eye stop being black?

Usage: python run_ipdscan.py            (the game must already be in gameplay)
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


def cmd(text):
    subprocess.run([sys.executable, DRIVE, "cmd", text], capture_output=True, text=True)


def wait_line(m, rx, since, timeout=25.0):
    pat = re.compile(rx)
    t0 = time.time()
    while time.time() - t0 < timeout:
        lines = m.log_lines()
        for l in lines[since:]:
            if pat.search(l):
                return l, len(lines)
        time.sleep(0.3)
    return None, len(m.log_lines())


def main():
    m = _mod()
    cmd("ds 3d 1")
    time.sleep(6)
    for s in (1.0, 2.0, 4.0, 8.0, 16.0, 25.0):
        n = len(m.log_lines())
        cmd("ds ipd %.2f;ds check" % s)
        line, n = wait_line(m, r"selfcheck .*brightness", n)
        line2, _ = wait_line(m, r"pictures taken after", n)
        print("eye separation x%-5.1f  %s" % (s, line.split("]", 1)[1].strip() if line else "no self-check"))
        if line2:
            print("                     %s" % line2.split("]", 1)[1].strip())
    cmd("ds ipd 1.0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
