"""DS3VR2 V11 marker test on the fake headset (JJ away, 2026-10-31): into the game, lock on to the nearest enemy, record the
markers (mkdump + capture) in native and in AFW, release the lock, close. ~20 s in the world so the enemy has little time.
Usage: python ds3vr2/tools/run_markers.py      (leaves no game running; settings restored by ds_drive stop)"""
import os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
DRIVE = os.path.join(HERE, "ds_drive.py")
LOG = r"E:\Games\Steam\steamapps\common\DARK SOULS III\Game\DS3VR2\ds3vr2_log.txt"


def drive(*a, timeout=300):
    r = subprocess.run([sys.executable, DRIVE, *a], capture_output=True, text=True, timeout=timeout)
    return (r.stdout + r.stderr).strip()


def log_has(text, since):
    try:
        return any(text in l for l in open(LOG, encoding="utf-8", errors="replace").read().splitlines()[since:])
    except OSError:
        return False


import re
def in_gameplay():
    # native lists our second view; AFW never does - both feed the eyes (ownview heartbeat "eyes fed L <n>", n > 0)
    try:
        return any(re.search(r"eyes fed L [1-9]", l) for l in open(LOG, encoding="utf-8", errors="replace").read().splitlines())
    except OSError:
        return False


def log_len():
    try:
        return len(open(LOG, encoding="utf-8", errors="replace").read().splitlines())
    except OSError:
        return 0


def first_line():
    try:
        with open(LOG, encoding="utf-8", errors="replace") as f:
            return f.readline()
    except OSError:
        return ""


old_first = first_line()
print(drive("start"))
t0 = time.time()
while time.time() - t0 < 90 and first_line() == old_first:   # the NEW session's log (the old one still holds JJ's lines)
    time.sleep(1)
subprocess.run([sys.executable, os.path.join(HERE, "to_game.py"), "240"], capture_output=True, text=True, timeout=300)
# gameplay = our second view listed (the stereo heartbeat), whatever to_game.py concluded
t0 = time.time()
while time.time() - t0 < 120 and not in_gameplay():
    time.sleep(1)
if not in_gameplay():
    print("never reached gameplay - stopping"); print(drive("stop")); sys.exit(1)
time.sleep(4)
print(drive("shot", os.path.join(HERE, "out", "mk_before.png")))
n = log_len()
TEST = len(sys.argv) > 1 and sys.argv[1] == "test"
if TEST:   # MKTEST: test markers over the player's own HUD (no enemy needed): bars (plain shaders), item slots and the
    # souls counter (batched shaders), one capture each, then the AFW part below with the slots point
    for xy in ("-0.75 0.40", "-0.72 -0.30", "0.78 -0.44"):
        print(drive("cmd", "ds mktest " + xy + " 5"))
        time.sleep(1.5)
        print(drive("cmd", "ds mkdump 1;ds capture;ds mkinfo"))
        time.sleep(2)
    print(drive("cmd", "ds mktest -0.72 -0.30 5"))
else:
    print(drive("key", "q"))                    # lock on
time.sleep(1.5)
print(drive("cmd", "ds mkdump 2;ds capture"))
time.sleep(3)
print(drive("cmd", "ds afw off"))
time.sleep(3)
print(drive("cmd", "ds mkdump 2;ds capture"))
time.sleep(3)
print(drive("cmd", "ds afw on"))
time.sleep(3)
print(drive("cmd", "ds mkdump 1;ds capture"))
time.sleep(3)
print(drive("cmd", "ds afw status;ds markers 1"))   # JJ keeps AFW on (afw=1)
time.sleep(1)
if TEST:
    print(drive("cmd", "ds mktest 0 0 0"))
else:
    print(drive("key", "q"))                    # release the lock
time.sleep(1)
print(drive("shot", os.path.join(HERE, "out", "mk_after.png")))
print(drive("stop"))
print("log lines this run from", n)
