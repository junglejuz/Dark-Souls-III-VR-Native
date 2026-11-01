"""DS3VR2 one fake-headset run (2026-10-30): launch, start-up boxes, Continue, one quick 180-degree camera turn + small tilt
(mouse_turn.py), then `ds view 0`, `ds sunprobe 3`, a capture, `ds view 1`. Leaves the game RUNNING for more commands.
Usage: python ds3vr2/tools/run_probe.py <tag> <eye_res 0|1>   (eye_res 0 also sets the VR GraphicsConfig to 1440x810)
Writes the sunprobe lines to <this folder>/out/sun_<tag>.txt."""
import importlib.util, os, re, subprocess, sys, time
ROOT = r"D:\Documents\Antigravity\VR_Modding"
DRIVE = os.path.join(ROOT, r"ds3vr2\tools\ds_drive.py")
SCR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
os.makedirs(SCR, exist_ok=True)
HERE = os.path.dirname(os.path.abspath(__file__))
INI = r"E:\Games\Steam\steamapps\common\DARK SOULS III\Game\DS3VR2\settings.ini"
GFX = r"C:\Users\jungl\AppData\Roaming\DarkSoulsIII\GraphicsConfig.xml"
tag, eye_res = sys.argv[1], sys.argv[2]


def drive(*a):
    return subprocess.run([sys.executable, DRIVE, *a], capture_output=True, text=True).stdout


def sleep(s):
    time.sleep(s)


drive("stop")
L = [l for l in open(INI, encoding="utf-8").read().splitlines() if not l.lower().startswith("eye_res=")] + ["eye_res=" + eye_res]
open(INI, "w", encoding="utf-8", newline="").write("\r\n".join(L) + "\r\n")
if eye_res == "0":
    t = open(GFX, "rb").read().decode("utf-16")
    t = re.sub(r"<Resolution-WindowScreenWidth>\d+<", "<Resolution-WindowScreenWidth>1440<", t)
    t = re.sub(r"<Resolution-WindowScreenHeight>\d+<", "<Resolution-WindowScreenHeight>810<", t)
    open(GFX, "wb").write(t.encode("utf-16"))
print(drive("start").strip())
spec = importlib.util.spec_from_file_location("t", os.path.join(ROOT, r"ds3vr2\tools\to_game.py"))
t = importlib.util.module_from_spec(spec); spec.loader.exec_module(t)
m = t._mod()
t0 = time.time(); seen = False; dark = None
while time.time() - t0 < 150:
    w, b = t.screen_brightness(m)
    if w >= 40: seen = True; dark = None
    elif seen:
        dark = dark or time.time()
        if b > 15 and time.time() - dark > 4: break
    sleep(0.25)
print("title after %.0f s" % (time.time() - t0))
sleep(1)
for keys, wait in ((["e"], 4.5), (["e"], 3.5), (["right", "e"], 3.0), (["e"], 3.0), (["e"], 0)):
    for k in keys: drive("key", k)
    sleep(wait)
print(drive("wait", "views now 2", "90").strip()[-60:])
sleep(4)
M = os.path.join(HERE, "mouse_turn.py")
subprocess.run([sys.executable, M, "180", "100", "0"]); subprocess.run([sys.executable, M, "10", "0", "-12"])
sleep(1)
drive("cmd", "ds view 0"); sleep(1.5)
drive("cmd", "ds sunprobe 3"); sleep(1.5)
drive("cmd", "ds capture"); sleep(3)
drive("cmd", "ds view 1")
lines = [l[15:] for l in drive("tail", "800").splitlines() if "sunprobe" in l]
open(os.path.join(SCR, "sun_%s.txt" % tag), "w").write("\n".join(lines))
print(len(lines), "sunprobe lines")
for l in lines:
    if "vertex stream" in l: print(l[:400])
