#pragma once
// DS3VR2 EYESIZE (M4): the game renders at each eye's own shape and size (VR_ESSENTIALS §3, FAIL-PERF-020) instead of the
// 2048x1152 window. Both engine views share their final targets (PICSPLIT), so the GAME's own render size is set, and
// both views are built at it by the engine itself. v1's proven route (skvr/DS3_PLAN.md TALLRES / RESLIST /
// INTERNALRES, HEADSET 2026-09-27):
//   DllMain: GraphicsConfig.xml -> Windowed at W x H; the windowed resolution list's 3840x2160 slot -> W x H (the game
//            rejects a saved size that is not in its list).
//   first graphics call (code decrypted): the menu's desktop-size filter off, the 16:9 carve-out off (full picture),
//            the internal render size globals found.
//   swapchain: created / resized at W x H whenever the game renders at W x H (the window itself is clamped by the monitor).
// W x H = the runtime's recommended eye size (saved from the last session) reshaped for the lens mode (lens.h) x the
// panel's resolution slider. Changes take effect at the next launch.

namespace bvr::ds3::eyesize {

void dllmain(const wchar_t* dataDir);        // VR launches only, from DllMain (kernel32 + file calls only)
void gate();                                  // the first graphics call (game thread, code decrypted)
void hook_factory(void* factory);             // CreateDXGIFactory's result: size the game's swapchain
void install();
void watch();           // once per Present: log any change of the game's own render size                               // adapter init: resize filter, log what happened at start
bool wanted(unsigned& w, unsigned& h);        // this launch's GAME render size (false = off)
bool full_size(unsigned& w, unsigned& h);     // the eye picture size DLSS reconstructs to (= wanted() with DLSS off)
int dlss_mode();                              // the DLSS mode this launch was sized for (settings.ini `dlss`)
bool internal(unsigned& w, unsigned& h);      // the game's internal render size now
const char* status();
// Saves the runtime's recommended size and eye angles (radians, both eyes) for the next launch's size.
void note_runtime(unsigned recW, unsigned recH, const float ang[2][4]);
int lens_mode();                              // this launch's lens mode (lens.h)

} // namespace bvr::ds3::eyesize
