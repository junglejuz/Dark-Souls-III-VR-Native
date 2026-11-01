#pragma once
// DS3VR2 KEEPFOCUS (V9, 2026-10-31). Dark Souls III slows to 2-6 frames a second the moment its window is not the front
// window (pacetrace.log 00:20:15-00:20:54 and 00:21:12: fg=0, each Present 64-288 ms; back to 87-90 at once with fg=1) -
// in the headset the window is rarely in front (Virtual Desktop, a click elsewhere), so the game "froze" for JJ, AFW
// looked 5 fps. Port of Batman City's KEEP RUNNING UNFOCUSED (batman_city/src/game/arkhamcity/window.cpp, AKVR's before):
// the front-window question answers "the game" for every thread of the process, the active / focus questions for the
// window's own thread, and the window never hears that it lost activation (or is minimised). VR launches only.
#include <windows.h>

namespace bvr::ds3::focus {

void install();                 // the user32 hooks (adapter, after MinHook is up); setting keep_focus (default 1)
void attach(HWND gameWindow);   // the swapchain's window, at the first Present: its messages are filtered from then on
HWND real_foreground();         // the truth, for our own checks
const char* status();

} // namespace bvr::ds3::focus
