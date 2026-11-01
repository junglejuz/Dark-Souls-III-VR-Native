#pragma once
// DS3VR2 ENVTIME (L7 2026-10-29): when the engine rebuilds its light-environment slots (0x1401F40B0, once a frame on the game
// side) against when each view's instance blocks read them (0x14019B820, during render recording). The game view's
// envData (the light-set slot written to GBuffer3.a) is scrambled while our view's is right: this times the two.
#include <cstdint>

namespace bvr::ds3::envtime {

bool install();                 // MinHook the two engine functions (queued; the caller applies)
void mark(const char* what);    // a timeline event from elsewhere (render-all begin / end)
void on_present();              // frame boundary: log the frame's timeline while armed
void arm(int frames);           // `ds envtime N`
void render_all_begin();        // render-all starts: count the rebuilds of this frame
void share(int on);
void once(int on);              // ENVONCE: the second view reuses the first view's light environments (the fix)             // ENVSHARE: 1 = only the first rebuild per render-all runs (both eyes share the light environments)

} // namespace bvr::ds3::envtime
