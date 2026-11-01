"""Drive Dark Souls III from launch to gameplay on the fake headset (JJ away).

Route: title "press any button" -> the server login attempt -> the "failed to log in" box (CANCEL) ->
"starting in offline mode" (OK) -> main menu with Continue highlighted (confirm) -> gameplay.

Everything here REACTS, nothing waits on a guessed delay: the dialog band of the window is sampled four
times a second and CANCEL is pressed the moment the box appears (JJ 2026-10-30: the old version sat on a
fixed six-second sleep and was far too slow to answer it). Gameplay is confirmed from the log.

Usage: python to_game.py [timeout_s]
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVE = os.path.join(HERE, "ds_drive.py")

# The dialog sits across the middle of the window; this band is black on the title screen and on a load,
# and bright as soon as a message box or the main menu is up.
BAND_TOP, BAND_BOTTOM = 0.44, 0.58


def _mod():
    from importlib import util
    spec = util.spec_from_file_location("ds_drive", DRIVE)
    m = util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def screen_brightness(m):
    """(whole window, dialog band) mean brightness, or (-1, -1) if there is no window.

    The pair is what tells a menu from play: the title screen, the login box and the offline notice are all a
    bright band on an otherwise black screen, while a gameplay frame is bright everywhere."""
    b = band_brightness(m)
    if b < 0:
        return -1.0, -1.0
    return _whole_brightness(m), b


def _whole_brightness(m):
    import ctypes
    import ctypes.wintypes as w
    from PIL import ImageGrab
    h = m.game_window()
    if not h:
        return -1.0
    r = w.RECT()
    m.u.GetClientRect(h, ctypes.byref(r))
    p = w.POINT(0, 0)
    m.u.ClientToScreen(h, ctypes.byref(p))
    im = ImageGrab.grab(bbox=(p.x, p.y, p.x + r.right, p.y + r.bottom), all_screens=True).convert("L").resize((48, 32))
    px = list(im.getdata())
    return sum(px) / float(len(px))


def band_brightness(m):
    """Mean brightness of the dialog band of the game window, or -1 if there is no window."""
    import ctypes
    import ctypes.wintypes as w
    from PIL import ImageGrab
    h = m.game_window()
    if not h:
        return -1.0
    r = w.RECT()
    m.u.GetClientRect(h, ctypes.byref(r))
    p = w.POINT(0, 0)
    m.u.ClientToScreen(h, ctypes.byref(p))
    top = p.y + int(r.bottom * BAND_TOP)
    bot = p.y + int(r.bottom * BAND_BOTTOM)
    im = ImageGrab.grab(bbox=(p.x, top, p.x + r.right, bot), all_screens=True).convert("L").resize((64, 16))
    px = list(im.getdata())
    return sum(px) / float(len(px))


def key(m, name):
    subprocess.run([sys.executable, DRIVE, "key", name], capture_output=True, text=True)


def main(timeout=360.0):
    m = _mod()
    t0 = time.time()
    while time.time() - t0 < timeout:
        if any("session state" in l for l in m.log_lines()):
            break
        time.sleep(1)

    # Only lines written SINCE this run started count: the log is appended across launches, so searching the
    # whole file reported "gameplay" from the previous session's line and the driver stopped at the title screen.
    start = len(m.log_lines())

    # SAFETY (JJ 2026-10-30: "why are you just standing there swapping weapons constantly?"). In Dark Souls III
    # the arrow keys swap weapons and items and E is the action button, so a driver that keeps pressing them while
    # the game is actually being PLAYED cycles the player's gear and can use items. The old loop decided "a dialog
    # is up" from "the middle of the window is bright", which is also true of every gameplay frame, so once it
    # missed the gameplay signal it hammered right+confirm for the whole timeout.
    #
    # Two independent stops now, and a key is only ever sent when BOTH agree a menu is up:
    #   1. the log says no engine view is listed (gameplay lists one), and
    #   2. the screen is mostly DARK with a bright middle band - a dialog over black, which is what the title,
    #      the login box and the offline notice all look like, and what a played frame never looks like.
    # Plus a hard budget of presses, so a misread can cost a few keys, never a whole session.
    def in_game():
        if any("count 0 -> 1" in l or "count -1 -> 1" in l for l in m.log_lines()[start:]):
            return True
        for l in reversed(m.log_lines()[start:]):          # the 5 s heartbeat: "views now N"
            k = l.find("views now ")
            if k >= 0:
                try:
                    return int(l[k + 10:].split(",")[0].strip()) >= 1
                except ValueError:
                    return False
        return False

    pressed_title = False
    last_action = 0.0
    presses = 0
    while time.time() - t0 < timeout:
        if in_game():
            print("gameplay: main view listed")
            return 0
        if presses >= 12:
            print("gave up after 12 key presses without reaching gameplay - nothing more will be pressed")
            return 1
        whole, band = screen_brightness(m)
        now = time.time()
        if whole < 0:
            time.sleep(0.5)
            continue
        menu_up = whole < 25.0 and band > 8.0        # a dialog over black, never a played frame
        if now - last_action < 1.2:                  # one action at a time; the menus need a moment to settle
            time.sleep(0.25)
            continue
        if menu_up:
            # CANCEL is to the right of OK on the login box; on the offline notice and the main menu the
            # highlighted item is already the one we want, so right-then-confirm is safe for all three.
            key(m, "right")
            key(m, "e")
            presses += 2
            last_action = time.time()
        elif whole < 25.0 and not pressed_title:
            key(m, "e")                              # title screen: press any button
            pressed_title = True
            presses += 1
            last_action = time.time()
        else:
            time.sleep(0.25)
    print("TIMEOUT: no gameplay")
    return 1


if __name__ == "__main__":
    sys.exit(main(float(sys.argv[1]) if len(sys.argv) > 1 else 360.0))
