"""DS3VR2 test driver for fake-headset runs (JJ away: memory jj-drives-i-record allows it; JJ 2026-10-29 "use the fake
headset to test anything you need to"). Every action is checked by the caller (log line or screenshot).
Usage:
  python ds_drive.py start            back up settings.ini, set runtime_json = fake headset, leave the VR flag, launch Steam
  python ds_drive.py stop             close the game, restore settings.ini (ALWAYS run this at the end)
  python ds_drive.py wait <regex> [timeout_s]   wait for a NEW log line matching regex (prints it)
  python ds_drive.py key <name> [hold_ms]       press a key in the game window (enter, e, q, esc, up, down, left, right, f2, w, a, s, d, space)
  python ds_drive.py hold <name> <ms>           hold a key (walk)
  python ds_drive.py shot <file.png>            screenshot of the game window's client area
  python ds_drive.py cmd "<line>"               write DS3VR2\\command.txt (one or more lines separated by ';')
  python ds_drive.py tail [n]                   last n log lines
"""
import ctypes, ctypes.wintypes as w, os, re, shutil, subprocess, sys, time

GAME = r"E:\Games\Steam\steamapps\common\DARK SOULS III\Game"
DATA = os.path.join(GAME, "DS3VR2")
LOG = os.path.join(DATA, "ds3vr2_log.txt")
INI = os.path.join(DATA, "settings.ini")
BAK = os.path.join(DATA, "settings.ini.before_fake_run")
XRSIM_JSON = os.path.join(os.environ["LOCALAPPDATA"], r"AC6VR\xrsim\ac6vr_xrsim64.json")
u = ctypes.windll.user32
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass

SCAN = {"enter": (0x1C, False), "e": (0x12, False), "q": (0x10, False), "esc": (0x01, False), "up": (0x48, True),
        "down": (0x50, True), "left": (0x4B, True), "right": (0x4D, True), "f2": (0x3C, False), "w": (0x11, False),
        "a": (0x1E, False), "s": (0x1F, False), "d": (0x20, False), "space": (0x39, False), "f": (0x21, False),
        "tab": (0x0F, False), "f10": (0x44, False), "f11": (0x57, False), "f12": (0x58, False),
        "i": (0x17, False), "j": (0x24, False), "k": (0x25, False), "l": (0x26, False)}  # camera up / left / down / right


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", w.WORD), ("wScan", w.WORD), ("dwFlags", w.DWORD), ("time", w.DWORD), ("extra", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", w.DWORD), ("u", _U)]


def game_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq DarkSoulsIII.exe", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for l in out.splitlines():
        if l.lower().startswith('"darksoulsiii.exe'):
            return int(l.split('","')[1])
    return None


def game_window():
    pid = game_pid()
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, w.HWND, w.LPARAM)
    def cb(h, l):
        p = w.DWORD(); u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(h):
            r = w.RECT(); u.GetClientRect(h, ctypes.byref(r))
            if r.right > 200:
                found.append(h)
        return True
    if pid:
        u.EnumWindows(cb, 0)
    return found[0] if found else None


def focus(h):
    for _ in range(6):
        if u.GetForegroundWindow() == h:
            return True
        u.keybd_event(0x12, 0, 0, 0); u.keybd_event(0x12, 0, 2, 0)
        u.ShowWindow(h, 5); u.SetForegroundWindow(h); time.sleep(0.15)
    return u.GetForegroundWindow() == h


def send(scan, ext, up):
    i = INPUT(); i.type = 1; i.ki.wScan = scan
    i.ki.dwFlags = 0x0008 | (0x0001 if ext else 0) | (0x0002 if up else 0)
    u.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))


def key(name, hold_ms=90):
    h = game_window()
    if not h:
        print("no game window"); return 1
    if not focus(h):
        print("could not focus the game window"); return 1
    scan, ext = SCAN[name.lower()]
    send(scan, ext, False); time.sleep(hold_ms / 1000.0); send(scan, ext, True)
    print("key", name); return 0


def shot(path):
    from PIL import ImageGrab
    h = game_window()
    if not h:
        print("no game window"); return 1
    r = w.RECT(); u.GetClientRect(h, ctypes.byref(r))
    p = w.POINT(0, 0); u.ClientToScreen(h, ctypes.byref(p))
    ImageGrab.grab(bbox=(p.x, p.y, p.x + r.right, p.y + r.bottom), all_screens=True).save(path)
    print("shot", path, r.right, r.bottom); return 0


