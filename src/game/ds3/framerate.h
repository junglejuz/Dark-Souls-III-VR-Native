#pragma once
// DS3VR2 frame rate (M5) - Dark Souls III's 60 fps cap lifted through the game's OWN debug cap: the frame-timer
// object's byte +0x358 turns on an override period of 1 / float +0x354. No code is patched (v1 DS3, STATIC; the same
// method as the public DS3DebugFPS). Live: settings fpscap, panel FRAME RATE, command `ds fps N`.
// Caveat kept honest in the panel: above about 80 fps the game's own running / rolling speed drifts. That is a
// Dark Souls III bug and Sekiro's speed-table fix has no counterpart in DS3's readable code (checked 2026-10-30).

namespace bvr::ds3::framerate {

void install();          // pattern scan + apply the saved cap (adapter init)
void tick();             // once per Present: re-assert the two cap fields
void set_cap(int fps);   // 0 = match the headset's refresh (the default), 30..240 = a fixed rate, 60 = the game's own
int  cap();
const char* status();
void draw_ui();

} // namespace bvr::ds3::framerate