def log_lines():
    try:
        with open(LOG, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def wait(rx, timeout=120.0, start=None):
    pat = re.compile(rx)
    n0 = len(log_lines()) if start is None else start
    t0 = time.time()
    while time.time() - t0 < timeout:
        lines = log_lines()
        if len(lines) < n0:
            n0 = 0  # a new log (the game restarted)
        for l in lines[n0:]:
            if pat.search(l):
                print(l); return 0
        n0 = len(lines)
        time.sleep(0.5)
    print("TIMEOUT waiting for", rx); return 1


def set_ini(key_, value):
    lines = [l for l in open(INI, encoding="utf-8").read().splitlines() if l.strip()] if os.path.exists(INI) else ["[ds3vr2]"]
    if not any(l.strip().lower() == "[ds3vr2]" for l in lines):
        lines.insert(0, "[ds3vr2]")
    lines = [l for l in lines if not l.lower().startswith(key_.lower() + "=")]
    i = next(k for k, l in enumerate(lines) if l.strip().lower() == "[ds3vr2]")
    lines.insert(i + 1, "%s=%s" % (key_, value))
    open(INI, "w", encoding="utf-8", newline="").write("\r\n".join(lines) + "\r\n")


def start():
    if game_pid():
        print("Dark Souls III is already running"); return 1
    if os.path.exists(INI) and not os.path.exists(BAK):
        shutil.copy(INI, BAK)
    elif not os.path.exists(INI):
        open(BAK + ".none", "w").close()
    set_ini("runtime_json", XRSIM_JSON)
    open(os.path.join(DATA, "ds3vr2_launch_vr.flag"), "w").write("vr")
    os.startfile("steam://rungameid/374320")
    print("launched (fake headset)"); return 0


def stop():
    pid = game_pid()
    if pid:
        subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
        time.sleep(3)
    if os.path.exists(BAK):
        # keep what the run learned (pic path etc.) but never the fake runtime, and never the fake headset's size / lens
        # angles (eyesize sizes JJ's next launch from xr_rec_w / xr_rec_h / xr_ang - 2026-10-30 they leaked: 2064x2208)
        cur = [l for l in open(INI, encoding="utf-8").read().splitlines() if l.strip()] if os.path.exists(INI) else []
        own = {l.split("=", 1)[0].lower(): l for l in open(BAK, encoding="utf-8").read().splitlines() if "=" in l}
        headset = ("xr_rec_w", "xr_rec_h", "xr_ang")
        kept = [own.get(l.split("=", 1)[0].lower(), l) if l.split("=", 1)[0].lower() in headset else l
                for l in cur if not l.lower().startswith("runtime_json=")]
        open(INI, "w", encoding="utf-8", newline="").write("\r\n".join(kept) + "\r\n")
        os.remove(BAK)
    elif os.path.exists(BAK + ".none"):
        cur = [l for l in open(INI, encoding="utf-8").read().splitlines() if l.strip()] if os.path.exists(INI) else []
        kept = [l for l in cur if not l.lower().startswith("runtime_json=")]
        open(INI, "w", encoding="utf-8", newline="").write("\r\n".join(kept) + "\r\n")
        os.remove(BAK + ".none")
    flag = os.path.join(DATA, "ds3vr2_launch_vr.flag")
    if os.path.exists(flag):
        os.remove(flag)
    print("stopped; runtime_json removed from settings.ini"); return 0


def to_gameplay(timeout=240.0):
    """From launch to gameplay: wait for the session, then Enter every 3.5 s (title, the 'last ended without Quit Game'
    notice, the information box, Continue) until the main view is listed (log 'count 0 -> 1')."""
    if wait(r"session state FOCUSED", 150.0) != 0:
        return 1
    time.sleep(10)
    t0 = time.time()
    n0 = len(log_lines())
    while time.time() - t0 < timeout:
        lines = log_lines()
        if any(re.search(r"count 0 -> 1", l) for l in lines[n0:]):
            print("gameplay: main view listed"); return 0
        key("enter")
        time.sleep(3.5)
    print("TIMEOUT: no gameplay"); return 1


def cmd(text):
    open(os.path.join(DATA, "command.txt"), "w").write("\r\n".join(s.strip() for s in text.split(";")) + "\r\n")
    print("command:", text); return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    op = a[0]
    if op == "start": sys.exit(start())
    if op == "stop": sys.exit(stop())
    if op == "togame": sys.exit(to_gameplay())
    if op == "wait": sys.exit(wait(a[1], float(a[2]) if len(a) > 2 else 120.0))
    if op == "key": sys.exit(key(a[1], int(a[2]) if len(a) > 2 else 90))
    if op == "hold": sys.exit(key(a[1], int(a[2])))
    if op == "shot": sys.exit(shot(a[1]))
    if op == "cmd": sys.exit(cmd(a[1]))
    if op == "tail":
        for l in log_lines()[-(int(a[1]) if len(a) > 1 else 20):]:
            print(l[:240])
        sys.exit(0)
    print(__doc__); sys.exit(1)
